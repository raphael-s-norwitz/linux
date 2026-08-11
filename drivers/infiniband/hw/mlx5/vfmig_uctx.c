// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * VFMIG (CRIU SR-IOV migration) per-ucontext vendor verbs for mlx5_ib.
 *
 * These run on the uverbs fd (per-process) and snapshot/restore the
 * minimum kernel-side state needed to reconstitute a process's UAR
 * mmap()s after the ucontext is recreated on a destination VHCA whose
 * firmware state was imported via LOAD_VHCA_STATE.
 *
 *   QUERY_UCONTEXT   snapshot bfregi->sys_pages[], bfregi->count[], and
 *                    meta into userspace.
 */

#include <rdma/uverbs_ioctl.h>
#include <rdma/uverbs_std_types.h>
#include <rdma/uverbs_types.h>
#include <rdma/mlx5_user_ioctl_cmds.h>
#include <rdma/mlx5_user_ioctl_verbs.h>
#include <linux/mlx5/driver.h>

#include "mlx5_ib.h"

#define UVERBS_MODULE_NAME mlx5_ib
#include <rdma/uverbs_named_ioctl.h>

static int UVERBS_HANDLER(MLX5_IB_METHOD_VFMIG_QUERY_UCONTEXT)(struct uverbs_attr_bundle *attrs)
{
	struct mlx5_ib_vfmig_ucontext_meta meta = {};
	struct mlx5_bfreg_info *bfregi;
	struct mlx5_ib_ucontext *c;
	bool want_uar_table;
	bool want_count;
	size_t want_len;
	size_t arr_len;
	int err = 0;

	c = to_mucontext(ib_uverbs_get_ucontext(attrs));
	if (IS_ERR(c))
		return PTR_ERR(c);

	mlx5_ib_dbg(to_mdev(c->ibucontext.device),
		    "VFMIG_QUERY_UCONTEXT: total_bfregs=%u num_sys_pages=%u num_static=%u\n",
		    c->bfregi.total_num_bfregs, c->bfregi.num_sys_pages,
		    c->bfregi.num_static_sys_pages);

	bfregi = &c->bfregi;

	/*
	 * lib_uar_dyn=true bypasses bfregi->sys_pages[] / count[] entirely
	 * (UARs are MLX5_IB_OBJECT_UAR uobjects with their own table). v0
	 * of the restore path doesn't cover that mode, so reject the QUERY
	 * here too -- mirror the alloc-path reject in mlx5_ib_alloc_ucontext.
	 */
	if (bfregi->lib_uar_dyn)
		return -EOPNOTSUPP;

	want_uar_table = uverbs_attr_is_valid(attrs,
					      MLX5_IB_ATTR_VFMIG_QUERY_UCONTEXT_UAR_TABLE);
	want_count = uverbs_attr_is_valid(attrs,
					  MLX5_IB_ATTR_VFMIG_QUERY_UCONTEXT_BFREG_COUNT);

	mutex_lock(&bfregi->lock);

	if (want_uar_table) {
		want_len = (size_t)bfregi->num_sys_pages *
			   sizeof(*bfregi->sys_pages);
		arr_len = uverbs_attr_get_len(attrs,
					      MLX5_IB_ATTR_VFMIG_QUERY_UCONTEXT_UAR_TABLE);
		if (arr_len != want_len) {
			err = -EINVAL;
			goto out;
		}
		err = uverbs_copy_to(attrs,
				     MLX5_IB_ATTR_VFMIG_QUERY_UCONTEXT_UAR_TABLE,
			bfregi->sys_pages, want_len);
		if (err)
			goto out;
	}

	if (want_count) {
		want_len = (size_t)bfregi->total_num_bfregs *
			   sizeof(*bfregi->count);
		arr_len = uverbs_attr_get_len(attrs,
					      MLX5_IB_ATTR_VFMIG_QUERY_UCONTEXT_BFREG_COUNT);
		if (arr_len != want_len) {
			err = -EINVAL;
			goto out;
		}
		err = uverbs_copy_to(attrs,
				     MLX5_IB_ATTR_VFMIG_QUERY_UCONTEXT_BFREG_COUNT,
			bfregi->count, want_len);
		if (err)
			goto out;
	}

	meta.num_static_sys_pages   = bfregi->num_static_sys_pages;
	meta.num_sys_pages          = bfregi->num_sys_pages;
	meta.num_dyn_bfregs         = bfregi->num_dyn_bfregs;
	meta.num_low_latency_bfregs = bfregi->num_low_latency_bfregs;
	meta.total_num_bfregs       = bfregi->total_num_bfregs;
	meta.lib_caps               = c->lib_caps;
	meta.lib_uar_4k             = bfregi->lib_uar_4k ? 1 : 0;
	meta.lib_uar_dyn            = bfregi->lib_uar_dyn ? 1 : 0;
	meta.cqe_version            = c->cqe_version;
	/*
	 * Source ucontext's FW owner-id, exposed across the SAVE/LOAD seam
	 * as image-only metadata / diagnostics (0 = non-DEVX). The default
	 * libmlx5 ucontext auto-allocates a DEVX uid, so this is commonly
	 * non-zero; RESTORE_UCONTEXT logs a mismatch and continues rather
	 * than rejecting (the restore path opens the dest ucontext without
	 * DEVX, so dest.devx_uid is 0 by construction).
	 */
	meta.devx_uid               = c->devx_uid;

	err = uverbs_copy_to(attrs,
			     MLX5_IB_ATTR_VFMIG_QUERY_UCONTEXT_META,
		&meta, sizeof(meta));

out:
	mutex_unlock(&bfregi->lock);
	return err;
}

DECLARE_UVERBS_NAMED_METHOD(
	MLX5_IB_METHOD_VFMIG_QUERY_UCONTEXT,
	UVERBS_ATTR_PTR_OUT(MLX5_IB_ATTR_VFMIG_QUERY_UCONTEXT_UAR_TABLE,
			    UVERBS_ATTR_MIN_SIZE(0),
			    UA_OPTIONAL),
	UVERBS_ATTR_PTR_OUT(MLX5_IB_ATTR_VFMIG_QUERY_UCONTEXT_BFREG_COUNT,
			    UVERBS_ATTR_MIN_SIZE(0),
			    UA_OPTIONAL),
	UVERBS_ATTR_PTR_OUT(MLX5_IB_ATTR_VFMIG_QUERY_UCONTEXT_META,
			    UVERBS_ATTR_TYPE(struct mlx5_ib_vfmig_ucontext_meta),
			    UA_MANDATORY));

DECLARE_UVERBS_GLOBAL_METHODS(
	MLX5_IB_OBJECT_VFMIG,
	&UVERBS_METHOD(MLX5_IB_METHOD_VFMIG_QUERY_UCONTEXT));

const struct uapi_definition mlx5_ib_vfmig_defs[] = {
	UAPI_DEF_CHAIN_OBJ_TREE_NAMED(MLX5_IB_OBJECT_VFMIG),
	{},
};
