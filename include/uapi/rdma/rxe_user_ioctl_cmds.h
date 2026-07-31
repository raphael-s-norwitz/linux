/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR Linux-OpenIB) */
/*
 * Copyright (c) 2026, rxe CRIU migration. All rights reserved.
 *
 * Driver-private uverbs ioctl namespace for the Soft-RoCE (rxe) device.
 * Object/method/attr ids are scoped to the rxe driver (RDMA_DRIVER_RXE)
 * and never collide with another provider's ids.
 *
 * RXE_IB_OBJECT_MIGRATE carries the rxe arm of the CRIU dump-side
 * choreography. Its methods run on a per-process uverbs fd. Method id +0
 * is FREEZE_DATAPATH; QUERY_QP is +1 and QUERY_CQ is +2. Method id +3
 * (FREEZE_CONTEXT) stays reserved for the in-flight restore slice.
 *
 *   FREEZE_DATAPATH  non-destructively park (freeze=1) or unpark
 *             (freeze=0) a QP's requester/responder tasks so the dumper
 *             can take a consistent PSN/cursor snapshot. Keyed by QP
 *             handle; the QP keeps its IBTA state (typically RTS).
 *
 *   QUERY_QP  dump-side counterpart to UVERBS_METHOD_RESTORE_QP: pack the
 *             full rxe wire state (AV, PSNs, cursors, transport attrs,
 *             ring mmap offsets) into a payload byte-equal to
 *             struct rxe_restore_qp_req so the destination can restore
 *             the QP single-shot, plus the QP's userspace handle (the
 *             async-event cookie, not standard-queryable). cap / qp_type
 *             / qp_state are intentionally NOT emitted -- CRIU sources
 *             those from the standard IB_USER_VERBS_CMD_QUERY_QP verb and
 *             NLDEV. The in-flight SQ/RQ/responder ring images are a
 *             later slice; this method emits only the drained subset.
 *
 *   QUERY_CQ  dump-side counterpart to UVERBS_METHOD_RESTORE_CQ: return a
 *             CQ's ring mmap offset + entry count so the dumper sources
 *             both RESTORE_CQ inputs (the forced vm_pgoff and the cqe)
 *             authoritatively from the kernel, keyed by CQ handle. This
 *             retires the smaps cdev-VMA FIFO crutch the CQ-only path
 *             relied on, which a mixed PD+CQ+QP ufile would otherwise
 *             corrupt (QP rings land in the same FIFO but are sourced via
 *             QUERY_QP, never popped -- so a CQ could pop a QP ring's
 *             offset).
 */
#ifndef RXE_USER_IOCTL_CMDS_H
#define RXE_USER_IOCTL_CMDS_H

#include <linux/types.h>
#include <rdma/ib_user_ioctl_cmds.h>

enum rxe_ib_objects {
	RXE_IB_OBJECT_MIGRATE = (1U << UVERBS_ID_NS_SHIFT),
};

enum rxe_ib_migrate_methods {
	/*
	 * FREEZE_DATAPATH is +0, QUERY_QP +1, QUERY_CQ +2. Method +3
	 * (FREEZE_CONTEXT) stays reserved for the in-flight restore slice.
	 */
	RXE_IB_METHOD_FREEZE_DATAPATH = (1U << UVERBS_ID_NS_SHIFT),
	RXE_IB_METHOD_QUERY_QP = (1U << UVERBS_ID_NS_SHIFT) + 1,
	RXE_IB_METHOD_QUERY_CQ = (1U << UVERBS_ID_NS_SHIFT) + 2,
};

enum rxe_ib_freeze_datapath_attrs {
	RXE_IB_ATTR_FREEZE_DATAPATH_QP_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	RXE_IB_ATTR_FREEZE_DATAPATH_FREEZE,
};

enum rxe_ib_query_qp_attrs {
	RXE_IB_ATTR_QUERY_QP_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	RXE_IB_ATTR_QUERY_QP_RESP_BLOB,
	/*
	 * The QP's userspace async-event cookie (ib_qp_user_handle), which
	 * is not standard-queryable.
	 */
	RXE_IB_ATTR_QUERY_QP_RESP_USER_HANDLE,
	/*
	 * In-flight SQ ring image (optional): the live [consumer, producer)
	 * subspan, round-tripped opaquely into the RESTORE_QP UHW_IN tail.
	 * Byte length is reported in rxe_restore_qp_req::sq_image_bytes;
	 * absent/zero-length for a drained QP. Attr ids +4/+5 (RQ / responder
	 * resources) are added by the following commits.
	 */
	RXE_IB_ATTR_QUERY_QP_RESP_SQ_IMAGE,
	/*
	 * In-flight RQ ring image (optional): the live [consumer, producer)
	 * subspan, sized by rxe_restore_qp_req::rq_image_bytes. Absent for a
	 * drained or SRQ-fed QP. Attr id +5 (responder resources) is added by
	 * the following commit.
	 */
	RXE_IB_ATTR_QUERY_QP_RESP_RQ_IMAGE,
};

enum rxe_ib_query_cq_attrs {
	RXE_IB_ATTR_QUERY_CQ_HANDLE = (1U << UVERBS_ID_NS_SHIFT),
	RXE_IB_ATTR_QUERY_CQ_RESP_BLOB,
	/*
	 * In-flight CQE ring image (optional): a variable-length raw byte
	 * region appended on QUERY_CQ. Length is reported in
	 * rxe_query_cq_resp::cqe_image_bytes. Declared now so the ABI is
	 * stable, but left unfilled (zero-length) until the in-flight CQ
	 * slice; a drained CQ carries no unreaped completions.
	 */
	RXE_IB_ATTR_QUERY_CQ_RESP_CQE_IMAGE,
};

#endif
