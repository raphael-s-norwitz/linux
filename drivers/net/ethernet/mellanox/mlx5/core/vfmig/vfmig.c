// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/* Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved. */

/*
 * mlx5 host-driver-side VF migration / CRIU restore - control plane.
 * See vfmig.h for the architecture overview.
 *
 * This first patch is only the skeleton: the module-wide init/exit
 * entry points that main.c calls, with no cdev region wired up yet.
 * Subsequent patches add the char-device region, the per-PF cdev and
 * file ops, and the SAVE / LOAD machinery on top.
 */

#include <linux/mlx5/driver.h>

#include "mlx5_core.h"
#include "vfmig.h"

int mlx5_vfmig_module_init(void)
{
	return 0;
}

void mlx5_vfmig_module_exit(void)
{
}
