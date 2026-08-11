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

static int UVERBS_HANDLER(MLX5_IB_METHOD_VFMIG_RESTORE_UCONTEXT)(struct uverbs_attr_bundle *attrs)
{
	struct mlx5_ib_vfmig_ucontext_meta meta;
	struct mlx5_bfreg_info *bfregi;
	struct mlx5_ib_ucontext *c;
	const u32 *uar_table;
	const u32 *bfreg_count;
	struct mlx5_ib_dev *dev;
	size_t want_uar_len;
	size_t want_count_len;
	size_t arr_len;
	bool have_count;
	size_t i;
	int err;
	u16 cnt_attr = MLX5_IB_ATTR_VFMIG_RESTORE_UCONTEXT_BFREG_COUNT;

	c = to_mucontext(ib_uverbs_get_ucontext(attrs));
	if (IS_ERR(c))
		return PTR_ERR(c);

	dev = to_mdev(c->ibucontext.device);
	bfregi = &c->bfregi;

	if (bfregi->lib_uar_dyn)
		return -EOPNOTSUPP;

	/*
	 * Precondition #1: ucontext must be in restore-pending state, i.e.
	 * opened with MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE so allocate_uars()
	 * left sys_pages[] sentinel-filled. Without it, sys_pages[] holds
	 * live FW UAR ids from real ALLOC_UAR commands and overwriting them
	 * would leak the allocations. Cleared below, so a second RESTORE
	 * on the same ucontext fails here.
	 */
	if (!c->vfmig_restore_pending) {
		mlx5_ib_dbg(dev,
			    "VFMIG_RESTORE_UCONTEXT: not in restore-pending state (missing MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE on open, or RESTORE already applied)\n");
		return -EINVAL;
	}

	err = uverbs_copy_from(&meta, attrs,
			       MLX5_IB_ATTR_VFMIG_RESTORE_UCONTEXT_META);
	if (err)
		return err;

	/*
	 * Precondition #2: strict bitwise equality on every UAR-shape-
	 * defining field. v0 targets a homogeneous fleet (same kernel, same
	 * libmlx5, same FW caps); any drift means the snapshot came from a
	 * structurally different ucontext and a verbatim UAR-id replay would
	 * land at the wrong bfregi indices.
	 *
	 * devx_uid is intentionally NOT in this strict-equality bag -- see
	 * the log-and-continue handling below.
	 */
	if (meta.num_static_sys_pages   != bfregi->num_static_sys_pages   ||
	    meta.num_sys_pages          != bfregi->num_sys_pages          ||
	    meta.num_dyn_bfregs         != bfregi->num_dyn_bfregs         ||
	    meta.num_low_latency_bfregs != bfregi->num_low_latency_bfregs ||
	    meta.total_num_bfregs       != bfregi->total_num_bfregs       ||
	    meta.lib_caps               != c->lib_caps                    ||
	    meta.lib_uar_4k             != (bfregi->lib_uar_4k ? 1 : 0)   ||
	    meta.lib_uar_dyn            != (bfregi->lib_uar_dyn ? 1 : 0)  ||
	    meta.cqe_version            != c->cqe_version) {
		mlx5_ib_dbg(dev,
			    "VFMIG_RESTORE_UCONTEXT: META mismatch (snapshot vs dst)\n");
		return -EINVAL;
	}

	/*
	 * Precondition #3b: devx_uid mismatch is LOG-AND-CONTINUE, not a
	 * hard reject. The default libmlx5 ucontext auto-allocates a fresh
	 * DEVX uid on every ibv_open_device, so source.devx_uid !=
	 * dest.devx_uid by construction for the v0 critical path -- rejecting
	 * it would reject the common case. The DEALLOC_PD "unknown PDN"
	 * failure is mitigated by the mlx5_ib_dealloc_pd vfmig_restored gate
	 * (independent of devx_uid match), cross-uid QP/CQ/MR destroys honor
	 * uid, and the standard-verbs data path (HW doorbells/CQEs) is
	 * uid-blind. DEVX-direct manipulation of restored objects remains out
	 * of scope for v0.
	 */
	if (meta.devx_uid != c->devx_uid)
		mlx5_ib_dbg(dev,
			    "VFMIG_RESTORE_UCONTEXT: devx_uid mismatch tolerated: snapshot=%u dest=%u\n",
			    (unsigned int)meta.devx_uid, c->devx_uid);

	want_uar_len = (size_t)bfregi->num_sys_pages *
		       sizeof(*bfregi->sys_pages);
	arr_len = uverbs_attr_get_len(attrs,
				      MLX5_IB_ATTR_VFMIG_RESTORE_UCONTEXT_UAR_TABLE);
	if (arr_len != want_uar_len) {
		mlx5_ib_dbg(dev,
			    "VFMIG_RESTORE_UCONTEXT: UAR_TABLE length %zu != expected %zu\n",
			    arr_len, want_uar_len);
		return -EINVAL;
	}
	uar_table = uverbs_attr_get_alloced_ptr(attrs,
						MLX5_IB_ATTR_VFMIG_RESTORE_UCONTEXT_UAR_TABLE);
	if (IS_ERR(uar_table))
		return PTR_ERR(uar_table);

	/*
	 * Precondition #3: static slots must all be valid FW UAR ids.
	 * Dynamic slots [num_static..num_sys_pages) MAY be INVALID (unclaimed
	 * on the source) -- those replay through uar_mmap()'s lazy-alloc.
	 */
	for (i = 0; i < bfregi->num_static_sys_pages; i++) {
		if (uar_table[i] == MLX5_IB_INVALID_UAR_INDEX) {
			mlx5_ib_dbg(dev,
				    "VFMIG_RESTORE_UCONTEXT: static slot %zu is INVALID in snapshot\n",
				    i);
			return -EINVAL;
		}
	}

	have_count = uverbs_attr_is_valid(attrs, cnt_attr);
	if (have_count) {
		want_count_len = (size_t)bfregi->total_num_bfregs *
				 sizeof(*bfregi->count);
		arr_len = uverbs_attr_get_len(attrs, cnt_attr);
		if (arr_len != want_count_len) {
			mlx5_ib_dbg(dev,
				    "VFMIG_RESTORE_UCONTEXT: BFREG_COUNT length %zu != expected %zu\n",
				    arr_len, want_count_len);
			return -EINVAL;
		}
		bfreg_count = uverbs_attr_get_alloced_ptr(attrs, cnt_attr);
		if (IS_ERR(bfreg_count))
			return PTR_ERR(bfreg_count);

		/*
		 * Precondition #4: v0 only restores "no live QPs / no claimed
		 * dyn UARs" snapshots. Non-zero count means the source had
		 * refs we don't yet rebuild. The wire shape is locked; a
		 * future MR/QP-restore step relaxes this without an ABI bump.
		 */
		for (i = 0; i < bfregi->total_num_bfregs; i++) {
			if (bfreg_count[i] != 0) {
				mlx5_ib_dbg(dev,
					    "VFMIG_RESTORE_UCONTEXT: BFREG_COUNT[%zu]=%u, v0 requires all-zero\n",
					    i, bfreg_count[i]);
				return -EINVAL;
			}
		}
	}

	/*
	 * All preconditions held. Apply under bfregi->lock; the lock also
	 * serializes against uar_mmap()'s lazy-alloc so a concurrent mmap
	 * cannot observe a half-seeded sys_pages[]. count[] is already zero
	 * from allocate_uars()'s kcalloc and v0 enforces all-zero on the
	 * wire, so there is nothing to copy for it yet.
	 */
	mutex_lock(&bfregi->lock);
	memcpy(bfregi->sys_pages, uar_table, want_uar_len);
	c->vfmig_restore_pending = false;
	mutex_unlock(&bfregi->lock);

	mlx5_ib_dbg(dev,
		    "VFMIG_RESTORE_UCONTEXT: seeded %u sys_pages, cleared restore-pending\n",
		    bfregi->num_sys_pages);
	return 0;
}

