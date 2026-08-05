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

#include <linux/anon_inodes.h>
#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/device/class.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/idr.h>
#include <linux/kdev_t.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mlx5/device.h>
#include <linux/mlx5/driver.h>
#include <linux/mlx5/vport.h>
#include <uapi/linux/mlx5_vfmig.h>

#include "mlx5_core.h"
#include "vfmig.h"

/* Char-device major/minor range shared by all per-PF vfmig cdevs. */
#define MLX5_VFMIG_MAX_DEVICES 256

static dev_t mlx5_vfmig_devt;
static struct class *mlx5_vfmig_class;
static DEFINE_IDA(mlx5_vfmig_minor_ida);

/*
 * Upper bound on a SAVE snapshot, from the width of save_vhca_state_in.size.
 * A firmware-reported size beyond this is treated as corruption.
 */
#define VFMIG_MAX_SAVE_SIZE \
	(BIT_ULL(__mlx5_bit_sz(save_vhca_state_in, size)) - 1)

/*
 * Wire-format record header prefixing the SAVE stream, byte-compatible
 * with the VFIO mlx5 variant driver's migration header
 * (drivers/vfio/pci/mlx5/cmd.h:mlx5_vf_migration_header) so a saved blob
 * can later be replayed through LOAD_VHCA_STATE.
 */
struct vfmig_wire_header {
	__le64 record_size;
	__le32 flags;
	__le32 tag;
};

#define VFMIG_WIRE_TAG_FW_DATA		0

/**
 * struct mlx5_vfmig_pf - per-PF vfmig control-plane state
 * @kref:    refcount; drops the last reference from an open fd or the
 *           driver-remove path, whichever comes last.
 * @lock:    guards @dead / @pf_mdev against concurrent ioctls.
 * @pf_mdev: owning PF mlx5_core, NULLed on driver remove.
 * @dead:    set once the PF is being removed; ioctls then fail -ENODEV.
 * @cdev:    the /dev/mlx5_vfmig/<bdf> character device.
 * @minor:   minor number allocated from mlx5_vfmig_minor_ida.
 * @max_vfs: size of @restored in bits (PF's VF capacity at init).
 * @restored: per-VF "restored" latch, indexed by SR-IOV VF id. Set via
 *           MARK_RESTORED, read back via QUERY_VF. Accessed with atomic
 *           bitops; NULL when @max_vfs is 0.
 * @ctxs_lock: mutex protecting @save_ctxs / @load_ctxs list mutations.
 * @save_ctxs: open SAVE_VHCA_STATE sessions (struct mlx5_vfmig_save_ctx).
 * @load_ctxs: open LOAD_VHCA_STATE sessions (struct mlx5_vfmig_load_ctx).
 */
struct mlx5_vfmig_pf {
	struct kref		kref;
	struct rw_semaphore	lock;
	struct mlx5_core_dev	*pf_mdev;
	bool			dead;
	struct cdev		cdev;
	int			minor;
	u16			max_vfs;
	unsigned long		*restored;
	struct mutex		ctxs_lock; /* guards save_ctxs/load_ctxs */
	struct list_head	save_ctxs;
	struct list_head	load_ctxs;
};

static void vfmig_pf_release(struct kref *kref)
{
	struct mlx5_vfmig_pf *vfmig =
		container_of(kref, struct mlx5_vfmig_pf, kref);

	WARN_ON(!list_empty(&vfmig->save_ctxs));
	WARN_ON(!list_empty(&vfmig->load_ctxs));
	mutex_destroy(&vfmig->ctxs_lock);
	bitmap_free(vfmig->restored);
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

/* -------- ioctl handlers ------------------------------------------------- */

/*
 * QUERY_HCA_CAP(other_function=1) - PF-side query of a VF's vhca_id.
 * Mirrors mlx5vf_cmd_get_vhca_id() in drivers/vfio/pci/mlx5/cmd.c.
 */
static int vfmig_query_vhca_id(struct mlx5_core_dev *pf_mdev,
			       u16 function_id, u16 *vhca_id)
{
	u32 in[MLX5_ST_SZ_DW(query_hca_cap_in)] = {};
	void *out;
	int out_size;
	int ret;

	out_size = MLX5_ST_SZ_BYTES(query_hca_cap_out);
	out = kzalloc(out_size, GFP_KERNEL);
	if (!out)
		return -ENOMEM;

	MLX5_SET(query_hca_cap_in, in, opcode, MLX5_CMD_OP_QUERY_HCA_CAP);
	MLX5_SET(query_hca_cap_in, in, other_function, 1);
	MLX5_SET(query_hca_cap_in, in, function_id, function_id);
	MLX5_SET(query_hca_cap_in, in, op_mod,
		 MLX5_SET_HCA_CAP_OP_MOD_GENERAL_DEVICE << 1 |
		 HCA_CAP_OPMOD_GET_CUR);

	ret = mlx5_cmd_exec_inout(pf_mdev, query_hca_cap, in, out);
	if (ret)
		goto out;

	*vhca_id = MLX5_GET(query_hca_cap_out, out,
			    capability.cmd_hca_cap.vhca_id);
out:
	kfree(out);
	return ret;
}

static long vfmig_ioc_get_vhca_id(struct mlx5_vfmig_pf *vfmig,
				  void __user *uarg)
{
	struct mlx5_vfmig_get_vhca_id arg;
	struct mlx5_core_sriov *sriov;
	u16 vhca_id;
	int err;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;
	if (arg.reserved)
		return -EINVAL;

	sriov = &vfmig->pf_mdev->priv.sriov;
	if (arg.vf_id >= sriov->num_vfs)
		return -EINVAL;

	/* SR-IOV VF index @vf_id maps to function_id vf_id + 1. */
	err = vfmig_query_vhca_id(vfmig->pf_mdev, arg.vf_id + 1, &vhca_id);
	if (err)
		return err;

	arg.vhca_id = vhca_id;
	if (copy_to_user(uarg, &arg, sizeof(arg)))
		return -EFAULT;

	return 0;
}

static long vfmig_ioc_mark_restored(struct mlx5_vfmig_pf *vfmig,
				    void __user *uarg)
{
	struct mlx5_vfmig_mark_restored arg;
	struct mlx5_core_sriov *sriov;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;
	if (arg.reserved)
		return -EINVAL;

	sriov = &vfmig->pf_mdev->priv.sriov;
	if (arg.vf_id >= sriov->num_vfs || arg.vf_id >= vfmig->max_vfs)
		return -EINVAL;

	if (test_and_set_bit(arg.vf_id, vfmig->restored))
		return -EALREADY;

	mlx5_core_dbg(vfmig->pf_mdev, "vfmig: VF %u marked restored\n",
		      arg.vf_id);

	return 0;
}

static long vfmig_ioc_query_vf(struct mlx5_vfmig_pf *vfmig,
			       void __user *uarg)
{
	struct mlx5_vfmig_query_vf arg;
	struct mlx5_core_sriov *sriov;
	u16 vhca_id;
	int err;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;

	sriov = &vfmig->pf_mdev->priv.sriov;
	arg.num_vfs = sriov->num_vfs;
	arg.reserved = 0;

	if (arg.vf_id >= sriov->num_vfs) {
		arg.vhca_id = 0;
		arg.restored = 0;
		if (copy_to_user(uarg, &arg, sizeof(arg)))
			return -EFAULT;
		return -ERANGE;
	}

	err = vfmig_query_vhca_id(vfmig->pf_mdev, arg.vf_id + 1, &vhca_id);
	if (err)
		return err;

	arg.vhca_id = vhca_id;
	arg.restored = (arg.vf_id < vfmig->max_vfs &&
			test_bit(arg.vf_id, vfmig->restored)) ? 1 : 0;

	if (copy_to_user(uarg, &arg, sizeof(arg)))
		return -EFAULT;

	return 0;
}

/*
 * Gating capabilities for the VF migratable bit. The PF mdev itself
 * must report both `migration` and `vhca_resource_manager` -- matches
 * what mlx5_devlink_port_fn_migratable_set checks before letting
 * userspace flip the per-VF migratable bit. Returns 0 if supported,
 * -EOPNOTSUPP otherwise.
 */
static int vfmig_check_pf_migration_caps(struct mlx5_core_dev *pf_mdev)
{
	if (!MLX5_CAP_GEN(pf_mdev, migration)) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: PF firmware does not advertise migration capability\n");
		return -EOPNOTSUPP;
	}
	if (!MLX5_CAP_GEN(pf_mdev, vhca_resource_manager)) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: PF firmware does not advertise vhca_resource_manager\n");
		return -EOPNOTSUPP;
	}

	return 0;
}

