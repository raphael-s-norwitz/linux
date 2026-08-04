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

#endif /* _UAPI_LINUX_MLX5_VFMIG_H */