DECLARE_UVERBS_NAMED_METHOD(
	MLX5_IB_METHOD_VFMIG_RESTORE_UCONTEXT,
	UVERBS_ATTR_PTR_IN(MLX5_IB_ATTR_VFMIG_RESTORE_UCONTEXT_UAR_TABLE,
			   UVERBS_ATTR_MIN_SIZE(0),
			   UA_MANDATORY,
			   UA_ALLOC_AND_COPY),
	UVERBS_ATTR_PTR_IN(MLX5_IB_ATTR_VFMIG_RESTORE_UCONTEXT_BFREG_COUNT,
			   UVERBS_ATTR_MIN_SIZE(0),
			   UA_OPTIONAL,
			   UA_ALLOC_AND_COPY),
	UVERBS_ATTR_PTR_IN(MLX5_IB_ATTR_VFMIG_RESTORE_UCONTEXT_META,
			   UVERBS_ATTR_TYPE(struct mlx5_ib_vfmig_ucontext_meta),
			   UA_MANDATORY));

/*
 * QUERY_DYN_UARS: snapshot every outstanding MLX5_IB_OBJECT_UAR uobject
 * in this (lib_uar_dyn) ucontext. Two-pass: pass 1 (no RECORDS) reports
 * COUNT; pass 2 fills a COUNT-sized RECORDS array. We walk
 * ufile->uobjects (the per-fd committed-uobject list, the only place with
 * a well-defined iteration order) under ufile->uobjects_lock, filter by
 * object id, and copy {handle, uar_index, mmap_offset, alloc_type}.
 */