/*
 * Pre-bind helper: idempotently set HCA_CAP_2.migratable=1 on @vf_id.
 * The firmware only accepts this modify-cap while the VF is unbound
 * (no ENABLE_HCA issued yet); on an already-probed VF it returns
 * "bad resource state". The bit is intentionally never cleared again:
 * a VF migration-enabled once stays so for the SR-IOV provisioning.
 *
 * The vport number for VF index @vf_id under standard SR-IOV is
 * @vf_id + 1 (vport 0 is the PF).
 */
static int vfmig_set_vf_migratable(struct mlx5_core_dev *pf_mdev, u32 vf_id)
{
	int query_sz = MLX5_ST_SZ_BYTES(query_hca_cap_out);
	u16 vport = vf_id + 1;
	void *query_ctx;
	void *hca_caps;
	int err;

	query_ctx = kzalloc(query_sz, GFP_KERNEL);
	if (!query_ctx)
		return -ENOMEM;

	err = mlx5_vport_get_other_func_cap(pf_mdev, vport, query_ctx,
					    MLX5_CAP_GENERAL_2);
	if (err) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: query GENERAL_2 cap for vf %u (vport %u) failed: %d\n",
			       vf_id, vport, err);
		goto out;
	}

	hca_caps = MLX5_ADDR_OF(query_hca_cap_out, query_ctx, capability);
	if (MLX5_GET(cmd_hca_cap_2, hca_caps, migratable)) {
		err = 0;
		goto out;
	}

	MLX5_SET(cmd_hca_cap_2, hca_caps, migratable, 1);
	err = mlx5_vport_set_other_func_cap(pf_mdev, hca_caps, vport,
					    MLX5_SET_HCA_CAP_OP_MOD_GENERAL_DEVICE2);
	if (err) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: set GENERAL_2.migratable=1 for vf %u (vport %u) failed: %d (VF must be unbound)\n",
			       vf_id, vport, err);
		goto out;
	}
	mlx5_core_info(pf_mdev,
		       "vfmig: enabled migratable cap for vf %u (vport %u)\n",
		       vf_id, vport);
out:
	kfree(query_ctx);
	return err;
}

static long vfmig_ioc_enable_migratable(struct mlx5_vfmig_pf *vfmig,
					void __user *uarg)
{
	struct mlx5_vfmig_enable_migratable arg;
	struct mlx5_core_sriov *sriov;
	int err;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;
	if (arg.reserved)
		return -EINVAL;

	sriov = &vfmig->pf_mdev->priv.sriov;
	if (arg.vf_id >= sriov->num_vfs)
		return -EINVAL;

	err = vfmig_check_pf_migration_caps(vfmig->pf_mdev);
	if (err)
		return err;

	return vfmig_set_vf_migratable(vfmig->pf_mdev, arg.vf_id);
}

/*
 * Raw firmware SUSPEND_VHCA on a VF's @vhca_id, issued by the PF with
 * other_function implied by @vhca_id. @op_mod selects the
 * initiator/responder direction. This is the primitive the suspend
 * ladder is built from.
 */
static int vfmig_cmd_suspend_vhca(struct mlx5_core_dev *pf_mdev, u16 vhca_id,
				  u16 op_mod)
{
	u32 out[MLX5_ST_SZ_DW(suspend_vhca_out)] = {};
	u32 in[MLX5_ST_SZ_DW(suspend_vhca_in)] = {};

	MLX5_SET(suspend_vhca_in, in, opcode, MLX5_CMD_OP_SUSPEND_VHCA);
	MLX5_SET(suspend_vhca_in, in, vhca_id, vhca_id);
	MLX5_SET(suspend_vhca_in, in, op_mod, op_mod);

	return mlx5_cmd_exec_inout(pf_mdev, suspend_vhca, in, out);
}

static int vfmig_cmd_resume_vhca(struct mlx5_core_dev *pf_mdev, u16 vhca_id,
				 u16 op_mod)
{
	u32 out[MLX5_ST_SZ_DW(resume_vhca_out)] = {};
	u32 in[MLX5_ST_SZ_DW(resume_vhca_in)] = {};

	MLX5_SET(resume_vhca_in, in, opcode, MLX5_CMD_OP_RESUME_VHCA);
	MLX5_SET(resume_vhca_in, in, vhca_id, vhca_id);
	MLX5_SET(resume_vhca_in, in, op_mod, op_mod);

	return mlx5_cmd_exec_inout(pf_mdev, resume_vhca, in, out);
}

/* Park one ladder step deeper from state @s (RUNNING->P2P or P2P->STOP). */
static int vfmig_dp_suspend_step(struct mlx5_core_dev *pf_mdev, u16 vhca_id,
				 u8 s)
{
	u16 op_mod = (s == MLX5_VFMIG_DP_RUNNING) ?
		MLX5_SUSPEND_VHCA_IN_OP_MOD_SUSPEND_INITIATOR :
		MLX5_SUSPEND_VHCA_IN_OP_MOD_SUSPEND_RESPONDER;
	int err = vfmig_cmd_suspend_vhca(pf_mdev, vhca_id, op_mod);

	if (err)
		mlx5_core_warn(pf_mdev,
			       "vfmig: SUSPEND_VHCA(%s) vhca_id 0x%04x failed: %d\n",
			       s == MLX5_VFMIG_DP_RUNNING ? "INITIATOR" : "RESPONDER",
			       vhca_id, err);
	return err;
}

/* Unpark one ladder step shallower from state @s (STOP->P2P or P2P->RUNNING). */
static int vfmig_dp_resume_step(struct mlx5_core_dev *pf_mdev, u16 vhca_id,
				u8 s)
{
	u16 op_mod = (s == MLX5_VFMIG_DP_STOP) ?
		MLX5_RESUME_VHCA_IN_OP_MOD_RESUME_RESPONDER :
		MLX5_RESUME_VHCA_IN_OP_MOD_RESUME_INITIATOR;
	int err = vfmig_cmd_resume_vhca(pf_mdev, vhca_id, op_mod);

	if (err)
		mlx5_core_warn(pf_mdev,
			       "vfmig: RESUME_VHCA(%s) vhca_id 0x%04x failed: %d\n",
			       s == MLX5_VFMIG_DP_STOP ? "RESPONDER" : "INITIATOR",
			       vhca_id, err);
	return err;
}

/*
 * Walk the datapath ladder from @from to @to one firmware step at a time
 * (deeper via SUSPEND, shallower via RESUME), latching the depth actually
 * reached in *@reached. On a step failure the walk stops and *@reached
 * holds the truthful intermediate depth, so the caller can recover with
 * the inverse operation. Returns 0 or the first firmware error; *@reached
 * is always set.
 */
static int vfmig_dp_transition(struct mlx5_core_dev *pf_mdev, u16 vhca_id,
			       u8 from, u8 to, u8 *reached)
{
	int err = 0;
	u8 s = from;

	while (s < to) {		/* deeper suspend */
		err = vfmig_dp_suspend_step(pf_mdev, vhca_id, s);
		if (err)
			break;
		s++;
	}
	while (s > to) {		/* shallower resume */
		err = vfmig_dp_resume_step(pf_mdev, vhca_id, s);
		if (err)
			break;
		s--;
	}

	*reached = s;
	return err;
}

/*
 * Read back cmd_hca_cap_2.migratable for @vf_id via
 * QUERY_HCA_CAP(other_function=1), the gate SUSPEND requires. The vport
 * number for VF index @vf_id under standard SR-IOV is @vf_id + 1.
 */
static int vfmig_query_vf_migratable(struct mlx5_core_dev *pf_mdev, u32 vf_id,
				     bool *enabled)
{
	int query_sz = MLX5_ST_SZ_BYTES(query_hca_cap_out);
	u16 vport = vf_id + 1;
	void *query_ctx;
	void *hca_caps;
	int err;

	query_ctx = kzalloc(query_sz, GFP_KERNEL);
	if (!query_ctx)
		return -ENOMEM;

	err = mlx5_vport_get_other_func_cap(pf_mdev, vport, query_ctx,
					    MLX5_CAP_GENERAL_2);
	if (err) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: query GENERAL_2 cap for vf %u (vport %u) failed: %d\n",
			       vf_id, vport, err);
		goto out;
	}

	hca_caps = MLX5_ADDR_OF(query_hca_cap_out, query_ctx, capability);
	*enabled = MLX5_GET(cmd_hca_cap_2, hca_caps, migratable);
out:
	kfree(query_ctx);
	return err;
}

