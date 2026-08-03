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

/* Module init/exit hooks for the cdev region. */
int  mlx5_vfmig_module_init(void);
void mlx5_vfmig_module_exit(void);

#else /* !CONFIG_MLX5_VFMIG */

static inline int mlx5_vfmig_module_init(void)
{
	return 0;
}

static inline void mlx5_vfmig_module_exit(void)
{
}

#endif /* CONFIG_MLX5_VFMIG */

#endif /* __MLX5_CORE_VFMIG_H__ */