static int UVERBS_HANDLER(MLX5_IB_METHOD_VFMIG_QUERY_DYN_UARS)(struct uverbs_attr_bundle *attrs)
{
	struct mlx5_ib_vfmig_dyn_uar_record *records = NULL;
	struct mlx5_ib_ucontext *c;
	struct ib_uverbs_file *ufile;
	struct ib_uobject *uobj;
	struct mlx5_ib_dev *dev;
	bool want_records;
	size_t want_len;
	size_t arr_len;
	u32 nrecords;
	u32 count;
	int err;
	u16 rec_attr = MLX5_IB_ATTR_VFMIG_QUERY_DYN_UARS_RECORDS;

	c = to_mucontext(ib_uverbs_get_ucontext(attrs));
	if (IS_ERR(c))
		return PTR_ERR(c);

	dev = to_mdev(c->ibucontext.device);
	ufile = attrs->ufile;

	if (!c->bfregi.lib_uar_dyn) {
		mlx5_ib_dbg(dev,
			    "VFMIG_QUERY_DYN_UARS: ucontext is not lib_uar_dyn=true; use VFMIG_QUERY_UCONTEXT\n");
		return -EINVAL;
	}

	want_records = uverbs_attr_is_valid(attrs, rec_attr);
	if (want_records) {
		records = uverbs_zalloc(attrs,
					uverbs_attr_get_len(attrs, rec_attr));
		if (IS_ERR(records))
			return PTR_ERR(records);
	}

	count = 0;
	spin_lock_irq(&ufile->uobjects_lock);
	list_for_each_entry(uobj, &ufile->uobjects, list) {
		struct mlx5_user_mmap_entry *entry;

		if (uobj_get_object_id(uobj) != MLX5_IB_OBJECT_UAR)
			continue;
		entry = uobj->object;
		if (!entry)
			continue;

		if (!want_records) {
			count++;
			continue;
		}

		/*
		 * Bail out cleanly if RECORDS is shorter than what we would
		 * emit (also cross-checked against the final count below).
		 */
		nrecords = uverbs_attr_get_len(attrs, rec_attr) /
			sizeof(*records);
		if (count >= nrecords) {
			spin_unlock_irq(&ufile->uobjects_lock);
			return -EINVAL;
		}

		records[count].handle = uobj->id;
		records[count].uar_index = entry->page_idx;
		/*
		 * Emit the libmlx5-wire-format mmap_offset (what UAR_OBJ_ALLOC
		 * reports and userspace mmap()s), so captured values round-trip
		 * byte-for-byte and are directly usable with mmap() after
		 * restore.
		 */
		records[count].mmap_offset = mlx5_entry_to_mmap_offset(entry);
		switch (entry->mmap_flag) {
		case MLX5_IB_MMAP_TYPE_UAR_WC:
			records[count].alloc_type =
				MLX5_IB_UAPI_UAR_ALLOC_TYPE_BF;
			break;
		case MLX5_IB_MMAP_TYPE_UAR_NC:
			records[count].alloc_type =
				MLX5_IB_UAPI_UAR_ALLOC_TYPE_NC;
			break;
		default:
			/*
			 * MLX5_IB_OBJECT_UAR uobjects only ever carry
			 * UAR_WC / UAR_NC mmap_flags; anything else is a bug.
			 */
			spin_unlock_irq(&ufile->uobjects_lock);
			mlx5_ib_dbg(dev,
				    "VFMIG_QUERY_DYN_UARS: unexpected mmap_flag=%u on UAR handle=%u\n",
				    entry->mmap_flag, uobj->id);
			return -EIO;
		}
		count++;
	}
	spin_unlock_irq(&ufile->uobjects_lock);

	if (want_records) {
		want_len = (size_t)count * sizeof(*records);
		arr_len = uverbs_attr_get_len(attrs, rec_attr);
		if (arr_len != want_len) {
			mlx5_ib_dbg(dev,
				    "VFMIG_QUERY_DYN_UARS: RECORDS length %zu != expected %zu (count=%u)\n",
				    arr_len, want_len, count);
			return -EINVAL;
		}
		err = uverbs_copy_to(attrs, rec_attr, records, want_len);
		if (err)
			return err;
	}

	err = uverbs_copy_to(attrs, MLX5_IB_ATTR_VFMIG_QUERY_DYN_UARS_COUNT,
			     &count, sizeof(count));
	if (err)
		return err;

	mlx5_ib_dbg(dev, "VFMIG_QUERY_DYN_UARS: %s pass, %u dyn UAR(s)\n",
		    want_records ? "snapshot" : "sizing", count);
	return 0;
}

