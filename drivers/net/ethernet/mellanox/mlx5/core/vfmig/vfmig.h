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
 * Probe-time restore hooks, called from the VF's mlx5_function_enable().
 * mlx5_vfmig_vf_consume_restored() test-and-clears the "restored" latch
 * (returning the staged vhca_id) so the probe can skip INIT_HCA;
 * mlx5_vfmig_vf_apply_pending_load() applies any staged LOAD_VHCA_STATE
 * blob for the VF (suspend -> LOAD -> resume) via its PF mdev.
 */
bool mlx5_vfmig_vf_consume_restored(struct mlx5_core_dev *vf_dev,
				    u16 *vhca_id_out);
int  mlx5_vfmig_vf_apply_pending_load(struct mlx5_core_dev *vf_dev);

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

static inline bool mlx5_vfmig_vf_consume_restored(struct mlx5_core_dev *vf_dev,
						  u16 *vhca_id_out)
{
	return false;
}

static inline int mlx5_vfmig_vf_apply_pending_load(struct mlx5_core_dev *vf_dev)
{
	return 0;
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
