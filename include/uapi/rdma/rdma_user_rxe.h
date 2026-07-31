/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR Linux-OpenIB) */
/*
 * Copyright (c) 2016 Mellanox Technologies Ltd. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *	- Redistributions of source code must retain the above
 *	  copyright notice, this list of conditions and the following
 *	  disclaimer.
 *
 *	- Redistributions in binary form must reproduce the above
 *	  copyright notice, this list of conditions and the following
 *	  disclaimer in the documentation and/or other materials
 *	  provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef RDMA_USER_RXE_H
#define RDMA_USER_RXE_H

#include <linux/types.h>
#include <linux/socket.h>
#include <linux/in.h>
#include <linux/in6.h>

enum {
	RXE_NETWORK_TYPE_IPV4 = 1,
	RXE_NETWORK_TYPE_IPV6 = 2,
};

/*
 * Flags accepted by struct rxe_alloc_ucontext_req. Older librxe
 * userspace passes inlen=0 to GET_CONTEXT, in which case the kernel
 * sees req = {0} and behaviour is unchanged.
 *
 * RXE_ALLOC_UCTX_RESTORE_MODE: open the ucontext in CRIU-restore
 * mode. The kernel latches a sticky bit on the resulting rxe_ucontext
 * that the per-driver ib_device_ops.ucontext_is_restore_mode predicate
 * reports to the generic UVERBS_METHOD_RESTORE_<TYPE> dispatchers.
 * See tools/testing/criu_rdma/design/uobject_restore.md.
 */
enum {
	RXE_ALLOC_UCTX_RESTORE_MODE = 1u << 0,
};

struct rxe_alloc_ucontext_req {
	__u32	flags;
	__u32	reserved;
};

union rxe_gid {
	__u8	raw[16];
	struct {
		__be64	subnet_prefix;
		__be64	interface_id;
	} global;
};

struct rxe_global_route {
	union rxe_gid	dgid;
	__u32		flow_label;
	__u8		sgid_index;
	__u8		hop_limit;
	__u8		traffic_class;
};

struct rxe_av {
	__u8			port_num;
	/* From RXE_NETWORK_TYPE_* */
	__u8			network_type;
	__u8			dmac[6];
	struct rxe_global_route	grh;
	union {
		struct sockaddr_in	_sockaddr_in;
		struct sockaddr_in6	_sockaddr_in6;
	} sgid_addr, dgid_addr;
};

struct rxe_send_wr {
	__aligned_u64		wr_id;
	__u32			reserved;
	__u32			opcode;
	__u32			send_flags;
	union {
		__be32		imm_data;
		__u32		invalidate_rkey;
	} ex;
	union {
		struct {
			__aligned_u64 remote_addr;
			__u32	length;
			__u32	rkey;
			__u8	type;
			__u8	level;
		} flush;
		struct {
			__aligned_u64 remote_addr;
			__u32	rkey;
			__u32	reserved;
		} rdma;
		struct {
			__aligned_u64 remote_addr;
			__aligned_u64 compare_add;
			__aligned_u64 swap;
			__u32	rkey;
			__u32	reserved;
		} atomic;
		struct {
			__u32	remote_qpn;
			__u32	remote_qkey;
			__u16	pkey_index;
			__u16	reserved;
			__u32	ah_num;
			__u32	pad[4];
			struct rxe_av av;
		} ud;
		struct {
			__aligned_u64	addr;
			__aligned_u64	length;
			__u32		mr_lkey;
			__u32		mw_rkey;
			__u32		rkey;
			__u32		access;
		} mw;
		/* reg is only used by the kernel and is not part of the uapi */
#ifdef __KERNEL__
		struct {
			union {
				struct ib_mr *mr;
				__aligned_u64 reserved;
			};
			__u32	     key;
			__u32	     access;
		} reg;
#endif
	} wr;
};

struct rxe_sge {
	__aligned_u64 addr;
	__u32	length;
	__u32	lkey;
};

struct mminfo {
	__aligned_u64		offset;
	__u32			size;
	__u32			pad;
};

struct rxe_dma_info {
	__u32			length;
	__u32			resid;
	__u32			cur_sge;
	__u32			num_sge;
	__u32			sge_offset;
	__u32			reserved;
	union {
		__DECLARE_FLEX_ARRAY(__u8, inline_data);
		__DECLARE_FLEX_ARRAY(__u8, atomic_wr);
		__DECLARE_FLEX_ARRAY(struct rxe_sge, sge);
	};
};