DECLARE_UVERBS_NAMED_METHOD(
	MLX5_IB_METHOD_VFMIG_QUERY_DYN_UARS,
	UVERBS_ATTR_PTR_OUT(MLX5_IB_ATTR_VFMIG_QUERY_DYN_UARS_RECORDS,
			    UVERBS_ATTR_MIN_SIZE(0),
			    UA_OPTIONAL),
	UVERBS_ATTR_PTR_OUT(MLX5_IB_ATTR_VFMIG_QUERY_DYN_UARS_COUNT,
			    UVERBS_ATTR_TYPE(__u32),
			    UA_MANDATORY));

/*
 * Single-record dyn-UAR restore worker. Allocates and pins the
 * MLX5_IB_OBJECT_UAR uobject at @rec->handle, then attaches an
 * mlx5_user_mmap_entry whose start_pgoff matches @rec->mmap_offset.
 *
 * Failure modes:
 *   - rdma_alloc_begin_uobject_at_handle() returning -EBUSY: handle
 *     already in use by something else in this ucontext (concurrent
 *     UAR_OBJ_ALLOC, or duplicate handle in the snapshot). Surface as
 *     -EBUSY; the snapshot is malformed and the caller should fail.
 *   - restore_uar_entry() returning -EBUSY: mmap pgoff already in use
 *     (range collision with another mmap_entry in this ucontext). v0
 *     does not attempt to relocate.
 *
 * On error before commit: rdma_alloc_abort_uobject() drops the pinned
 * idr slot and the rdmacg charge. After commit, the uobject is owned
 * by the ufile and will be torn down on ucontext close.
 */