static long vfmig_ioc_suspend_vhca(struct mlx5_vfmig_pf *vfmig,
				   void __user *uarg)
{
	struct mlx5_core_dev *pf_mdev = vfmig->pf_mdev;
	struct mlx5_vfmig_suspend_vhca arg;
	struct mlx5_core_sriov *sriov;
	bool migratable = false;
	u8 cur, target, reached;
	u32 dir;
	u16 vhca_id;
	int err;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;
	if (arg.reserved[0] || arg.reserved[1])
		return -EINVAL;
	dir = arg.flags ? arg.flags : MLX5_VFMIG_DIR_FLAG_ALL;
	if (dir & ~(u32)MLX5_VFMIG_DIR_FLAG_ALL)
		return -EINVAL;

	sriov = &pf_mdev->priv.sriov;
	if (arg.vf_id >= sriov->num_vfs)
		return -EINVAL;

	cur = sriov->vfs_ctx[arg.vf_id].vfmig_dp_state;

	/*
	 * Map the requested direction(s) onto a target depth: suspending the
	 * initiator parks it (RUNNING->P2P); suspending the responder implies
	 * the initiator is already parked and drives to STOP. A responder-only
	 * suspend while still RUNNING is out of order (firmware requires
	 * initiator-first) -- reject it.
	 */
	if ((dir & MLX5_VFMIG_DIR_FLAG_RESPONDER) &&
	    !(dir & MLX5_VFMIG_DIR_FLAG_INITIATOR) &&
	    cur == MLX5_VFMIG_DP_RUNNING)
		return -EINVAL;

	target = cur;
	if ((dir & MLX5_VFMIG_DIR_FLAG_INITIATOR) && target < MLX5_VFMIG_DP_P2P)
		target = MLX5_VFMIG_DP_P2P;
	if (dir & MLX5_VFMIG_DIR_FLAG_RESPONDER)
		target = MLX5_VFMIG_DP_STOP;

	if (target <= cur)		/* already at or past the target */
		return 0;

	err = vfmig_check_pf_migration_caps(pf_mdev);
	if (err)
		return err;

	err = vfmig_query_vf_migratable(pf_mdev, arg.vf_id, &migratable);
	if (err)
		return err;
	if (!migratable) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: vf %u is not migration-enabled (issue ENABLE_MIGRATABLE pre-bind)\n",
			       arg.vf_id);
		return -EOPNOTSUPP;
	}

	err = vfmig_query_vhca_id(pf_mdev, arg.vf_id + 1, &vhca_id);
	if (err)
		return err;

	err = vfmig_dp_transition(pf_mdev, vhca_id, cur, target, &reached);
	sriov->vfs_ctx[arg.vf_id].vfmig_dp_state = reached;
	if (err)
		return err;

	mlx5_core_info(pf_mdev,
		       "vfmig: suspended vf %u (vhca_id 0x%04x) datapath %u->%u\n",
		       arg.vf_id, vhca_id, cur, reached);
	return 0;
}

static long vfmig_ioc_resume_vhca(struct mlx5_vfmig_pf *vfmig,
				  void __user *uarg)
{
	struct mlx5_core_dev *pf_mdev = vfmig->pf_mdev;
	struct mlx5_vfmig_resume_vhca arg;
	struct mlx5_core_sriov *sriov;
	u8 cur, target, reached;
	u32 dir;
	u16 vhca_id;
	int err;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;
	if (arg.reserved[0] || arg.reserved[1])
		return -EINVAL;
	dir = arg.flags ? arg.flags : MLX5_VFMIG_DIR_FLAG_ALL;
	if (dir & ~(u32)MLX5_VFMIG_DIR_FLAG_ALL)
		return -EINVAL;

	sriov = &pf_mdev->priv.sriov;
	if (arg.vf_id >= sriov->num_vfs)
		return -EINVAL;

	cur = sriov->vfs_ctx[arg.vf_id].vfmig_dp_state;

	/*
	 * Map the requested direction(s) onto a target depth: resuming the
	 * responder revives it (STOP->P2P); resuming the initiator drives all
	 * the way to RUNNING. An initiator-only resume while still STOP is out
	 * of order (firmware requires responder-first) -- reject it.
	 */
	if ((dir & MLX5_VFMIG_DIR_FLAG_INITIATOR) &&
	    !(dir & MLX5_VFMIG_DIR_FLAG_RESPONDER) &&
	    cur == MLX5_VFMIG_DP_STOP)
		return -EINVAL;

	target = cur;
	if ((dir & MLX5_VFMIG_DIR_FLAG_RESPONDER) && target > MLX5_VFMIG_DP_P2P)
		target = MLX5_VFMIG_DP_P2P;
	if (dir & MLX5_VFMIG_DIR_FLAG_INITIATOR)
		target = MLX5_VFMIG_DP_RUNNING;

	if (target >= cur)		/* already at or above the target */
		return 0;

	err = vfmig_query_vhca_id(pf_mdev, arg.vf_id + 1, &vhca_id);
	if (err)
		return err;

	err = vfmig_dp_transition(pf_mdev, vhca_id, cur, target, &reached);
	sriov->vfs_ctx[arg.vf_id].vfmig_dp_state = reached;
	if (err)
		return err;

	mlx5_core_info(pf_mdev,
		       "vfmig: resumed vf %u (vhca_id 0x%04x) datapath %u->%u\n",
		       arg.vf_id, vhca_id, cur, reached);
	return 0;
}

/* -------- SAVE_VHCA_STATE: DMA/mkey helpers (cloned from VFIO variant) --- */

/*
 * The helpers below (alloc_mkey_in, create_mkey, register_dma_pages,
 * unregister_dma_pages, alloc_pages, free_pages) are copies of the static
 * helpers in drivers/vfio/pci/mlx5/cmd.c. They are duplicated here so this
 * driver needs no new export boundary into the VFIO variant; a future
 * patch should hoist them into mlx5_core proper and let both consume the
 * shared versions.
 */
static u32 *vfmig_alloc_mkey_in(u32 npages, u32 pdn)
{
	int inlen;
	void *mkc;
	u32 *in;

	inlen = MLX5_ST_SZ_BYTES(create_mkey_in) +
		sizeof(__be64) * round_up(npages, 2);

	in = kvzalloc(inlen, GFP_KERNEL_ACCOUNT);
	if (!in)
		return NULL;

	MLX5_SET(create_mkey_in, in, translations_octword_actual_size,
		 DIV_ROUND_UP(npages, 2));

	mkc = MLX5_ADDR_OF(create_mkey_in, in, memory_key_mkey_entry);
	MLX5_SET(mkc, mkc, access_mode_1_0, MLX5_MKC_ACCESS_MODE_MTT);
	MLX5_SET(mkc, mkc, lr, 1);
	MLX5_SET(mkc, mkc, lw, 1);
	MLX5_SET(mkc, mkc, rr, 1);
	MLX5_SET(mkc, mkc, rw, 1);
	MLX5_SET(mkc, mkc, pd, pdn);
	MLX5_SET(mkc, mkc, bsf_octword_size, 0);
	MLX5_SET(mkc, mkc, qpn, 0xffffff);
	MLX5_SET(mkc, mkc, log_page_size, PAGE_SHIFT);
	MLX5_SET(mkc, mkc, translations_octword_size, DIV_ROUND_UP(npages, 2));
	MLX5_SET64(mkc, mkc, len, npages * PAGE_SIZE);

	return in;
}

static int vfmig_create_mkey(struct mlx5_core_dev *mdev, u32 npages,
			     u32 *mkey_in, u32 *mkey)
{
	int inlen = MLX5_ST_SZ_BYTES(create_mkey_in) +
		sizeof(__be64) * round_up(npages, 2);

	return mlx5_core_create_mkey(mdev, mkey, mkey_in, inlen);
}

static void vfmig_unregister_dma_pages(struct mlx5_core_dev *mdev, u32 npages,
				       u32 *mkey_in,
				       struct dma_iova_state *state,
				       enum dma_data_direction dir)
{
	dma_addr_t addr;
	__be64 *mtt;
	int i;

	if (dma_use_iova(state)) {
		dma_iova_destroy(mdev->device, state, npages * PAGE_SIZE, dir,
				 0);
	} else {
		mtt = (__be64 *)MLX5_ADDR_OF(create_mkey_in, mkey_in,
					     klm_pas_mtt);
		for (i = npages - 1; i >= 0; i--) {
			addr = be64_to_cpu(mtt[i]);
			dma_unmap_page(mdev->device, addr, PAGE_SIZE, dir);
		}
	}
}

static int vfmig_register_dma_pages(struct mlx5_core_dev *mdev, u32 npages,
				    struct page **page_list, u32 *mkey_in,
				    struct dma_iova_state *state,
				    enum dma_data_direction dir)
{
	dma_addr_t addr;
	size_t mapped = 0;
	__be64 *mtt;
	int i, err;

	mtt = (__be64 *)MLX5_ADDR_OF(create_mkey_in, mkey_in, klm_pas_mtt);

