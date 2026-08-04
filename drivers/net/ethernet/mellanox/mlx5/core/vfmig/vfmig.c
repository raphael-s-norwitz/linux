// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/* Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved. */

/*
 * mlx5 host-driver-side VF migration / CRIU restore - control plane.
 * See vfmig.h for the architecture overview.
 *
 * This patch adds the per-PF char device that userspace opens to drive
 * migration of that PF's VFs. Each PF mlx5_core gets a cdev at
 * /dev/mlx5_vfmig/<pf-bdf>; VFs are skipped. The ioctl surface is a
 * stub (-ENOTTY) here -- later patches hang the SAVE / LOAD and per-VF
 * tracking commands off it.
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/device/class.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/kdev_t.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <linux/rwsem.h>
#include <linux/slab.h>
#include <linux/mlx5/driver.h>

#include "mlx5_core.h"
#include "vfmig.h"

/* Char-device major/minor range shared by all per-PF vfmig cdevs. */
#define MLX5_VFMIG_MAX_DEVICES 256

static dev_t mlx5_vfmig_devt;
static struct class *mlx5_vfmig_class;
static DEFINE_IDA(mlx5_vfmig_minor_ida);

/**
 * struct mlx5_vfmig_pf - per-PF vfmig control-plane state
 * @kref:    refcount; drops the last reference from an open fd or the
 *           driver-remove path, whichever comes last.
 * @lock:    guards @dead / @pf_mdev against concurrent ioctls.
 * @pf_mdev: owning PF mlx5_core, NULLed on driver remove.
 * @dead:    set once the PF is being removed; ioctls then fail -ENODEV.
 * @cdev:    the /dev/mlx5_vfmig/<bdf> character device.
 * @minor:   minor number allocated from mlx5_vfmig_minor_ida.
 */
struct mlx5_vfmig_pf {
	struct kref		kref;
	struct rw_semaphore	lock;
	struct mlx5_core_dev	*pf_mdev;
	bool			dead;
	struct cdev		cdev;
	int			minor;
};

static void vfmig_pf_release(struct kref *kref)
{
	struct mlx5_vfmig_pf *vfmig =
		container_of(kref, struct mlx5_vfmig_pf, kref);

	ida_free(&mlx5_vfmig_minor_ida, vfmig->minor);
	kfree(vfmig);
}

static void vfmig_pf_get(struct mlx5_vfmig_pf *vfmig)
{
	kref_get(&vfmig->kref);
}

static void vfmig_pf_put(struct mlx5_vfmig_pf *vfmig)
{
	kref_put(&vfmig->kref, vfmig_pf_release);
}

static int vfmig_open(struct inode *inode, struct file *filp)
{
	struct mlx5_vfmig_pf *vfmig =
		container_of(inode->i_cdev, struct mlx5_vfmig_pf, cdev);

	vfmig_pf_get(vfmig);
	filp->private_data = vfmig;

	return 0;
}

static int vfmig_release(struct inode *inode, struct file *filp)
{
	struct mlx5_vfmig_pf *vfmig = filp->private_data;

	vfmig_pf_put(vfmig);

	return 0;
}

static long vfmig_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct mlx5_vfmig_pf *vfmig = filp->private_data;
	long ret;

	down_read(&vfmig->lock);
	if (vfmig->dead)
		ret = -ENODEV;
	else
		ret = -ENOTTY;
	up_read(&vfmig->lock);

	return ret;
}

static const struct file_operations mlx5_vfmig_fops = {
	.owner		= THIS_MODULE,
	.open		= vfmig_open,
	.release	= vfmig_release,
	.unlocked_ioctl	= vfmig_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

int mlx5_vfmig_pf_init(struct mlx5_core_dev *pf_mdev)
{
	struct mlx5_vfmig_pf *vfmig;
	struct device *dev;
	dev_t devt;
	int minor;
	int err;

	if (mlx5_core_is_vf(pf_mdev))
		return 0;

	vfmig = kzalloc(sizeof(*vfmig), GFP_KERNEL);
	if (!vfmig)
		return -ENOMEM;

	kref_init(&vfmig->kref);
	init_rwsem(&vfmig->lock);
	vfmig->pf_mdev = pf_mdev;

	minor = ida_alloc_max(&mlx5_vfmig_minor_ida,
			      MLX5_VFMIG_MAX_DEVICES - 1, GFP_KERNEL);
	if (minor < 0) {
		err = minor;
		goto err_free;
	}
	vfmig->minor = minor;

	devt = MKDEV(MAJOR(mlx5_vfmig_devt), minor);

	cdev_init(&vfmig->cdev, &mlx5_vfmig_fops);
	vfmig->cdev.owner = THIS_MODULE;
	err = cdev_add(&vfmig->cdev, devt, 1);
	if (err)
		goto err_minor;

	dev = device_create(mlx5_vfmig_class, mlx5_core_dma_dev(pf_mdev),
			    devt, vfmig, "mlx5_vfmig!%s",
			    dev_name(mlx5_core_dma_dev(pf_mdev)));
	if (IS_ERR(dev)) {
		err = PTR_ERR(dev);
		goto err_cdev;
	}

	pf_mdev->priv.vfmig = vfmig;

	return 0;

err_cdev:
	cdev_del(&vfmig->cdev);
err_minor:
	ida_free(&mlx5_vfmig_minor_ida, minor);
err_free:
	kfree(vfmig);
	return err;
}

void mlx5_vfmig_pf_cleanup(struct mlx5_core_dev *pf_mdev)
{
	struct mlx5_vfmig_pf *vfmig = pf_mdev->priv.vfmig;
	dev_t devt;

	if (!vfmig)
		return;

	pf_mdev->priv.vfmig = NULL;

	down_write(&vfmig->lock);
	vfmig->dead = true;
	vfmig->pf_mdev = NULL;
	up_write(&vfmig->lock);

	devt = MKDEV(MAJOR(mlx5_vfmig_devt), vfmig->minor);
	device_destroy(mlx5_vfmig_class, devt);
	cdev_del(&vfmig->cdev);

	vfmig_pf_put(vfmig);
}

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
	ida_destroy(&mlx5_vfmig_minor_ida);
}
