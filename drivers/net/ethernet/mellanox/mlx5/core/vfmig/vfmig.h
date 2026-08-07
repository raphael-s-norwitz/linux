/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/* Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved. */

/*
 * Host-driver-side VF migration / CRIU restore support for mlx5_core.
 *
 * Goal
 * ----
 * Provide an in-driver SAVE+LOAD plumbing for mlx5 VFs, modelled on the
 * VFIO mlx5 variant driver (drivers/vfio/pci/mlx5/) but driven from the
 * host's PF mlx5_core via a per-PF cdev. Intended consumer: a CRIU-style
 * checkpoint/restore agent that snapshots a running RDMA workload's VHCA
 * state on one provisioning of the PF and re-applies it on the next,
 * without going through a guest VM.
 *
 * Architecture
 * ------------
 *   - One char device per PF mlx5_core, /dev/mlx5_vfmig/<pf_bdf>.
 *   - The cdev only exists on PF mdevs.
 *
 * Lifetime
 * --------
 *   mlx5_vfmig_pf_init()      from mlx5_init_one_devl_locked() after the
 *                             PF is fully up.
 *   mlx5_vfmig_pf_cleanup()   from mlx5_uninit_one().
 *
 * When CONFIG_MLX5_VFMIG=n the whole subsystem compiles out: the call
 * sites in main.c resolve to the no-op stubs below, so they stay free of
 * #ifdef sprinkles.
 */

#ifndef __MLX5_CORE_VFMIG_H__
#define __MLX5_CORE_VFMIG_H__

#include <linux/mlx5/driver.h>

struct vfmig_iova_domain;

#ifdef CONFIG_MLX5_VFMIG

/* Per-PF cdev create/destroy; no-ops on VFs. */
int  mlx5_vfmig_pf_init(struct mlx5_core_dev *pf_mdev);
void mlx5_vfmig_pf_cleanup(struct mlx5_core_dev *pf_mdev);

/*
 * Drop staged-but-unconsumed LOAD slots from mlx5_device_disable_sriov():
 * the pending_load array survives an sriov_numvfs cycle, but a slot left
 * staged for a torn-down VF generation must not apply to the next one.
 */
void mlx5_vfmig_pf_drop_pending_loads(struct mlx5_core_dev *pf_mdev);

/*
 * Clear all orchestrator-stamped per-VF UUIDs from the SR-IOV disable
 * path so the "all-zeros after sriov_numvfs=0" SET_VF_UUID lifecycle
 * holds even though the vf_uuid array survives the cycle.
 */
void mlx5_vfmig_pf_drop_vf_uuids(struct mlx5_core_dev *pf_mdev);

/*
 * Free all per-VF IOVA arenas from the SR-IOV disable path, after the
 * VFs are torn down. The iova_dom array survives an sriov_numvfs cycle,
 * so an arena reserved for one VF generation must not leak into the
 * next.
 */
void mlx5_vfmig_pf_drop_iova_domains(struct mlx5_core_dev *pf_mdev);

/*
 * Probe-time restore hooks, called from the VF's mlx5_function_enable().
 * mlx5_vfmig_vf_consume_restored() test-and-clears the "restored" latch
 * (returning the staged vhca_id) so the probe can skip INIT_HCA;
 * mlx5_vfmig_vf_apply_pending_load() applies any staged LOAD_VHCA_STATE
 * blob for the VF (suspend -> LOAD -> resume) via its PF mdev.
 */
bool mlx5_vfmig_vf_consume_restored(struct mlx5_core_dev *vf_dev,
				    u16 *vhca_id_out);
int  mlx5_vfmig_vf_apply_pending_load(struct mlx5_core_dev *vf_dev);

/*
 * Reconstitute the destination VF's mlx5_core page rb-tree
 * (priv->page_root_xa[function=0]) from the per-VF deterministic
 * IOVA domain's FW_PAGE entries, mirroring the
 * alloc_system_page() -> insert_page() path that ran on the source.
 *
 * Without this step, mlx5_reclaim_root_pages() at restored-VF
 * teardown finds an empty rb-tree, returns 0 pages reclaimed, and
 * the IOVA allocator never frees the restored backing pages -- per-
 * VF leak that grows unbounded across bind/unbind cycles. See the
 * function comment in vfmig.c for the full rationale and the
 * symptoms of the missing-import regression.
 *
 * MUST be called between mlx5_cmd_enable() (which initialises
 * priv->page_root_xa) and any FW give/take-pages event on the
 * restored VHCA. Current caller is the restored-VF branch of
 * mlx5_function_enable() in main.c, after
 * mlx5_vfmig_vf_apply_pending_load() returns success.
 *
 * No-op on PFs, on VFs whose cmd ring isn't tracked (vfmig_iova_dom
 * is NULL), and on tracked VFs whose IOVA registry happens to have
 * no FW_PAGE entries (e.g. a SAVE that captured zero FW pages).
 *
 * Returns 0 on success or a negative errno from the first failing
 * mlx5_pages_import_replayed_fw_page() call. Partial inserts are
 * NOT rolled back; mlx5_reclaim_root_pages() at the next teardown
 * frees them via the same vfmig branch.
 */
int  mlx5_vfmig_vf_import_replayed_fw_pages(struct mlx5_core_dev *vf_dev);

/*
 * Return the per-VF deterministic IOVA domain for @vf_dev if its VF was
 * vfmig-tracked (SET_TRACKED { enable=1 }) when probe fired, else NULL.
 * Called from the VF's cmd-ring allocation (cmd.c) to decide whether to
 * route DMA through the vfmig allocator; NULL on PFs and untracked VFs
 * keeps them on the default dma-iommu path. See the implementation for
 * the lifetime contract that makes the unlocked domain read safe.
 */
struct vfmig_iova_domain *
mlx5_vf_get_vfmig_iova_domain(struct mlx5_core_dev *vf_dev);

/* Module init/exit hooks for the cdev region. */
int  mlx5_vfmig_module_init(void);
void mlx5_vfmig_module_exit(void);

#else /* !CONFIG_MLX5_VFMIG */

static inline int mlx5_vfmig_pf_init(struct mlx5_core_dev *pf_mdev)
{
	return 0;
}

static inline void mlx5_vfmig_pf_cleanup(struct mlx5_core_dev *pf_mdev)
{
}

static inline void
mlx5_vfmig_pf_drop_pending_loads(struct mlx5_core_dev *pf_mdev)
{
}

static inline void
mlx5_vfmig_pf_drop_vf_uuids(struct mlx5_core_dev *pf_mdev)
{
}

static inline void
mlx5_vfmig_pf_drop_iova_domains(struct mlx5_core_dev *pf_mdev)
{
}

static inline bool mlx5_vfmig_vf_consume_restored(struct mlx5_core_dev *vf_dev,
						  u16 *vhca_id_out)
{
	return false;
}

static inline int mlx5_vfmig_vf_apply_pending_load(struct mlx5_core_dev *vf_dev)
{
	return 0;
}

static inline int
mlx5_vfmig_vf_import_replayed_fw_pages(struct mlx5_core_dev *vf_dev)
{
	return 0;
}

static inline struct vfmig_iova_domain *
mlx5_vf_get_vfmig_iova_domain(struct mlx5_core_dev *vf_dev)
{
	return NULL;
}

static inline int mlx5_vfmig_module_init(void)
{
	return 0;
}

static inline void mlx5_vfmig_module_exit(void)
{
}

#endif /* CONFIG_MLX5_VFMIG */

#endif /* __MLX5_CORE_VFMIG_H__ */