struct rxe_send_wqe {
	struct rxe_send_wr	wr;
	__u32			status;
	__u32			state;
	__aligned_u64		iova;
	__u32			mask;
	__u32			first_psn;
	__u32			last_psn;
	__u32			ack_length;
	__u32			ssn;
	__u32			has_rd_atomic;
	struct rxe_dma_info	dma;
};

struct rxe_recv_wqe {
	__aligned_u64		wr_id;
	__u32			reserved;
	__u32			padding;
	struct rxe_dma_info	dma;
};

struct rxe_create_ah_resp {
	__u32 ah_num;
	__u32 reserved;
};

struct rxe_create_cq_resp {
	struct mminfo mi;
};

struct rxe_resize_cq_resp {
	struct mminfo mi;
};

/*
 * Driver-private payload for UVERBS_METHOD_RESTORE_CQ on rxe.
 *
 * Passed via the UVERBS_ATTR_UHW_IN tail of the restore-cq method.
 * When @vm_pgoff is non-zero, rxe binds the new CQ's mmap region at
 * exactly that source-side offset so userspace can mmap() the
 * dumped-VMA-pgoff against the destination CQ. When absent (UHW_IN
 * not provided) or zero, rxe falls back to its monotonic counter --
 * identical to the rxe_create_cq path.
 *
 * The dumper captures the source-side rxe_create_cq_resp::mi.offset
 * and replays it here on the destination. rxe rejects -EEXIST if a
 * sibling CQ on the destination has already claimed the same offset
 * (e.g. two concurrent restores of CQs that legitimately collided
 * across hosts, or a buggy dumper). On success, the destination's
 * rxe_create_cq_resp::mi.offset returned via UHW_OUT equals
 * @vm_pgoff.
 *
 * In-flight CQ ring round-trip (mirrors rxe_restore_qp_req + SQ image).
 * A CQ ring is a shared cdev-file VMA that CRIU does not snapshot, so the
 * unreaped CQEs and the producer/consumer cursors present at checkpoint
 * are otherwise lost (post-restore ibv_poll_cq returns nothing and a
 * polling client live-locks). The dumper sources these from QUERY_CQ and
 * replays them here:
 *   @producer / @consumer  the source ring cursors. The user CQ ring is
 *       QUEUE_TYPE_TO_CLIENT, so the producer is kernel-owned (q->index,
 *       mirrored to buf->producer_index) and the consumer is client-owned
 *       (buf->consumer_index) -- the opposite ownership to the SQ/RQ. The
 *       destination seeds them via rxe_cq_seed_ring() (NOT rxe_qp_seed_ring,
 *       which seeds q->index from the consumer and would make the next
 *       rxe_cq_post clobber slot 0).
 *   @cqe_image_bytes  byte length of the in-flight [consumer, producer)
 *       CQE image appended to the UHW_IN tail after this fixed struct,
 *       exactly like rxe_restore_qp_req + SQ image. Zero => empty-CQ fast
 *       path (no blit). The destination validates it against the
 *       freshly-created ring geometry (-EINVAL on mismatch).
 *
 * Size note: must stay strictly larger than sizeof(__u64) (== 8B).
 * The uverbs ioctl bundle treats UHW_IN attrs with len <= 8 as
 * inline (the kernel reuses the bundle's data slot itself as the
 * inbuf), which clobbers @vm_pgoff with whatever value userspace
 * happened to put in struct ib_uverbs_attr::data (a pointer to
 * this struct, in the natural calling convention). The struct is
 * sized > 8 so the dispatcher takes the pointer path and
 * copy_from_user reads the real userspace buffer. @reserved must be
 * 0 and backs forward-compat fields.
 */
struct rxe_restore_cq_req {
	__aligned_u64 vm_pgoff;
	__u32 producer;
	__u32 consumer;
	__u32 cqe_image_bytes;
	__u32 reserved;
};

/*
 * Payload of RXE_IB_ATTR_QUERY_CQ_RESP_BLOB (RXE_IB_METHOD_QUERY_CQ).
 *
 * The dump-side counterpart to struct rxe_restore_cq_req: @vm_pgoff is
 * the CQ ring's mmap byte offset (cq->queue->ip->info.offset) that the
 * dumper replays into RESTORE_CQ, and @cqe is the user-visible entry
 * count. @producer / @consumer are the live ring cursors
 * (QUEUE_TYPE_TO_CLIENT: producer == q->index, consumer ==
 * buf->consumer_index), replayed into rxe_restore_cq_req so the restored
 * ring's unreaped completions are visible to ibv_poll_cq.
 * @cqe_image_bytes is the byte length of the in-flight [consumer,
 * producer) CQE image emitted in the optional
 * RXE_IB_ATTR_QUERY_CQ_RESP_CQE_IMAGE attr (0 for a drained CQ).
 * @reserved must be 0 and is available for future fields.
 */