	if (dma_iova_try_alloc(mdev->device, state, 0, npages * PAGE_SIZE)) {
		addr = state->addr;
		for (i = 0; i < npages; i++) {
			err = dma_iova_link(mdev->device, state,
					    page_to_phys(page_list[i]), mapped,
					    PAGE_SIZE, dir, 0);
			if (err)
				goto error;
			*mtt++ = cpu_to_be64(addr);
			addr += PAGE_SIZE;
			mapped += PAGE_SIZE;
		}
		err = dma_iova_sync(mdev->device, state, 0, mapped);
		if (err)
			goto error;
	} else {
		for (i = 0; i < npages; i++) {
			addr = dma_map_page(mdev->device, page_list[i], 0,
					    PAGE_SIZE, dir);
			err = dma_mapping_error(mdev->device, addr);
			if (err)
				goto error;
			*mtt++ = cpu_to_be64(addr);
		}
	}
	return 0;

error:
	vfmig_unregister_dma_pages(mdev, i, mkey_in, state, dir);
	return err;
}

static int vfmig_alloc_pages(struct page ***page_list, unsigned int npages)
{
	unsigned int filled, done = 0;
	int i;

	*page_list = kvcalloc(npages, sizeof(struct page *),
			      GFP_KERNEL_ACCOUNT);
	if (!*page_list)
		return -ENOMEM;

	for (;;) {
		filled = alloc_pages_bulk(GFP_KERNEL_ACCOUNT, npages - done,
					  *page_list + done);
		if (!filled)
			goto err;

		done += filled;
		if (done == npages)
			break;
	}

	return 0;
err:
	for (i = 0; i < done; i++)
		__free_page((*page_list)[i]);

	kvfree(*page_list);
	*page_list = NULL;
	return -ENOMEM;
}

static void vfmig_free_pages(struct page **page_list, u32 npages)
{
	int i;

	if (!page_list)
		return;

	for (i = npages - 1; i >= 0; i--)
		__free_page(page_list[i]);

	kvfree(page_list);
}

/*
 * Synchronous QUERY_VHCA_MIGRATION_STATE: single-shot (no incremental, no
 * chunk mode), *@size_out gets required_umem_size in bytes.
 */
static int vfmig_cmd_query_vhca_migration_state(struct mlx5_core_dev *pf_mdev,
						u16 vhca_id, u64 *size_out)
{
	u32 out[MLX5_ST_SZ_DW(query_vhca_migration_state_out)] = {};
	u32 in[MLX5_ST_SZ_DW(query_vhca_migration_state_in)] = {};
	int err;

	MLX5_SET(query_vhca_migration_state_in, in, opcode,
		 MLX5_CMD_OP_QUERY_VHCA_MIGRATION_STATE);
	MLX5_SET(query_vhca_migration_state_in, in, vhca_id, vhca_id);
	MLX5_SET(query_vhca_migration_state_in, in, op_mod, 0);
	MLX5_SET(query_vhca_migration_state_in, in, incremental, 0);
	MLX5_SET(query_vhca_migration_state_in, in, chunk, 0);

	err = mlx5_cmd_exec_inout(pf_mdev, query_vhca_migration_state, in, out);
	if (err)
		return err;

	*size_out = MLX5_GET(query_vhca_migration_state_out, out,
			     required_umem_size);
	return 0;
}

/*
 * Synchronous SAVE_VHCA_STATE. @size is the buffer capacity offered to the
 * firmware (bytes); *@actual_size_out gets the bytes it actually wrote.
 */
static int vfmig_cmd_save_vhca_state(struct mlx5_core_dev *pf_mdev, u16 vhca_id,
				     u32 mkey, size_t size,
				     u64 *actual_size_out)
{
	u32 out[MLX5_ST_SZ_DW(save_vhca_state_out)] = {};
	u32 in[MLX5_ST_SZ_DW(save_vhca_state_in)] = {};
	int err;

	MLX5_SET(save_vhca_state_in, in, opcode, MLX5_CMD_OP_SAVE_VHCA_STATE);
	MLX5_SET(save_vhca_state_in, in, op_mod, 0);
	MLX5_SET(save_vhca_state_in, in, vhca_id, vhca_id);
	MLX5_SET(save_vhca_state_in, in, mkey, mkey);
	MLX5_SET(save_vhca_state_in, in, size, size);
	MLX5_SET(save_vhca_state_in, in, incremental, 0);
	MLX5_SET(save_vhca_state_in, in, set_track, 0);

	err = mlx5_cmd_exec_inout(pf_mdev, save_vhca_state, in, out);
	if (err)
		return err;

	*actual_size_out = MLX5_GET(save_vhca_state_out, out,
				    actual_image_size);
	return 0;
}

/* -------- SAVE_VHCA_STATE: per-fd state buffer -------------------------- */

/*
 * Per-SAVE-fd context, hung off vfmig->save_ctxs. All firmware resources
 * are allocated up front in the ioctl handler and torn down on release()
 * (or by pf_cleanup if the PF is removed first). The fd's sole job is to
 * drain the staging pages, framed as one FW_DATA wire record.
 */
struct mlx5_vfmig_save_ctx {
	struct list_head node;		/* on vfmig->save_ctxs */
	struct mlx5_vfmig_pf *vfmig;	/* holds a kref */
	struct mutex io_lock;		/* serializes concurrent read()s */
	u32 vf_id;
	u16 vhca_id;
	u32 flags;			/* MLX5_VFMIG_SAVE_FLAG_* */

	/*
	 * Transient suspend bookkeeping for the self-suspend / resume-on-
	 * close policy. @owns_suspend is true only when this SAVE session
	 * issued the SUSPEND itself (the VF was RUNNING/P2P at open); it is
	 * false when the caller pre-parked the VF to STOP via SUSPEND_VHCA,
	 * in which case the caller owns the matching RESUME_VHCA and close()
	 * must not auto-resume. The suspended_* bools track which ladder
	 * steps this session actually issued, so close() (or the setup error
	 * path) undoes exactly those.
	 */
	bool owns_suspend;
	bool suspended_initiator;
	bool suspended_responder;

	/* Firmware-tied resources, mutated under vfmig->lock. */
	bool resources_freed;
	bool pd_allocated;
	u32 pdn;
	bool image_dma_mapped;
	bool image_mkey_created;
	struct page **image_pages;
	u32 image_npages;		/* allocated capacity in PAGE_SIZE */
	u32 *image_mkey_in;
	u32 image_mkey;
	struct dma_iova_state image_dma_state;

	u64 image_size;			/* bytes the firmware wrote */
	u64 read_pos;			/* cursor over [FW_DATA hdr | payload] */
};

/* Build the on-wire FW_DATA header for ctx->image_size into @hdr. */
static void vfmig_save_build_header(struct mlx5_vfmig_save_ctx *ctx,
				    struct vfmig_wire_header *hdr)
{
	hdr->record_size = cpu_to_le64(ctx->image_size);
	hdr->flags = cpu_to_le32(0);
	hdr->tag = cpu_to_le32(VFMIG_WIRE_TAG_FW_DATA);
}

/*
 * Copy out the byte range [read_pos, read_pos+count) of the save stream:
 *   [0 .. HDR_SZ)                  FW_DATA wire header
 *   [HDR_SZ .. HDR_SZ+image_size)  firmware payload from image_pages[]
 * EOF is HDR_SZ + image_size. Caller holds vfmig->lock for read AND
 * ctx->io_lock.
 */
static ssize_t vfmig_save_drain(struct mlx5_vfmig_save_ctx *ctx,
				char __user *ubuf, size_t count)
{
	const u64 HDR_SZ = sizeof(struct vfmig_wire_header);
	const u64 total  = HDR_SZ + ctx->image_size;
	size_t copied = 0;
	ssize_t err = 0;

	if (ctx->read_pos >= total)
		return 0;

	/* FW_DATA wire header. */
	if (ctx->read_pos < HDR_SZ && count) {
		struct vfmig_wire_header hdr;
		u64 hoff = ctx->read_pos;
		size_t want = min_t(size_t, count, HDR_SZ - hoff);

		vfmig_save_build_header(ctx, &hdr);
		if (copy_to_user(ubuf, ((u8 *)&hdr) + hoff, want))
			return -EFAULT;
		ctx->read_pos += want;
		ubuf += want;
		count -= want;
		copied += want;
	}

	/* FW payload pages. */
	while (count && ctx->read_pos < total) {
		u64 payload_off = ctx->read_pos - HDR_SZ;
		u32 page_idx = payload_off >> PAGE_SHIFT;
		size_t page_off = payload_off & (PAGE_SIZE - 1);
		size_t want = min3((size_t)(total - ctx->read_pos), count,
				   PAGE_SIZE - page_off);
		const u8 *from;

		if (page_idx >= ctx->image_npages)
			return copied ? (ssize_t)copied : -EINVAL;

		from = kmap_local_page(ctx->image_pages[page_idx]);
		if (copy_to_user(ubuf, from + page_off, want)) {
			kunmap_local(from);
			err = -EFAULT;
			break;
		}
		kunmap_local(from);
		ctx->read_pos += want;
		ubuf += want;
		count -= want;
		copied += want;
	}

	if (!copied && err)
		return err;
	return copied;
}

