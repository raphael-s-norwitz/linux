/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR Linux-OpenIB) */
/*
 * Copyright (c) 2026, rxe CRIU migration. All rights reserved.
 *
 * Driver-private uverbs ioctl namespace for the Soft-RoCE (rxe) device.
 * Object/method/attr ids are scoped to the rxe driver (RDMA_DRIVER_RXE)
 * and never collide with another provider's ids.
 *
 * RXE_IB_OBJECT_MIGRATE carries the rxe arm of the CRIU dump-side
 * choreography. Its methods run on a per-process uverbs fd. This slice
 * introduces the object with its first method, QUERY_CQ; method ids +0/+1
 * are reserved for FREEZE_DATAPATH / QUERY_QP, which land with the QP
 * migration slice, so QUERY_CQ is pinned to +2 and its id stays stable.
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
	 * +0 / +1 are reserved for RXE_IB_METHOD_FREEZE_DATAPATH and
	 * RXE_IB_METHOD_QUERY_QP, which land with the QP migration slice.
	 * QUERY_CQ is pinned to +2 so its id is stable when they arrive.
	 */
	RXE_IB_METHOD_QUERY_CQ = (1U << UVERBS_ID_NS_SHIFT) + 2,
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