struct rxe_query_cq_resp {
	__aligned_u64 vm_pgoff;
	__u32 cqe;
	__u32 producer;
	__u32 consumer;
	__u32 cqe_image_bytes;
	__u32 reserved[2];
};

struct rxe_create_qp_resp {
	struct mminfo rq_mi;
	struct mminfo sq_mi;
};

/*
 * Driver-private QP wire-state payload, shared by the dump-side
 * RXE_IB_METHOD_QUERY_QP (which emits it) and UVERBS_METHOD_RESTORE_QP
 * (which consumes it via the UHW_IN tail).
 *
 * Unlike the FW-backed mlx5 path -- where LOAD_VHCA_STATE preserves the
 * entire QPC, so the mlx5 UHW only carries userspace VAs and the qpn --
 * rxe has no firmware. Every byte of wire-relevant QP state that the
 * destination cannot re-derive from the generic RESTORE_QP method attrs
 * (cap / type / state / create_flags / pd / cqs) must travel in this
 * blob and be stamped directly by rxe_restore_qp. This is the rxe
 * mirror of "mlx5 sources it from FW": same logical QP state, different
 * backing store per driver. The restore verb is single-shot -- it lands
 * the QP directly at its captured final state with no kernel-side
 * ib_modify_qp chain and no userspace modify replay.
 *
 * Fields fall into three groups.
 *
 * Identity / mmap (always meaningful):
 *   @qpn  the source QP number. rxe qpns are wire-visible (BTH DestQP),
 *       so -- like the rxe MR lkey/rkey identity contract -- the
 *       restored QP MUST reclaim the same qpn or the peer's in-flight
 *       packets address a stranger. Installed via
 *       rxe_add_to_pool_at_index(qp_pool, qpn); -EBUSY on collision.
 *   @sq_vm_pgoff / @rq_vm_pgoff  source-side mmap offsets of the SQ / RQ
 *       rings (from rxe_create_qp_resp::{sq,rq}_mi.offset). Non-zero =>
 *       bind the restored rings at these exact offsets so the dumped
 *       VMAs map back 1:1; zero => monotonic-counter fallback (probe /
 *       pgoff-agnostic restore). Mirrors rxe_restore_cq_req::vm_pgoff.
 *
 * ib_qp_attr-class wire state (meaningful for RTR/RTS; RESET/INIT emit
 * zeroes and rxe_restore_qp skips whatever the state didn't reach):
 *   @av  primary address vector (qp->pri_av). Byte-identical to the
 *       struct rxe_av the kernel stores; memcpy'd into place. Carries
 *       dgid / dmac / sgid_index / network_type / port etc.
 *   @dest_qp_num  remote QPN (qp->attr.dest_qp_num).
 *   @qkey  Q_Key (qp->attr.qkey; UD).
 *   @sq_psn / @rq_psn  the modify-time PSN bases (qp->attr.{sq,rq}_psn),
 *       echoed back by ibv_query_qp.
 *   @qp_access_flags  qp->attr.qp_access_flags.
 *   @max_rd_atomic / @max_dest_rd_atomic  outstanding RDMA/atomic depths.
 *   @pkey_index  qp->attr.pkey_index.
 *   @path_mtu  IB MTU enum (qp->attr.path_mtu); drives qp->mtu.
 *   @retry_cnt / @rnr_retry / @min_rnr_timer / @timeout  RC reliability
 *       knobs; @timeout drives qp->qp_timeout_jiffies.
 *   @port_num  qp->attr.port_num.
 *   @sq_sig_all  create-time send completion policy (qp->sq_sig_type:
 *       1 => IB_SIGNAL_ALL_WR, 0 => IB_SIGNAL_REQ_WR). Carried here
 *       because the generic RESTORE_QP method exposes only
 *       create_flags, not the legacy sq_sig_all bit.
 *
 * Internal cursors ib_modify_qp cannot express -- the precise reason a
 * create+modify replay cannot faithfully restore an in-flight QP:
 *   @req_psn   next PSN the requester will send (qp->req.psn).
 *   @comp_psn  next PSN the completer expects ACKed (qp->comp.psn).
 *   @resp_psn  next request PSN the responder expects (qp->resp.psn).
 *   @resp_msn  responder message sequence number (qp->resp.msn).
 *   @req_wqe_index  requester's SQ consumer cursor (qp->req.wqe_index).
 *   @ssn  send sequence number (qp->ssn).
 *
 * In-flight (non-drained) datapath state. A QP frozen (not drained) at
 * the snapshot point may have posted-but-unsent / sent-but-unacked SQ
 * work, pre-posted RQ buffers, and RC responder replay resources. The
 * fixed header carries the cursors + responder scalars; the
 * variable-length ring/resource byte images travel out-of-band (QUERY_QP
 * image attrs; RESTORE_QP UHW_IN tail) and are located by the
 * @*_image_bytes counts. All-zero here (and zero-length images) means a
 * drained/idle QP -- the cursor-only restore path. QUERY_QP leaves this
 * whole group zero until the in-flight QP slice lands; a drained QP
 * needs only the groups above.
 *   @sq_producer / @sq_consumer  SQ ring shared-page indices. Together
 *       with @req_wqe_index they bracket the three SQ regions
 *       (unsent / unacked / retired); restore rewinds the requester to
 *       @sq_consumer and replays [@sq_consumer, @sq_producer).
 *   @rq_producer / @rq_consumer  RQ ring indices (pre-posted recv WQEs).
 *   @resp_ack_psn / @resp_opcode / @resp_status / @resp_aeth_syndrome
 *       responder scalars not already covered by @resp_psn / @resp_msn.
 *   @res_head / @res_tail  RC responder-resources ring cursors.
 *   @sq_image_bytes / @rq_image_bytes / @res_image_bytes  byte lengths of
 *       the SQ slot region, RQ slot region, and responder-resources array
 *       images. Zero => that image is absent (drained ring / UD-UC with no
 *       responder array / SRQ-backed RQ, out of scope). The destination
 *       validates each against the freshly-created ring/array geometry.
 *
 * Size note: well over the 8-byte inline-attr threshold (see
 * rxe_restore_cq_req), so the uverbs dispatcher always takes the
 * copy_from_user pointer path. @reserved* must be 0 and back
 * forward-compat fields.
 */
