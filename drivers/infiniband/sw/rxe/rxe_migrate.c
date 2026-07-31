// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * Copyright (c) 2026 rxe CRIU migration. All rights reserved.
 *
 * Driver-private RXE_IB_OBJECT_MIGRATE uverbs object: the rxe arm of the
 * CRIU dump-side choreography. The generic restore path lives in the
 * UVERBS_OBJECT_RESTORE family; this object carries the dump-side queries
 * whose payloads are rxe-private.
 *
 * The object carries FREEZE_DATAPATH plus the dump-side query verbs
 * QUERY_QP and QUERY_CQ, the counterparts to UVERBS_METHOD_RESTORE_QP /
 * RESTORE_CQ. The query verbs emit only the drained subset of wire
 * state; the in-flight SQ/RQ/CQE ring images land with later slices.
 */

#include <rdma/uverbs_ioctl.h>
#include <rdma/uverbs_std_types.h>
#include <rdma/uverbs_types.h>
#include <rdma/rxe_user_ioctl_cmds.h>

#include "rxe.h"
#include "rxe_queue.h"

#define UVERBS_MODULE_NAME rdma_rxe
#include <rdma/uverbs_named_ioctl.h>

/*
 * Only connected/datagram transports carry the wire state QUERY_QP
 * emits. GSI/SMI and other special QP types are out of scope for CRIU
 * migration.
 */
