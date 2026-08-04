/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *
 * UAPI for mlx5 host-driven VF migration / CRIU restore.
 *
 * One char device per mlx5_core PF, exposed as /dev/mlx5_vfmig/<bdf>,
 * lets the host PF introspect and migrate the firmware state of its
 * SR-IOV VFs without going through a guest VM. This header grows one
 * command at a time as the kernel side lands each piece of
 * functionality; it currently covers only PF-side introspection.
 *
 * All commands address a VF by its @vf_id, the SR-IOV VF index in
 * 0..num_vfs-1 (i.e. the virtfn<vf_id> under the PF).
 */

#ifndef _UAPI_LINUX_MLX5_VFMIG_H
#define _UAPI_LINUX_MLX5_VFMIG_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define MLX5_VFMIG_IOC_MAGIC	0xB5

/*
 * MLX5_VFMIG_IOC_MARK_RESTORED:
 *   Latch VF @vf_id as having had its firmware state restored. A later
 *   mlx5_core probe of that VF consumes the bit to skip re-init of state
 *   that was loaded out of band. Returns 0 on success, -EINVAL if @vf_id
 *   is out of range or @reserved is non-zero, -EALREADY if the bit was
 *   already set.
 */
struct mlx5_vfmig_mark_restored {
	__u32 vf_id;	/* in */
	__u32 reserved;
};

#define MLX5_VFMIG_IOC_MARK_RESTORED \
	_IOW(MLX5_VFMIG_IOC_MAGIC, 0x01, struct mlx5_vfmig_mark_restored)

/*
 * MLX5_VFMIG_IOC_GET_VHCA_ID:
 *   PF-side query of the VF's vhca_id via QUERY_HCA_CAP(other_function=1).
 *   Lets userspace confirm the PF can address the VF without binding any
 *   driver to it. Returns 0 on success, -EINVAL if @vf_id is out of range
 *   or @reserved is non-zero.
 */
struct mlx5_vfmig_get_vhca_id {
	__u32 vf_id;	/* in  */
	__u16 vhca_id;	/* out */
	__u16 reserved;
};

#define MLX5_VFMIG_IOC_GET_VHCA_ID \
	_IOWR(MLX5_VFMIG_IOC_MAGIC, 0x02, struct mlx5_vfmig_get_vhca_id)

/*
 * MLX5_VFMIG_IOC_QUERY_VF:
 *   Diagnostic snapshot of one VF on the owning PF: its live vhca_id
 *   (queried via QUERY_HCA_CAP(other_function=1)), the "restored" bit
 *   latched on the PF, and the total number of VFs provisioned.
 *   Userspace iterates @vf_id 0..num_vfs-1 to enumerate. Returns 0 on
 *   success, or -ERANGE if @vf_id >= num_vfs (with @num_vfs still filled
 *   in so callers can size their iteration).
 */
struct mlx5_vfmig_query_vf {
	__u32 vf_id;		/* in  */
	__u32 num_vfs;		/* out: total VFs provisioned on this PF */
	__u16 vhca_id;		/* out */
	__u8  restored;		/* out: 1 if MARK_RESTORED was issued */
	__u8  reserved;
};

#define MLX5_VFMIG_IOC_QUERY_VF \
	_IOWR(MLX5_VFMIG_IOC_MAGIC, 0x03, struct mlx5_vfmig_query_vf)

/*
 * MLX5_VFMIG_IOC_ENABLE_MIGRATABLE:
 *   Set the per-VF cmd_hca_cap_2.migratable bit, the firmware gate for
 *   SUSPEND/SAVE/LOAD/RESUME. This MUST be called while the VF is
 *   unbound (no driver attached): firmware accepts the modify-cap on a
 *   VHCA in pre-ENABLE_HCA state but rejects it on one mlx5_core has
 *   already probed.
 *
 *   Idempotent: returns 0 with no firmware traffic if the bit is
 *   already set. Returns -EOPNOTSUPP if the PF firmware does not
 *   advertise migration / vhca_resource_manager, -EINVAL if @vf_id is
 *   out of range or @reserved is non-zero. The bit is intentionally
 *   left set across mlx5_core probes.
 */
struct mlx5_vfmig_enable_migratable {
	__u32 vf_id;	/* in */
	__u32 reserved;
};

#define MLX5_VFMIG_IOC_ENABLE_MIGRATABLE \
	_IOW(MLX5_VFMIG_IOC_MAGIC, 0x06, struct mlx5_vfmig_enable_migratable)

#endif /* _UAPI_LINUX_MLX5_VFMIG_H */