static int vfmig_restore_one_dyn_uar(struct uverbs_attr_bundle *attrs,
				     struct mlx5_ib_ucontext *c,
				     const struct mlx5_ib_vfmig_dyn_uar_record *rec)
{
	struct mlx5_ib_dev *dev = to_mdev(c->ibucontext.device);
	struct mlx5_user_mmap_entry *entry;
	struct ib_uobject *uobj;
	u32 mmap_pgoff;
	int err;

	if (rec->alloc_type != MLX5_IB_UAPI_UAR_ALLOC_TYPE_BF &&
	    rec->alloc_type != MLX5_IB_UAPI_UAR_ALLOC_TYPE_NC) {
		mlx5_ib_dbg(dev,
			    "VFMIG_RESTORE_DYN_UARS: unsupported alloc_type=%u for handle=%u\n",
			    rec->alloc_type, rec->handle);
		return -EINVAL;
	}

	if (rec->mmap_offset & ~PAGE_MASK) {
		mlx5_ib_dbg(dev,
			    "VFMIG_RESTORE_DYN_UARS: unaligned mmap_offset=0x%llx for handle=%u\n",
			    (unsigned long long)rec->mmap_offset, rec->handle);
		return -EINVAL;
	}
	/*
	 * QUERY_DYN_UARS emits the libmlx5-wire-format mmap_offset (what
	 * userspace mmap()s); rdma_user_mmap_entry_insert_exact() needs
	 * the rdma_user_mmap_entry start_pgoff. Run the inverse codec.
	 */
	mmap_pgoff = mlx5_mmap_offset_to_pgoff(rec->mmap_offset);
	if (mmap_pgoff == U32_MAX) {
		mlx5_ib_dbg(dev,
			    "VFMIG_RESTORE_DYN_UARS: out-of-range mmap_offset=0x%llx for handle=%u\n",
			    (unsigned long long)rec->mmap_offset, rec->handle);
		return -EINVAL;
	}

	uobj = rdma_alloc_begin_uobject_at_handle(attrs,
						  MLX5_IB_OBJECT_UAR,
						  rec->handle);
	if (IS_ERR(uobj)) {
		err = PTR_ERR(uobj);
		mlx5_ib_dbg(dev,
			    "VFMIG_RESTORE_DYN_UARS: alloc_at_handle(%u) failed: %d\n",
			    rec->handle, err);
		return err;
	}

	entry = restore_uar_entry(c, rec->alloc_type, rec->uar_index,
				  mmap_pgoff);
	if (IS_ERR(entry)) {
		err = PTR_ERR(entry);
		mlx5_ib_dbg(dev,
			    "VFMIG_RESTORE_DYN_UARS: restore_uar_entry(handle=%u uar=%u pgoff=%u) failed: %d\n",
			    rec->handle, rec->uar_index, mmap_pgoff, err);
		/*
		 * No HW object yet (mmap_entry not inserted, no
		 * mlx5_cmd_uar_alloc was issued -- restore path skips it).
		 */
		rdma_alloc_abort_uobject(uobj, attrs, /* hw_obj_valid */ false);
		return err;
	}

	uobj->object = entry;
	rdma_alloc_commit_uobject(uobj, attrs);

	mlx5_ib_dbg(dev,
		    "VFMIG_RESTORE_DYN_UARS: restored handle=%u uar=%u pgoff=%u alloc_type=%u\n",
		    rec->handle, rec->uar_index, mmap_pgoff, rec->alloc_type);
	return 0;
}

/*
 * RESTORE_DYN_UARS -- replay a QUERY_DYN_UARS snapshot onto a destination
 * lib_uar_dyn=true ucontext, recreating each MLX5_IB_OBJECT_UAR uobject at
 * its source handle/offset. Single-shot: consumes vfmig_restore_pending.
 */
