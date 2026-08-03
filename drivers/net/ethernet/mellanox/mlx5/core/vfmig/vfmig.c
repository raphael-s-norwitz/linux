// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/* Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved. */

/*
 * mlx5 host-driver-side VF migration / CRIU restore - control plane.
 * See vfmig.h for the architecture overview.
 *
 * This patch wires up the module-wide char-device region and device
 * class that per-PF cdevs will hang off of. Subsequent patches add the
 * per-PF cdev and file ops, and the SAVE / LOAD machinery on top.
 */

#include <linux/device/class.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/mlx5/driver.h>

#include "mlx5_core.h"
#include "vfmig.h"

/* Char-device major/minor range shared by all per-PF vfmig cdevs. */
#define MLX5_VFMIG_MAX_DEVICES 256

static dev_t mlx5_vfmig_devt;
static struct class *mlx5_vfmig_class;

int mlx5_vfmig_module_init(void)
{
	int err;

	err = alloc_chrdev_region(&mlx5_vfmig_devt, 0,
				  MLX5_VFMIG_MAX_DEVICES, "mlx5_vfmig");
	if (err)
		return err;

	mlx5_vfmig_class = class_create("mlx5_vfmig");
	if (IS_ERR(mlx5_vfmig_class)) {
		err = PTR_ERR(mlx5_vfmig_class);
		goto err_class;
	}

	return 0;

err_class:
	unregister_chrdev_region(mlx5_vfmig_devt, MLX5_VFMIG_MAX_DEVICES);
	return err;
}

void mlx5_vfmig_module_exit(void)
{
	class_destroy(mlx5_vfmig_class);
	unregister_chrdev_region(mlx5_vfmig_devt, MLX5_VFMIG_MAX_DEVICES);
}