static int rxe_migrate_chk_qp_type(const struct ib_qp *ibqp)
{
	switch (ibqp->qp_type) {
	case IB_QPT_RC:
	case IB_QPT_UC:
	case IB_QPT_UD:
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

/*
 * FREEZE_DATAPATH: non-destructively park (freeze=1) or unpark
 * (freeze=0) a user QP's datapath so the dumper can take a coherent
 * PSN/cursor snapshot. rxe_qp_pause/resume are idempotent, so a repeated
 * freeze or thaw is harmless.
 */
static int UVERBS_HANDLER(RXE_IB_METHOD_FREEZE_DATAPATH)(
	struct uverbs_attr_bundle *attrs)
{
	struct ib_qp *ibqp;
	struct rxe_qp *qp;
	u8 freeze;
	int err;

	ibqp = uverbs_attr_get_obj(attrs,
				   RXE_IB_ATTR_FREEZE_DATAPATH_QP_HANDLE);
	if (IS_ERR(ibqp))
		return PTR_ERR(ibqp);

	err = rxe_migrate_chk_qp_type(ibqp);
	if (err)
		return err;

	err = uverbs_copy_from(&freeze, attrs,
			       RXE_IB_ATTR_FREEZE_DATAPATH_FREEZE);
	if (err)
		return err;

	qp = to_rqp(ibqp);

	/* Only user QPs carry the datapath CRIU freezes. */
	if (!qp->is_user)
		return -ENXIO;

	if (freeze)
		rxe_qp_pause(qp);
	else
		rxe_qp_resume(qp);

	return 0;
}

/*
 * Emit one optional ring image attr, shipping only the in-flight
 * [consumer, producer) subspan rather than the whole ring. @producer /
 * @consumer are the caller's coherent cursor snapshot; the emitted image
 * agrees byte-for-byte with the cursors carried in the resp blob. A level
 * ring (or a dumper that didn't ask for the attr) is a no-op; a
 * provided-but-too-small buffer is a hard error. Linearizing the live
 * subspan keeps a default ib_send_bw ring under the u16 uverbs attr
 * length that a whole-ring blit would overflow.
 */
static int rxe_query_emit_ring(struct uverbs_attr_bundle *attrs, u16 attr_id,
			       const struct rxe_queue *q,
			       u32 producer, u32 consumer)
{
	u32 count = (producer - consumer) & q->index_mask;
	size_t bytes = (size_t)count << q->log2_elem_size;
	int user_len, ret;
	void *tmp;

	if (!uverbs_attr_is_valid(attrs, attr_id) || bytes == 0)
		return 0;

	user_len = uverbs_attr_get_len(attrs, attr_id);
	if (user_len < 0)
		return 0;
	if ((u32)user_len < bytes)
		return -ENOSPC;

	tmp = kvmalloc(bytes, GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;

	ret = queue_inflight_capture(q, producer, consumer, tmp, bytes);
	if (ret >= 0)
		ret = uverbs_copy_to(attrs, attr_id, tmp, bytes);

	kvfree(tmp);
	return ret;
}

static int UVERBS_HANDLER(RXE_IB_METHOD_QUERY_QP)(
	struct uverbs_attr_bundle *attrs)
{
	struct rxe_restore_qp_req blob = {};
	struct ib_qp *ibqp;
	u64 user_handle;
	struct rxe_qp *qp;
	int err;

	ibqp = uverbs_attr_get_obj(attrs, RXE_IB_ATTR_QUERY_QP_HANDLE);
	if (IS_ERR(ibqp))
		return PTR_ERR(ibqp);

	err = rxe_migrate_chk_qp_type(ibqp);
	if (err)
		return err;

	qp = to_rqp(ibqp);

	/* No user-side wire state to emit for kernel QPs. */
	if (!qp->is_user || !qp->sq.queue)
		return -ENXIO;

	/* Identity + ring mmap offsets. */
	blob.qpn = ibqp->qp_num;
	if (qp->sq.queue->ip)
		blob.sq_vm_pgoff = qp->sq.queue->ip->info.offset;
	if (qp->rq.queue && qp->rq.queue->ip)
		blob.rq_vm_pgoff = qp->rq.queue->ip->info.offset;

	/* ib_qp_attr-class wire state. */
	memcpy(&blob.av, &qp->pri_av, sizeof(blob.av));
	blob.dest_qp_num	= qp->attr.dest_qp_num;
	blob.qkey		= qp->attr.qkey;
	blob.sq_psn		= qp->attr.sq_psn;
	blob.rq_psn		= qp->attr.rq_psn;
	blob.qp_access_flags	= qp->attr.qp_access_flags;
	blob.max_rd_atomic	= qp->attr.max_rd_atomic;
	blob.max_dest_rd_atomic = qp->attr.max_dest_rd_atomic;
	blob.pkey_index		= qp->attr.pkey_index;
	blob.path_mtu		= qp->attr.path_mtu;
	blob.retry_cnt		= qp->attr.retry_cnt;
	blob.rnr_retry		= qp->attr.rnr_retry;
	blob.min_rnr_timer	= qp->attr.min_rnr_timer;
	blob.timeout		= qp->attr.timeout;
	blob.port_num		= qp->attr.port_num;
	blob.sq_sig_all		= (qp->sq_sig_type == IB_SIGNAL_ALL_WR) ? 1 : 0;

	/* Live cursors ib_modify_qp cannot express. */
	blob.req_psn		= qp->req.psn;
	blob.comp_psn		= qp->comp.psn;
	blob.resp_psn		= qp->resp.psn;
	blob.resp_msn		= qp->resp.msn;
	blob.req_wqe_index	= qp->req.wqe_index;
	blob.ssn		= atomic_read(&qp->ssn);

	/*
	 * In-flight SQ/RQ rings: the cursors locate the live
	 * [consumer, producer) work and {sq,rq}_image_bytes is that subspan's
	 * byte length (0 for a drained ring, where producer == consumer). An
	 * SRQ-fed QP has no private receive queue, so its RQ cursors stay
	 * zero. The responder group lands in the next commit and stays zero
	 * here, so the drained dump path is unchanged.
	 */
	blob.sq_producer = queue_get_producer(qp->sq.queue, qp->sq.queue->type);
	blob.sq_consumer = queue_get_consumer(qp->sq.queue, qp->sq.queue->type);
	blob.sq_image_bytes = ((blob.sq_producer - blob.sq_consumer) &
			       qp->sq.queue->index_mask)
			      << qp->sq.queue->log2_elem_size;

	if (qp->rq.queue && !qp->srq) {
		blob.rq_producer = queue_get_producer(qp->rq.queue,
						      qp->rq.queue->type);
		blob.rq_consumer = queue_get_consumer(qp->rq.queue,
						      qp->rq.queue->type);
		blob.rq_image_bytes = ((blob.rq_producer - blob.rq_consumer) &
				       qp->rq.queue->index_mask)
				      << qp->rq.queue->log2_elem_size;
	}

	err = uverbs_copy_to(attrs, RXE_IB_ATTR_QUERY_QP_RESP_BLOB,
			     &blob, sizeof(blob));
	if (err)
		return err;

	/*
	 * The async-event cookie the source's ibv_create_qp recorded on
	 * the QP uobject. Not standard-queryable, so CRIU must preserve it
	 * through the kernel-sourced dump.
	 */
	user_handle = ib_qp_user_handle(ibqp);
	err = uverbs_copy_to(attrs, RXE_IB_ATTR_QUERY_QP_RESP_USER_HANDLE,
			     &user_handle, sizeof(user_handle));
	if (err)
		return err;

	/*
	 * In-flight SQ/RQ ring images (optional PTR_OUT): the live
	 * [consumer, producer) subspans, round-tripped opaquely into the
	 * RESTORE_QP UHW_IN tail. Each is a no-op when its subspan is empty
	 * or the dumper didn't request the attr.
	 */
	err = rxe_query_emit_ring(attrs, RXE_IB_ATTR_QUERY_QP_RESP_SQ_IMAGE,
				  qp->sq.queue, blob.sq_producer,
				  blob.sq_consumer);
	if (err)
		return err;

	if (qp->rq.queue && !qp->srq)
		return rxe_query_emit_ring(attrs,
					   RXE_IB_ATTR_QUERY_QP_RESP_RQ_IMAGE,
					   qp->rq.queue, blob.rq_producer,
					   blob.rq_consumer);

	return 0;
}

static int UVERBS_HANDLER(RXE_IB_METHOD_QUERY_CQ)(
	struct uverbs_attr_bundle *attrs)
{
	struct rxe_query_cq_resp blob = {};
	struct rxe_cq *cq;
	struct ib_cq *ibcq;
	int err;

	ibcq = uverbs_attr_get_obj(attrs, RXE_IB_ATTR_QUERY_CQ_HANDLE);
	if (IS_ERR(ibcq))
		return PTR_ERR(ibcq);

	cq = to_rcq(ibcq);

	/* Kernel-mode CQs have no user mmap ring to round-trip. */
	if (!cq->is_user || !cq->queue || !cq->queue->ip)
		return -ENXIO;

	blob.vm_pgoff = cq->queue->ip->info.offset;
	blob.cqe      = ibcq->cqe;

	/*
	 * Snapshot the producer/consumer cursors coherently. cq_lock is an
	 * irqsave spinlock and the copy_to_user / image emit below fault to
	 * userspace and can sleep, so take it only long enough to read the
	 * two cursors, then drop it. In the real CRIU flow the dumpee is
	 * stopped and its feeding QPs are frozen, so the ring is quiescent
	 * and the post-drop image read is stable; the lock just closes a
	 * cross-context producer race.
	 */
	spin_lock_irq(&cq->cq_lock);
	blob.producer = queue_get_producer(cq->queue, cq->queue->type);
	blob.consumer = queue_get_consumer(cq->queue, cq->queue->type);
	spin_unlock_irq(&cq->cq_lock);

	/*
	 * cqe_image_bytes is the in-flight [consumer, producer) subspan
	 * (unreaped completions), not the whole ring: the whole ring
	 * overflows the u16 uverbs attr length for any non-trivial CQ.
	 */
	blob.cqe_image_bytes = ((blob.producer - blob.consumer) &
				cq->queue->index_mask)
			       << cq->queue->log2_elem_size;

	err = uverbs_copy_to(attrs, RXE_IB_ATTR_QUERY_CQ_RESP_BLOB,
			     &blob, sizeof(blob));
	if (err)
		return err;

	return rxe_query_emit_ring(attrs, RXE_IB_ATTR_QUERY_CQ_RESP_CQE_IMAGE,
				   cq->queue, blob.producer, blob.consumer);
}

DECLARE_UVERBS_NAMED_METHOD(
	RXE_IB_METHOD_FREEZE_DATAPATH,
	UVERBS_ATTR_IDR(RXE_IB_ATTR_FREEZE_DATAPATH_QP_HANDLE,
			UVERBS_OBJECT_QP,
			UVERBS_ACCESS_READ,
			UA_MANDATORY),
	UVERBS_ATTR_PTR_IN(RXE_IB_ATTR_FREEZE_DATAPATH_FREEZE,
			   UVERBS_ATTR_TYPE(u8),
			   UA_MANDATORY));

DECLARE_UVERBS_NAMED_METHOD(
	RXE_IB_METHOD_QUERY_QP,
	UVERBS_ATTR_IDR(RXE_IB_ATTR_QUERY_QP_HANDLE,
			UVERBS_OBJECT_QP,
			UVERBS_ACCESS_READ,
			UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(RXE_IB_ATTR_QUERY_QP_RESP_BLOB,
			    UVERBS_ATTR_TYPE(struct rxe_restore_qp_req),
			    UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(RXE_IB_ATTR_QUERY_QP_RESP_USER_HANDLE,
			    UVERBS_ATTR_TYPE(u64),
			    UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(RXE_IB_ATTR_QUERY_QP_RESP_SQ_IMAGE,
			    UVERBS_ATTR_MIN_SIZE(0),
			    UA_OPTIONAL),
	UVERBS_ATTR_PTR_OUT(RXE_IB_ATTR_QUERY_QP_RESP_RQ_IMAGE,
			    UVERBS_ATTR_MIN_SIZE(0),
			    UA_OPTIONAL));

DECLARE_UVERBS_NAMED_METHOD(
	RXE_IB_METHOD_QUERY_CQ,
	UVERBS_ATTR_IDR(RXE_IB_ATTR_QUERY_CQ_HANDLE,
			UVERBS_OBJECT_CQ,
			UVERBS_ACCESS_READ,
			UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(RXE_IB_ATTR_QUERY_CQ_RESP_BLOB,
			    UVERBS_ATTR_TYPE(struct rxe_query_cq_resp),
			    UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(RXE_IB_ATTR_QUERY_CQ_RESP_CQE_IMAGE,
			    UVERBS_ATTR_MIN_SIZE(0),
			    UA_OPTIONAL));

DECLARE_UVERBS_GLOBAL_METHODS(
	RXE_IB_OBJECT_MIGRATE,
	&UVERBS_METHOD(RXE_IB_METHOD_FREEZE_DATAPATH),
	&UVERBS_METHOD(RXE_IB_METHOD_QUERY_QP),
	&UVERBS_METHOD(RXE_IB_METHOD_QUERY_CQ));

const struct uapi_definition rxe_migrate_defs[] = {
	UAPI_DEF_CHAIN_OBJ_TREE_NAMED(RXE_IB_OBJECT_MIGRATE),
	{},
};