static ssize_t vfmig_save_read(struct file *filp, char __user *ubuf,
			       size_t count, loff_t *ppos)
{
	struct mlx5_vfmig_save_ctx *ctx = filp->private_data;
	struct mlx5_vfmig_pf *vfmig = ctx->vfmig;
	ssize_t ret;

	if (!count)
		return 0;

	mutex_lock(&ctx->io_lock);
	down_read(&vfmig->lock);
	if (vfmig->dead || ctx->resources_freed) {
		ret = -ENODEV;
		goto out;
	}
	ret = vfmig_save_drain(ctx, ubuf, count);
out:
	up_read(&vfmig->lock);
	mutex_unlock(&ctx->io_lock);
	return ret;	/* stream_open() => ppos is NULL, leave it */
}

/*
 * Drop the firmware-tied resources held by @ctx. Idempotent via
 * @resources_freed. Called from release() (pf_mdev alive) or pf_cleanup()
 * (before pf_mdev is NULLed). The image page list is host memory and is
 * freed separately in release(). Caller holds vfmig->lock.
 */
static void vfmig_save_release_resources(struct mlx5_vfmig_save_ctx *ctx)
{
	struct mlx5_core_dev *pf_mdev = ctx->vfmig->pf_mdev;

	if (ctx->resources_freed)
		return;
	ctx->resources_freed = true;

	if (!pf_mdev)
		return;

	if (ctx->image_mkey_created) {
		mlx5_core_destroy_mkey(pf_mdev, ctx->image_mkey);
		ctx->image_mkey_created = false;
	}
	if (ctx->image_dma_mapped) {
		vfmig_unregister_dma_pages(pf_mdev, ctx->image_npages,
					   ctx->image_mkey_in,
					   &ctx->image_dma_state,
					   DMA_FROM_DEVICE);
		ctx->image_dma_mapped = false;
	}
	kvfree(ctx->image_mkey_in);
	ctx->image_mkey_in = NULL;

	if (ctx->pd_allocated) {
		mlx5_core_dealloc_pd(pf_mdev, ctx->pdn);
		ctx->pd_allocated = false;
	}

	/*
	 * Resume-on-close, inverse order of suspend (responder then
	 * initiator). Only undo suspends this session owns and issued, and
	 * only when the caller did not ask to keep the VF parked
	 * (KEEP_SUSPENDED). Best-effort: a failed resume is logged, not
	 * propagated -- the blob is already drained. The suspended_* bools
	 * guard against a stray RESUME if the SUSPEND never landed.
	 */
	if (!ctx->owns_suspend)
		return;
	if (ctx->flags & MLX5_VFMIG_SAVE_FLAG_KEEP_SUSPENDED)
		return;

	if (ctx->suspended_responder) {
		vfmig_dp_resume_step(pf_mdev, ctx->vhca_id, MLX5_VFMIG_DP_STOP);
		ctx->suspended_responder = false;
	}
	if (ctx->suspended_initiator) {
		vfmig_dp_resume_step(pf_mdev, ctx->vhca_id, MLX5_VFMIG_DP_P2P);
		ctx->suspended_initiator = false;
	}
}

static int vfmig_save_release(struct inode *inode, struct file *filp)
{
	struct mlx5_vfmig_save_ctx *ctx = filp->private_data;
	struct mlx5_vfmig_pf *vfmig = ctx->vfmig;

	down_read(&vfmig->lock);
	if (!vfmig->dead)
		vfmig_save_release_resources(ctx);
	up_read(&vfmig->lock);

	mutex_lock(&vfmig->ctxs_lock);
	list_del(&ctx->node);
	mutex_unlock(&vfmig->ctxs_lock);

	vfmig_free_pages(ctx->image_pages, ctx->image_npages);
	mutex_destroy(&ctx->io_lock);
	vfmig_pf_put(vfmig);
	kfree(ctx);
	return 0;
}

static const struct file_operations mlx5_vfmig_save_fops = {
	.owner		= THIS_MODULE,
	.read		= vfmig_save_read,
	.release	= vfmig_save_release,
};

static bool vfmig_vf_id_load_busy_locked(struct mlx5_vfmig_pf *vfmig,
					 u32 vf_id);

/* True iff @vf_id already has an open SAVE session. ctxs_lock held. */
static bool vfmig_vf_id_save_busy_locked(struct mlx5_vfmig_pf *vfmig, u32 vf_id)
{
	struct mlx5_vfmig_save_ctx *s;

	list_for_each_entry(s, &vfmig->save_ctxs, node)
		if (s->vf_id == vf_id)
			return true;
	return false;
}

/*
 * True iff @vf_id has any open SAVE or LOAD session. SAVE and LOAD are
 * mutually exclusive per VF so a session claims the vf_id for its whole
 * lifetime. ctxs_lock held.
 */
static bool vfmig_vf_id_busy_locked(struct mlx5_vfmig_pf *vfmig, u32 vf_id)
{
	return vfmig_vf_id_save_busy_locked(vfmig, vf_id) ||
	       vfmig_vf_id_load_busy_locked(vfmig, vf_id);
}

/*
 * Set up a SAVE session: resolve vhca_id, quiesce the VF to STOP (owning
 * and later undoing only the ladder steps this session issues), size the
 * snapshot, allocate a PD + image pages + MKEY, run SAVE_VHCA_STATE, then
 * hand back a read-only anon-inode fd. Caller holds vfmig->lock for read.
 */
static long vfmig_ioc_save_vhca_state(struct mlx5_vfmig_pf *vfmig,
				      void __user *uarg)
{
	struct mlx5_core_dev *pf_mdev = vfmig->pf_mdev;
	struct mlx5_vfmig_save_state arg;
	struct mlx5_vfmig_save_ctx *ctx;
	struct mlx5_core_sriov *sriov;
	bool migratable = false;
	u64 query_size = 0;
	u64 actual_size = 0;
	struct file *file;
	u32 npages;
	u16 vhca_id;
	int fd, err;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;
	if (arg.reserved || (arg.flags & ~MLX5_VFMIG_SAVE_FLAG_ALL))
		return -EINVAL;

	sriov = &pf_mdev->priv.sriov;
	if (arg.vf_id >= sriov->num_vfs)
		return -EINVAL;

	err = vfmig_check_pf_migration_caps(pf_mdev);
	if (err)
		return err;

	err = vfmig_query_vf_migratable(pf_mdev, arg.vf_id, &migratable);
	if (err)
		return err;
	if (!migratable) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: vf %u is not migration-enabled\n",
			       arg.vf_id);
		return -EOPNOTSUPP;
	}

	err = vfmig_query_vhca_id(pf_mdev, arg.vf_id + 1, &vhca_id);
	if (err)
		return err;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	INIT_LIST_HEAD(&ctx->node);
	mutex_init(&ctx->io_lock);
	ctx->vf_id = arg.vf_id;
	ctx->vhca_id = vhca_id;
	ctx->flags = arg.flags;

	/* Claim vf_id atomically vs other SAVE/LOAD sessions. */
	mutex_lock(&vfmig->ctxs_lock);
	if (vfmig_vf_id_busy_locked(vfmig, ctx->vf_id)) {
		mutex_unlock(&vfmig->ctxs_lock);
		err = -EBUSY;
		goto err_claim;
	}
	vfmig_pf_get(vfmig);
	ctx->vfmig = vfmig;
	list_add(&ctx->node, &vfmig->save_ctxs);
	mutex_unlock(&vfmig->ctxs_lock);

	err = mlx5_core_alloc_pd(pf_mdev, &ctx->pdn);
	if (err)
		goto err_res;
	ctx->pd_allocated = true;

	/*
	 * Quiesce to STOP for the capture, owning only the steps we issue:
	 *   - STOP already (explicit SUSPEND_VHCA): do nothing; the caller
	 *     owns the resume via RESUME_VHCA.
	 *   - P2P (explicit SUSPEND_VHCA(INITIATOR)): suspend the responder
	 *     only, and resume just that on close.
	 *   - RUNNING (standalone SAVE): suspend both directions and resume
	 *     both on close.
	 * The persistent vfmig_dp_state is owned by SUSPEND/RESUME_VHCA and
	 * left untouched: SAVE's suspend is transient and undone on close.
	 */
	switch (sriov->vfs_ctx[arg.vf_id].vfmig_dp_state) {
	case MLX5_VFMIG_DP_STOP:
		ctx->owns_suspend = false;
		break;
	case MLX5_VFMIG_DP_P2P:
		ctx->owns_suspend = true;
		err = vfmig_dp_suspend_step(pf_mdev, vhca_id,
					    MLX5_VFMIG_DP_P2P);
		if (err)
			goto err_res;
		ctx->suspended_responder = true;
		break;
	default: /* MLX5_VFMIG_DP_RUNNING */
		ctx->owns_suspend = true;
		err = vfmig_dp_suspend_step(pf_mdev, vhca_id,
					    MLX5_VFMIG_DP_RUNNING);
		if (err)
			goto err_res;
		ctx->suspended_initiator = true;
		err = vfmig_dp_suspend_step(pf_mdev, vhca_id,
					    MLX5_VFMIG_DP_P2P);
		if (err)
			goto err_res;
		ctx->suspended_responder = true;
		break;
	}

	err = vfmig_cmd_query_vhca_migration_state(pf_mdev, vhca_id,
						   &query_size);
	if (err) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: QUERY_VHCA_MIGRATION_STATE vf %u (vhca_id 0x%04x) failed: %d\n",
			       arg.vf_id, vhca_id, err);
		goto err_res;
	}
	if (!query_size || query_size > VFMIG_MAX_SAVE_SIZE) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: implausible migration size %llu for vf %u\n",
			       query_size, arg.vf_id);
		err = -ERANGE;
		goto err_res;
	}

	npages = max_t(u32, 1, DIV_ROUND_UP(query_size, PAGE_SIZE));
	err = vfmig_alloc_pages(&ctx->image_pages, npages);
	if (err)
		goto err_res;
	ctx->image_npages = npages;

	ctx->image_mkey_in = vfmig_alloc_mkey_in(npages, ctx->pdn);
	if (!ctx->image_mkey_in) {
		err = -ENOMEM;
		goto err_res;
	}

	err = vfmig_register_dma_pages(pf_mdev, npages, ctx->image_pages,
				       ctx->image_mkey_in,
				       &ctx->image_dma_state,
				       DMA_FROM_DEVICE);
	if (err)
		goto err_res;
	ctx->image_dma_mapped = true;

	err = vfmig_create_mkey(pf_mdev, npages, ctx->image_mkey_in,
				&ctx->image_mkey);
	if (err)
		goto err_res;
	ctx->image_mkey_created = true;

	err = vfmig_cmd_save_vhca_state(pf_mdev, vhca_id, ctx->image_mkey,
					npages * PAGE_SIZE, &actual_size);
	if (err) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: SAVE_VHCA_STATE vf %u (vhca_id 0x%04x) failed: %d\n",
			       arg.vf_id, vhca_id, err);
		goto err_res;
	}
	if (!actual_size || actual_size > (u64)npages * PAGE_SIZE) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: SAVE_VHCA_STATE returned implausible size %llu for vf %u\n",
			       actual_size, arg.vf_id);
		err = -EIO;
		goto err_res;
	}
	ctx->image_size = actual_size;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		err = fd;
		goto err_res;
	}

	file = anon_inode_getfile("mlx5_vfmig_save", &mlx5_vfmig_save_fops,
				  ctx, O_RDONLY | O_CLOEXEC);
	if (IS_ERR(file)) {
		err = PTR_ERR(file);
		goto err_fd;
	}
	stream_open(file_inode(file), file);

	arg.save_fd = fd;
	if (copy_to_user(uarg, &arg, sizeof(arg))) {
		err = -EFAULT;
		goto err_file;
	}

	fd_install(fd, file);
	mlx5_core_info(pf_mdev,
		       "vfmig: saved vf %u (vhca_id 0x%04x) state: %llu bytes\n",
		       arg.vf_id, vhca_id, actual_size);
	return 0;

