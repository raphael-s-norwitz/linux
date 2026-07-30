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
 * Size note: must stay strictly larger than sizeof(__u64) (== 8B).
 * The uverbs ioctl bundle treats UHW_IN attrs with len <= 8 as
 * inline (the kernel reuses the bundle's data slot itself as the
 * inbuf), which clobbers @vm_pgoff with whatever value userspace
 * happened to put in struct ib_uverbs_attr::data (a pointer to
 * this struct, in the natural calling convention). The reserved
 * tail forces sizeof(struct rxe_restore_cq_req) > 8 so the
 * dispatcher takes the pointer path and copy_from_user reads the
 * real userspace buffer. Future fields can claim @reserved[].
 */
struct rxe_restore_cq_req {
	__aligned_u64 vm_pgoff;
	__aligned_u64 reserved;
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