static int UVERBS_HANDLER(MLX5_IB_METHOD_VFMIG_RESTORE_DYN_UARS)(struct uverbs_attr_bundle *attrs)
{
	const struct mlx5_ib_vfmig_dyn_uar_record *records;
	struct mlx5_ib_ucontext *c;
	struct mlx5_ib_dev *dev;
	size_t arr_len;
	u32 nrecords;
	u32 i;
	int err;
	u16 rec_attr = MLX5_IB_ATTR_VFMIG_RESTORE_DYN_UARS_RECORDS;

	c = to_mucontext(ib_uverbs_get_ucontext(attrs));
	if (IS_ERR(c))
		return PTR_ERR(c);

	dev = to_mdev(c->ibucontext.device);

	/*
	 * Mirrors the precondition set in RESTORE_UCONTEXT:
	 *  - vfmig_restore_pending must be set (ucontext was opened with
	 *    MLX5_IB_ALLOC_UCTX_VFMIG_RESTORE; this is the single-shot
	 *    flag that prevents repeated restores from double-creating
	 *    uobjects);
	 *  - lib_uar_dyn must be true (a static-UAR ucontext uses
	 *    RESTORE_UCONTEXT instead).
	 */
	if (!c->vfmig_restore_pending) {
		mlx5_ib_dbg(dev,
			    "VFMIG_RESTORE_DYN_UARS: ucontext not in restore-pending state\n");
		return -EINVAL;
	}
	if (!c->bfregi.lib_uar_dyn) {
		mlx5_ib_dbg(dev,
			    "VFMIG_RESTORE_DYN_UARS: ucontext is not lib_uar_dyn=true; use VFMIG_RESTORE_UCONTEXT\n");
		return -EINVAL;
	}

	arr_len = uverbs_attr_get_len(attrs, rec_attr);
	if (!arr_len || arr_len % sizeof(*records)) {
		mlx5_ib_dbg(dev,
			    "VFMIG_RESTORE_DYN_UARS: bad RECORDS length %zu (record size %zu)\n",
			    arr_len, sizeof(*records));
		return -EINVAL;
	}
	nrecords = arr_len / sizeof(*records);

	records = uverbs_attr_get_alloced_ptr(attrs, rec_attr);
	if (IS_ERR(records))
		return PTR_ERR(records);

	/*
	 * Bail-on-first-error semantics. We could attempt to roll back
	 * already-committed uobjects on a partial failure, but in
	 * practice a partial RESTORE_DYN_UARS failure means the
	 * destination ucontext is unusable for the migrated workload --
	 * the userspace caller will close the fd and start over. We log
	 * loudly so the leftover uobjects (which will be torn down on
	 * fd close anyway) are easy to diagnose.
	 */
	for (i = 0; i < nrecords; i++) {
		err = vfmig_restore_one_dyn_uar(attrs, c, &records[i]);
		if (err) {
			mlx5_ib_dbg(dev,
				    "VFMIG_RESTORE_DYN_UARS: failed at record %u/%u: %d (close ucontext to retry)\n",
				    i, nrecords, err);
			return err;
		}
	}

	c->vfmig_restore_pending = false;
	mlx5_ib_dbg(dev,
		    "VFMIG_RESTORE_DYN_UARS: restored %u dyn UAR(s), cleared restore-pending\n",
		    nrecords);
	return 0;
}

DECLARE_UVERBS_NAMED_METHOD(
	MLX5_IB_METHOD_VFMIG_RESTORE_DYN_UARS,
	UVERBS_ATTR_PTR_IN(MLX5_IB_ATTR_VFMIG_RESTORE_DYN_UARS_RECORDS,
			   UVERBS_ATTR_MIN_SIZE(0),
			   UA_MANDATORY,
			   UA_ALLOC_AND_COPY));

DECLARE_UVERBS_GLOBAL_METHODS(
	MLX5_IB_OBJECT_VFMIG,
	&UVERBS_METHOD(MLX5_IB_METHOD_VFMIG_QUERY_UCONTEXT),
	&UVERBS_METHOD(MLX5_IB_METHOD_VFMIG_RESTORE_UCONTEXT),
	&UVERBS_METHOD(MLX5_IB_METHOD_VFMIG_QUERY_DYN_UARS),
	&UVERBS_METHOD(MLX5_IB_METHOD_VFMIG_RESTORE_DYN_UARS));

const struct uapi_definition mlx5_ib_vfmig_defs[] = {
	UAPI_DEF_CHAIN_OBJ_TREE_NAMED(MLX5_IB_OBJECT_VFMIG),
	{},
};