err_file:
	/* fput() runs vfmig_save_release, which frees resources + ctx. */
	fput(file);
	put_unused_fd(fd);
	return err;
err_fd:
	put_unused_fd(fd);
err_res:
	/*
	 * Setup failed and no fd escapes, so fully restore the VF: drop
	 * KEEP_SUSPENDED before teardown so release_resources unwinds any
	 * suspend this session issued, even if the caller asked to keep it
	 * parked on a *successful* save.
	 */
	ctx->flags &= ~MLX5_VFMIG_SAVE_FLAG_KEEP_SUSPENDED;
	vfmig_save_release_resources(ctx);
	vfmig_free_pages(ctx->image_pages, ctx->image_npages);
	mutex_lock(&vfmig->ctxs_lock);
	list_del(&ctx->node);
	mutex_unlock(&vfmig->ctxs_lock);
	vfmig_pf_put(vfmig);
	mutex_destroy(&ctx->io_lock);
	kfree(ctx);
	return err;
err_claim:
	mutex_destroy(&ctx->io_lock);
	kfree(ctx);
	return err;
}

/* -------- LOAD_VHCA_STATE: per-fd state buffer ------------------------- */

/*
 * Synchronous LOAD_VHCA_STATE. @size is the payload byte count the
 * firmware should ingest from the MTT-mapped @mkey.
 */
static int vfmig_cmd_load_vhca_state(struct mlx5_core_dev *pf_mdev,
				     u16 vhca_id, u32 mkey, size_t size)
{
	u32 out[MLX5_ST_SZ_DW(load_vhca_state_out)] = {};
	u32 in[MLX5_ST_SZ_DW(load_vhca_state_in)] = {};

	MLX5_SET(load_vhca_state_in, in, opcode, MLX5_CMD_OP_LOAD_VHCA_STATE);
	MLX5_SET(load_vhca_state_in, in, op_mod, 0);
	MLX5_SET(load_vhca_state_in, in, vhca_id, vhca_id);
	MLX5_SET(load_vhca_state_in, in, mkey, mkey);
	MLX5_SET(load_vhca_state_in, in, size, size);

	return mlx5_cmd_exec_inout(pf_mdev, load_vhca_state, in, out);
}

/* Parser states for the write() FSM. */
enum vfmig_load_state {
	VFMIG_LS_READ_HEADER = 0,	/* accumulating the 16-byte header */
	VFMIG_LS_PREP_IMAGE,		/* allocate staging for the payload */
	VFMIG_LS_READ_IMAGE,		/* accumulating the FW_DATA payload */
	VFMIG_LS_LOAD_IMAGE,		/* payload complete, run LOAD */
	VFMIG_LS_DONE,			/* loaded; reject trailing bytes */
};

/*
 * Per-LOAD-fd context, hung off vfmig->load_ctxs. Mirrors the SAVE ctx
 * but in the write direction (DMA_TO_DEVICE): the write() FSM ingests one
 * FW_DATA record, stages it into DMA pages + an MTT MKEY, and runs
 * LOAD_VHCA_STATE once the payload is complete.
 */
struct mlx5_vfmig_load_ctx {
	struct list_head node;		/* on vfmig->load_ctxs */
	struct mlx5_vfmig_pf *vfmig;	/* holds a kref */
	struct mutex io_lock;		/* serializes concurrent write()s */
	u32 vf_id;
	u16 vhca_id;

	enum vfmig_load_state state;
	u8 hdr_buf[sizeof(struct vfmig_wire_header)];
	u32 hdr_filled;			/* header bytes accumulated */
	u64 record_size;		/* FW_DATA payload size from header */
	u64 image_filled;		/* payload bytes staged so far */
	bool loaded;			/* LOAD_VHCA_STATE has run */

	/* Firmware-tied resources, mutated under vfmig->lock. */
	bool resources_freed;
	bool pd_allocated;
	u32 pdn;
	bool image_dma_mapped;
	bool image_mkey_created;
	struct page **image_pages;
	u32 image_npages;
	u32 *image_mkey_in;
	u32 image_mkey;
	struct dma_iova_state image_dma_state;
};

/* True iff @vf_id already has an open LOAD session. ctxs_lock held. */
static bool vfmig_vf_id_load_busy_locked(struct mlx5_vfmig_pf *vfmig,
					 u32 vf_id)
{
	struct mlx5_vfmig_load_ctx *l;

	list_for_each_entry(l, &vfmig->load_ctxs, node)
		if (l->vf_id == vf_id)
			return true;
	return false;
}

/* Accumulate up to @want header bytes; returns bytes taken or -EFAULT. */
static ssize_t vfmig_load_consume_header(struct mlx5_vfmig_load_ctx *ctx,
					 const char __user *ubuf, size_t want)
{
	size_t need = sizeof(ctx->hdr_buf) - ctx->hdr_filled;
	size_t take = min(need, want);

	if (!take)
		return 0;
	if (copy_from_user(ctx->hdr_buf + ctx->hdr_filled, ubuf, take))
		return -EFAULT;
	ctx->hdr_filled += take;
	return take;
}

/* Copy @want payload bytes from user into the staging pages at image_filled. */
static ssize_t vfmig_load_consume_image(struct mlx5_vfmig_load_ctx *ctx,
					const char __user *ubuf, size_t want)
{
	size_t copied = 0;

	while (want) {
		size_t page_off = ctx->image_filled & (PAGE_SIZE - 1);
		u32 page_idx = ctx->image_filled >> PAGE_SHIFT;
		size_t chunk = min_t(size_t, want, PAGE_SIZE - page_off);
		u8 *to;

		if (page_idx >= ctx->image_npages)
			return copied ? (ssize_t)copied : -EINVAL;

		to = kmap_local_page(ctx->image_pages[page_idx]);
		if (copy_from_user(to + page_off, ubuf, chunk)) {
			kunmap_local(to);
			return copied ? (ssize_t)copied : -EFAULT;
		}
		kunmap_local(to);

		ctx->image_filled += chunk;
		ubuf += chunk;
		want -= chunk;
		copied += chunk;
	}
	return copied;
}