struct rxe_restore_qp_req {
	struct rxe_av	av;
	__aligned_u64	sq_vm_pgoff;
	__aligned_u64	rq_vm_pgoff;
	__u32		qpn;
	__u32		dest_qp_num;
	__u32		qkey;
	__u32		sq_psn;
	__u32		rq_psn;
	__u32		qp_access_flags;
	__u32		max_rd_atomic;
	__u32		max_dest_rd_atomic;
	__u32		req_psn;
	__u32		comp_psn;
	__u32		resp_psn;
	__u32		resp_msn;
	__u32		req_wqe_index;
	__u32		ssn;
	__u16		pkey_index;
	__u8		path_mtu;
	__u8		retry_cnt;
	__u8		rnr_retry;
	__u8		min_rnr_timer;
	__u8		timeout;
	__u8		port_num;
	__u8		sq_sig_all;
	__u8		resp_aeth_syndrome;	/* qp->resp.aeth_syndrome */
	__u16		reserved;
	__u32		sq_producer;		/* SQ buf->producer_index */
	__u32		sq_consumer;		/* SQ buf->consumer_index */
	__u32		rq_producer;		/* RQ buf->producer_index */
	__u32		rq_consumer;		/* RQ buf->consumer_index */
	__u32		resp_ack_psn;		/* qp->resp.ack_psn */
	__s32		resp_opcode;		/* qp->resp.opcode (-1 idle) */
	__u32		resp_status;		/* qp->resp.status (ib_wc_status) */
	__u32		res_head;		/* qp->resp.res_head */
	__u32		res_tail;		/* qp->resp.res_tail */
	__u32		sq_image_bytes;		/* SQ slot region byte count */
	__u32		rq_image_bytes;		/* RQ slot region byte count */
	__u32		res_image_bytes;	/* responder-resources byte count */
	__aligned_u64	reserved2;
};

struct rxe_create_srq_resp {
	struct mminfo mi;
	__u32 srq_num;
	__u32 reserved;
};

struct rxe_modify_srq_cmd {
	__aligned_u64 mmap_info_addr;
};

/* This data structure is stored at the base of work and
 * completion queues shared between user space and kernel space.
 * It contains the producer and consumer indices. Is also
 * contains a copy of the queue size parameters for user space
 * to use but the kernel must use the parameters in the
 * rxe_queue struct. For performance reasons arrange to have
 * producer and consumer indices in separate cache lines
 * the kernel should always mask the indices to avoid accessing
 * memory outside of the data area
 */
struct rxe_queue_buf {
	__u32			log2_elem_size;
	__u32			index_mask;
	__u32			pad_1[30];
	__u32			producer_index;
	__u32			pad_2[31];
	__u32			consumer_index;
	__u32			pad_3[31];
	__u8			data[];
};

#endif /* RDMA_USER_RXE_H */
