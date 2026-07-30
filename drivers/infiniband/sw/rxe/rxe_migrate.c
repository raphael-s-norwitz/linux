// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * Copyright (c) 2026 rxe CRIU migration. All rights reserved.
 *
 * Driver-private RXE_IB_OBJECT_MIGRATE uverbs object: the rxe arm of the
 * CRIU dump-side choreography. The generic restore path lives in the
 * UVERBS_OBJECT_RESTORE family; this object carries the dump-side queries
 * whose payloads are rxe-private.
 *
 * This slice introduces the object with its first method, QUERY_CQ -- the
 * dump-side counterpart to UVERBS_METHOD_RESTORE_CQ. FREEZE_DATAPATH,
 * QUERY_QP and the in-flight CQE ring image land with the QP migration
 * slice.
 */

#include <rdma/uverbs_ioctl.h>
#include <rdma/uverbs_std_types.h>
#include <rdma/uverbs_types.h>
#include <rdma/rxe_user_ioctl_cmds.h>

#include "rxe.h"
#include "rxe_queue.h"

#define UVERBS_MODULE_NAME rdma_rxe
#include <rdma/uverbs_named_ioctl.h>

static int UVERBS_HANDLER(RXE_IB_METHOD_QUERY_CQ)(
	struct uverbs_attr_bundle *attrs)
{
	struct rxe_query_cq_resp blob = {};
	struct rxe_cq *cq;
	struct ib_cq *ibcq;

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
	 * Snapshot the producer/consumer cursors as read-only telemetry.
	 * cq_lock is an irqsave spinlock and the copy_to_user below can
	 * sleep, so take it only long enough to read the two cursors
	 * coherently, then drop it. In the real CRIU flow the dumpee is
	 * stopped and its feeding QPs are frozen, so the ring is quiescent;
	 * the lock just closes a cross-context producer race.
	 */
	spin_lock_irq(&cq->cq_lock);
	blob.producer = queue_get_producer(cq->queue, cq->queue->type);
	blob.consumer = queue_get_consumer(cq->queue, cq->queue->type);
	spin_unlock_irq(&cq->cq_lock);

	/*
	 * cqe_image_bytes stays 0 and RESP_CQE_IMAGE is left unfilled: the
	 * in-flight [consumer, producer) CQE-image round-trip is the CQ
	 * follow-up slice (the analogue of the QP SQ/RQ in-flight subspan).
	 * A drained CQ carries no unreaped completions, so vm_pgoff + cqe
	 * suffice to restore it.
	 */
	return uverbs_copy_to(attrs, RXE_IB_ATTR_QUERY_CQ_RESP_BLOB,
			      &blob, sizeof(blob));
}

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
	&UVERBS_METHOD(RXE_IB_METHOD_QUERY_CQ));

const struct uapi_definition rxe_migrate_defs[] = {
	UAPI_DEF_CHAIN_OBJ_TREE_NAMED(RXE_IB_OBJECT_MIGRATE),
	{},
};