/*
 * Allocate the staging buffer (@want_npages pages) + MTT MKEY the firmware
 * will read for LOAD_VHCA_STATE. The PD was allocated in the ioctl handler.
 * Caller holds vfmig->lock for read.
 */
static int vfmig_load_prepare_image(struct mlx5_vfmig_load_ctx *ctx,
				    u32 want_npages)
{
	struct mlx5_core_dev *pf_mdev = ctx->vfmig->pf_mdev;
	int err;

	if (WARN_ON(!pf_mdev))
		return -ENODEV;

	err = vfmig_alloc_pages(&ctx->image_pages, want_npages);
	if (err)
		return err;
	ctx->image_npages = want_npages;

	ctx->image_mkey_in = vfmig_alloc_mkey_in(want_npages, ctx->pdn);
	if (!ctx->image_mkey_in) {
		err = -ENOMEM;
		goto err_pages;
	}

	err = vfmig_register_dma_pages(pf_mdev, want_npages, ctx->image_pages,
				       ctx->image_mkey_in,
				       &ctx->image_dma_state, DMA_TO_DEVICE);
	if (err)
		goto err_mkey_in;
	ctx->image_dma_mapped = true;

	err = vfmig_create_mkey(pf_mdev, want_npages, ctx->image_mkey_in,
				&ctx->image_mkey);
	if (err)
		goto err_dma;
	ctx->image_mkey_created = true;
	return 0;

err_dma:
	vfmig_unregister_dma_pages(pf_mdev, want_npages, ctx->image_mkey_in,
				   &ctx->image_dma_state, DMA_TO_DEVICE);
	ctx->image_dma_mapped = false;
err_mkey_in:
	kvfree(ctx->image_mkey_in);
	ctx->image_mkey_in = NULL;
err_pages:
	vfmig_free_pages(ctx->image_pages, ctx->image_npages);
	ctx->image_pages = NULL;
	ctx->image_npages = 0;
	return err;
}

/* Validate the parsed 16-byte header. Only a single FW_DATA record is OK. */
static int vfmig_load_dispatch_header(struct mlx5_vfmig_load_ctx *ctx)
{
	struct vfmig_wire_header *hdr =
		(struct vfmig_wire_header *)ctx->hdr_buf;
	u64 record_size = le64_to_cpu(hdr->record_size);
	u32 flags = le32_to_cpu(hdr->flags);
	u32 tag = le32_to_cpu(hdr->tag);

	if (tag != VFMIG_WIRE_TAG_FW_DATA || flags) {
		mlx5_core_warn(ctx->vfmig->pf_mdev,
			       "vfmig: vf %u: unsupported LOAD record (tag 0x%x flags 0x%x)\n",
			       ctx->vf_id, tag, flags);
		return -EOPNOTSUPP;
	}
	if (ctx->loaded) {
		mlx5_core_warn(ctx->vfmig->pf_mdev,
			       "vfmig: vf %u: multiple FW_DATA records per LOAD session not supported\n",
			       ctx->vf_id);
		return -EINVAL;
	}
	if (!record_size || record_size > VFMIG_MAX_SAVE_SIZE)
		return -EINVAL;

	ctx->record_size = record_size;
	ctx->image_filled = 0;
	ctx->state = VFMIG_LS_PREP_IMAGE;
	return 0;
}

/* Install the staged payload into the (suspended) VHCA. */
static int vfmig_load_run_load(struct mlx5_vfmig_load_ctx *ctx)
{
	struct mlx5_core_dev *pf_mdev = ctx->vfmig->pf_mdev;
	int err;

	if (WARN_ON(!ctx->image_mkey_created))
		return -EINVAL;

	err = vfmig_cmd_load_vhca_state(pf_mdev, ctx->vhca_id, ctx->image_mkey,
					ctx->record_size);
	if (err) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: LOAD_VHCA_STATE vf %u (vhca_id 0x%04x) failed: %d\n",
			       ctx->vf_id, ctx->vhca_id, err);
		return err;
	}

	ctx->loaded = true;
	ctx->state = VFMIG_LS_DONE;
	mlx5_core_info(pf_mdev,
		       "vfmig: loaded vf %u (vhca_id 0x%04x) state: %llu bytes\n",
		       ctx->vf_id, ctx->vhca_id, ctx->record_size);
	return 0;
}

/*
 * Advance the parser one step, consuming from [*ubuf, *ubuf+*left). Some
 * transitions consume zero bytes but are mandatory work, so the caller
 * loops on @progressed rather than on @left.
 */
static int vfmig_load_step(struct mlx5_vfmig_load_ctx *ctx,
			   const char __user **ubuf, size_t *left,
			   bool *progressed)
{
	u32 want_npages;
	ssize_t n;
	int err;

	switch (ctx->state) {
	case VFMIG_LS_READ_HEADER:
		n = vfmig_load_consume_header(ctx, *ubuf, *left);
		if (n < 0)
			return n;
		*ubuf += n;
		*left -= n;
		*progressed = n > 0;
		if (ctx->hdr_filled == sizeof(ctx->hdr_buf))
			return vfmig_load_dispatch_header(ctx);
		return 0;
	case VFMIG_LS_PREP_IMAGE:
		want_npages = max_t(u32, 1,
				    DIV_ROUND_UP(ctx->record_size, PAGE_SIZE));
		err = vfmig_load_prepare_image(ctx, want_npages);
		if (err)
			return err;
		ctx->state = VFMIG_LS_READ_IMAGE;
		*progressed = true;
		return 0;
	case VFMIG_LS_READ_IMAGE: {
		size_t want = min_t(size_t, *left,
				    ctx->record_size - ctx->image_filled);

		if (want) {
			n = vfmig_load_consume_image(ctx, *ubuf, want);
			if (n < 0)
				return n;
			*ubuf += n;
			*left -= n;
			*progressed = n > 0;
		}
		if (ctx->image_filled == ctx->record_size) {
			ctx->state = VFMIG_LS_LOAD_IMAGE;
			*progressed = true;
		}
		return 0;
	}
	case VFMIG_LS_LOAD_IMAGE:
		err = vfmig_load_run_load(ctx);
		if (err)
			return err;
		*progressed = true;
		return 0;
	case VFMIG_LS_DONE:
	default:
		/* A complete blob was already loaded; trailing bytes are junk. */
		if (*left)
			return -EINVAL;
		return 0;
	}
}

static ssize_t vfmig_load_write(struct file *filp, const char __user *ubuf,
				size_t count, loff_t *ppos)
{
	struct mlx5_vfmig_load_ctx *ctx = filp->private_data;
	struct mlx5_vfmig_pf *vfmig = ctx->vfmig;
	const char __user *cursor = ubuf;
	size_t left = count;
	ssize_t produced;
	bool progressed;
	int err = 0;

	if (!count)
		return 0;

	mutex_lock(&ctx->io_lock);
	down_read(&vfmig->lock);
	if (vfmig->dead || ctx->resources_freed) {
		err = -ENODEV;
		goto out;
	}

	/* Drive the FSM until it stops making progress (not until left==0). */
	for (;;) {
		progressed = false;
		err = vfmig_load_step(ctx, &cursor, &left, &progressed);
		if (err || !progressed)
			break;
	}
out:
	up_read(&vfmig->lock);
	mutex_unlock(&ctx->io_lock);

	produced = (ssize_t)(count - left);
	if (!produced && err)
		return err;
	return produced;	/* stream_open() => ppos is NULL, leave it */
}

/*
 * Drop the firmware-tied resources held by @ctx. Idempotent via
 * @resources_freed. Called from release() (pf_mdev alive) or pf_cleanup()
 * (before pf_mdev is NULLed). The image page list is host memory and is
 * freed separately in release(). Caller holds vfmig->lock.
 */
static void vfmig_load_release_resources(struct mlx5_vfmig_load_ctx *ctx)
{
	struct mlx5_core_dev *pf_mdev = ctx->vfmig->pf_mdev;

	if (ctx->resources_freed)
		return;
	ctx->resources_freed = true;

	if (!pf_mdev)
		return;

	if (ctx->image_mkey_created) {
		mlx5_core_destroy_mkey(pf_mdev, ctx->image_mkey);
		ctx->image_mkey_created = false;
	}
	if (ctx->image_dma_mapped) {
		vfmig_unregister_dma_pages(pf_mdev, ctx->image_npages,
					   ctx->image_mkey_in,
					   &ctx->image_dma_state,
					   DMA_TO_DEVICE);
		ctx->image_dma_mapped = false;
	}
	kvfree(ctx->image_mkey_in);
	ctx->image_mkey_in = NULL;

	if (ctx->pd_allocated) {
		mlx5_core_dealloc_pd(pf_mdev, ctx->pdn);
		ctx->pd_allocated = false;
	}
}

static int vfmig_load_release(struct inode *inode, struct file *filp)
{
	struct mlx5_vfmig_load_ctx *ctx = filp->private_data;
	struct mlx5_vfmig_pf *vfmig = ctx->vfmig;

	down_read(&vfmig->lock);
	if (!vfmig->dead)
		vfmig_load_release_resources(ctx);
	up_read(&vfmig->lock);

	mutex_lock(&vfmig->ctxs_lock);
	list_del(&ctx->node);
	mutex_unlock(&vfmig->ctxs_lock);

	vfmig_free_pages(ctx->image_pages, ctx->image_npages);
	mutex_destroy(&ctx->io_lock);
	vfmig_pf_put(vfmig);
	kfree(ctx);
	return 0;
}

static const struct file_operations mlx5_vfmig_load_fops = {
	.owner		= THIS_MODULE,
	.write		= vfmig_load_write,
	.release	= vfmig_load_release,
};

/*
 * Set up a LOAD session on a VF already quiesced to STOP: resolve vhca_id,
 * allocate a PD, and hand back a write-only anon-inode fd. The staging
 * buffer + MKEY are allocated lazily once write() sees the record size,
 * and LOAD_VHCA_STATE runs when the payload completes. Caller holds
 * vfmig->lock for read.
 */
static long vfmig_ioc_load_vhca_state(struct mlx5_vfmig_pf *vfmig,
				      void __user *uarg)
{
	struct mlx5_core_dev *pf_mdev = vfmig->pf_mdev;
	struct mlx5_vfmig_load_state arg;
	struct mlx5_vfmig_load_ctx *ctx;
	struct mlx5_core_sriov *sriov;
	bool migratable = false;
	struct file *file;
	u16 vhca_id;
	int fd, err;

	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;
	if (arg.flags || arg.reserved)
		return -EINVAL;

	sriov = &pf_mdev->priv.sriov;
	if (arg.vf_id >= sriov->num_vfs)
		return -EINVAL;

	/* Firmware rejects LOAD unless the VHCA is fully suspended to STOP. */
	if (sriov->vfs_ctx[arg.vf_id].vfmig_dp_state != MLX5_VFMIG_DP_STOP) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: vf %u must be suspended to STOP before LOAD\n",
			       arg.vf_id);
		return -EINVAL;
	}

	err = vfmig_check_pf_migration_caps(pf_mdev);
	if (err)
		return err;

	err = vfmig_query_vf_migratable(pf_mdev, arg.vf_id, &migratable);
	if (err)
		return err;
	if (!migratable) {
		mlx5_core_warn(pf_mdev,
			       "vfmig: vf %u is not migration-enabled\n",
			       arg.vf_id);
		return -EOPNOTSUPP;
	}

	err = vfmig_query_vhca_id(pf_mdev, arg.vf_id + 1, &vhca_id);
	if (err)
		return err;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	INIT_LIST_HEAD(&ctx->node);
	mutex_init(&ctx->io_lock);
	ctx->vf_id = arg.vf_id;
	ctx->vhca_id = vhca_id;
	ctx->state = VFMIG_LS_READ_HEADER;

	/* Claim vf_id atomically vs other SAVE/LOAD sessions. */
	mutex_lock(&vfmig->ctxs_lock);
	if (vfmig_vf_id_busy_locked(vfmig, ctx->vf_id)) {
		mutex_unlock(&vfmig->ctxs_lock);
		err = -EBUSY;
		goto err_claim;
	}
	vfmig_pf_get(vfmig);
	ctx->vfmig = vfmig;
	list_add(&ctx->node, &vfmig->load_ctxs);
	mutex_unlock(&vfmig->ctxs_lock);

	err = mlx5_core_alloc_pd(pf_mdev, &ctx->pdn);
	if (err)
		goto err_res;
	ctx->pd_allocated = true;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		err = fd;
		goto err_res;
	}

	file = anon_inode_getfile("mlx5_vfmig_load", &mlx5_vfmig_load_fops,
				  ctx, O_WRONLY | O_CLOEXEC);
	if (IS_ERR(file)) {
		err = PTR_ERR(file);
		goto err_fd;
	}
	stream_open(file_inode(file), file);

	arg.load_fd = fd;
	if (copy_to_user(uarg, &arg, sizeof(arg))) {
		err = -EFAULT;
		goto err_file;
	}

	fd_install(fd, file);
	mlx5_core_info(pf_mdev,
		       "vfmig: LOAD session opened for vf %u (vhca_id 0x%04x)\n",
		       arg.vf_id, vhca_id);
	return 0;

err_file:
	/* fput() runs vfmig_load_release, which frees resources + ctx. */
	fput(file);
	put_unused_fd(fd);
	return err;
err_fd:
	put_unused_fd(fd);
err_res:
	vfmig_load_release_resources(ctx);
	vfmig_free_pages(ctx->image_pages, ctx->image_npages);
	mutex_lock(&vfmig->ctxs_lock);
	list_del(&ctx->node);
	mutex_unlock(&vfmig->ctxs_lock);
	vfmig_pf_put(vfmig);
	mutex_destroy(&ctx->io_lock);
	kfree(ctx);
	return err;
err_claim:
	mutex_destroy(&ctx->io_lock);
	kfree(ctx);
	return err;
}

static long vfmig_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct mlx5_vfmig_pf *vfmig = filp->private_data;
	void __user *uarg = (void __user *)arg;
	long ret;

	down_read(&vfmig->lock);
	if (vfmig->dead) {
		ret = -ENODEV;
		goto out;
	}

	switch (cmd) {
	case MLX5_VFMIG_IOC_MARK_RESTORED:
		ret = vfmig_ioc_mark_restored(vfmig, uarg);
		break;
	case MLX5_VFMIG_IOC_GET_VHCA_ID:
		ret = vfmig_ioc_get_vhca_id(vfmig, uarg);
		break;
	case MLX5_VFMIG_IOC_QUERY_VF:
		ret = vfmig_ioc_query_vf(vfmig, uarg);
		break;
	case MLX5_VFMIG_IOC_ENABLE_MIGRATABLE:
		ret = vfmig_ioc_enable_migratable(vfmig, uarg);
		break;
	case MLX5_VFMIG_IOC_SUSPEND_VHCA:
		ret = vfmig_ioc_suspend_vhca(vfmig, uarg);
		break;
	case MLX5_VFMIG_IOC_RESUME_VHCA:
		ret = vfmig_ioc_resume_vhca(vfmig, uarg);
		break;
	case MLX5_VFMIG_IOC_SAVE_VHCA_STATE:
		ret = vfmig_ioc_save_vhca_state(vfmig, uarg);
		break;
	case MLX5_VFMIG_IOC_LOAD_VHCA_STATE:
		ret = vfmig_ioc_load_vhca_state(vfmig, uarg);
		break;
	default:
		ret = -ENOTTY;
		break;
	}
out:
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
	mutex_init(&vfmig->ctxs_lock);
	INIT_LIST_HEAD(&vfmig->save_ctxs);
	INIT_LIST_HEAD(&vfmig->load_ctxs);
	vfmig->pf_mdev = pf_mdev;

	vfmig->max_vfs = pf_mdev->priv.sriov.max_vfs;
	if (vfmig->max_vfs) {
		vfmig->restored = bitmap_zalloc(vfmig->max_vfs, GFP_KERNEL);
		if (!vfmig->restored) {
			err = -ENOMEM;
			goto err_free;
		}
	}

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
	mutex_destroy(&vfmig->ctxs_lock);
	bitmap_free(vfmig->restored);
	kfree(vfmig);
	return err;
}

void mlx5_vfmig_pf_cleanup(struct mlx5_core_dev *pf_mdev)
{
	struct mlx5_vfmig_pf *vfmig = pf_mdev->priv.vfmig;
	struct mlx5_vfmig_save_ctx *save_ctx;
	struct mlx5_vfmig_load_ctx *load_ctx;
	dev_t devt;

	if (!vfmig)
		return;

	pf_mdev->priv.vfmig = NULL;

	/*
	 * Neuter the device. Tear down open SAVE/LOAD sessions' firmware
	 * resources (PD/MKEY/DMA) while pf_mdev is still alive; the page
	 * lists themselves are mdev-independent and get freed when each fd
	 * is later closed. The down_write blocks until all in-flight
	 * readers/writers drop their read locks, after which the session
	 * lists are stable.
	 */
	down_write(&vfmig->lock);
	vfmig->dead = true;
	list_for_each_entry(save_ctx, &vfmig->save_ctxs, node)
		vfmig_save_release_resources(save_ctx);
	list_for_each_entry(load_ctx, &vfmig->load_ctxs, node)
		vfmig_load_release_resources(load_ctx);
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
