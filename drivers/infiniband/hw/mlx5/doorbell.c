/*
 * Copyright (c) 2013-2015, Mellanox Technologies. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <linux/kref.h>
#include <linux/slab.h>
#include <linux/sched/mm.h>
#include <rdma/ib_umem.h>

#include "mlx5_ib.h"

#define MLX5_IB_DBR_SIZE (sizeof(__be32) * 2)

struct mlx5_ib_user_db_page {
	struct list_head	list;
	struct ib_umem	       *umem;
	struct ib_uverbs_buffer_desc desc;
	int			refcnt;
	struct mm_struct	*mm;
};

static int mlx5_ib_db_map_user_desc(struct mlx5_ib_ucontext *context,
				    const struct ib_uverbs_buffer_desc *desc,
				    struct mlx5_db *db)
{
	struct mlx5_ib_dev *dev = to_mdev(context->ibucontext.device);
	struct mlx5_ib_user_db_page *page;
	struct ib_umem *umem;
	int err = 0;

	if (desc->length < MLX5_IB_DBR_SIZE)
		return -EINVAL;
	/*
	 * For VA descriptors the umem is normalized to a single PAGE_SIZE
	 * region, so reject offsets that would place the 8-byte DBR
	 * straddling the page boundary.
	 */
	if (desc->type == IB_UVERBS_BUFFER_TYPE_VA &&
	    (desc->addr & ~PAGE_MASK) > PAGE_SIZE - MLX5_IB_DBR_SIZE)
		return -EINVAL;

	mutex_lock(&context->db_page_mutex);

	/*
	 * Only VA-typed descriptors are eligible to share a per-page
	 * doorbell umem; FD-typed descriptors are pinned individually.
	 */
	if (desc->type == IB_UVERBS_BUFFER_TYPE_VA) {
		list_for_each_entry(page, &context->db_page_list, list) {
			if (current->mm != page->mm)
				continue;
			if (page->desc.addr == (desc->addr & PAGE_MASK))
				goto found;
		}
	}

	page = kzalloc_obj(*page);
	if (!page) {
		err = -ENOMEM;
		goto out;
	}

	page->desc = *desc;

	/*
	 * Normalize VA descriptors to a page-aligned PAGE_SIZE region so
	 * multiple DBRs that fall in the same user page share one umem.
	 */
	if (page->desc.type == IB_UVERBS_BUFFER_TYPE_VA) {
		page->desc.addr &= PAGE_MASK;
		page->desc.length = PAGE_SIZE;
	}

	umem = ib_umem_get_desc(context->ibucontext.device, &page->desc, 0);
	if (IS_ERR(umem)) {
		err = PTR_ERR(umem);
		kfree(page);
		goto out;
	}

	/*
	 * The 8-byte DBR is programmed to the device as one DMA address,
	 * so it must live in a single contiguous DMA segment.
	 */
	if (!ib_umem_is_contiguous(umem)) {
		ib_umem_release(umem);
		kfree(page);
		err = -EINVAL;
		goto out;
	}

	page->umem = umem;
	if (page->desc.type == IB_UVERBS_BUFFER_TYPE_VA) {
		mmgrab(current->mm);
		page->mm = current->mm;
	}
	list_add(&page->list, &context->db_page_list);

	/*
	 * Source-side vfmig retag (mirrors create_real_mr). Only the miss
	 * branch retags: the ib_umem_get above is the sole path that hands a
	 * fresh PAGE_SIZE umem to vfmig_dma_ops.map_sg, planting the
	 * KIND_NONE registry entry; the 'found' branch just refcounts an
	 * already-retagged page. The key's fw_id is the page-aligned user VA
	 * (mlx5_ib_db_map_user's own dedup key), not a FW id -- doorbell
	 * pages have no FW identity. Non-fatal on error: the CQ/QP/SRQ
	 * create that triggered this mapping continues; the doorbell page is
	 * usable, just not CRIU-restorable.
	 */
	if (dev->mdev->cmd.vfmig_iova_dom &&
	    page->desc.type == IB_UVERBS_BUFFER_TYPE_VA) {
		struct sg_table *sgt = &page->umem->sgt_append.sgt;
		dma_addr_t iova_base = sg_dma_address(sgt->sgl) & PAGE_MASK;
		unsigned long user_virt = (unsigned long)page->desc.addr;
		int retag_err;

		retag_err = mlx5_vfmig_retag_user_dbr(dev->mdev, user_virt,
						      iova_base, PAGE_SIZE);
		if (retag_err)
			mlx5_ib_warn(dev,
				     "vfmig: source-side retag for DBR page failed: user_virt=0x%lx iova_base=0x%llx err=%d -- DBR page usable but not CRIU-restorable\n",
				     user_virt, (u64)iova_base, retag_err);
	}

found:
	db->dma = sg_dma_address(page->umem->sgt_append.sgt.sgl) +
		  (desc->addr & ~PAGE_MASK);
	db->u.user_page = page;
	++page->refcnt;

out:
	mutex_unlock(&context->db_page_mutex);

	return err;
}

int mlx5_ib_db_map_user(struct mlx5_ib_ucontext *context,
			const struct uverbs_attr_bundle *attrs, u16 attr_id,
			unsigned long virt, struct mlx5_db *db)
{
	struct ib_uverbs_buffer_desc desc = {
		.type = IB_UVERBS_BUFFER_TYPE_VA,
		.addr = virt,
		.length = MLX5_IB_DBR_SIZE,
	};

	if (attrs) {
		int err;

		err = uverbs_get_buffer_desc(attrs, attr_id, &desc);
		if (err && err != -ENOENT)
			return err;
	}

	return mlx5_ib_db_map_user_desc(context, &desc, db);
}

void mlx5_ib_db_unmap_user(struct mlx5_ib_ucontext *context, struct mlx5_db *db)
{
	mutex_lock(&context->db_page_mutex);

	if (!--db->u.user_page->refcnt) {
		list_del(&db->u.user_page->list);
		if (db->u.user_page->mm)
			mmdrop(db->u.user_page->mm);
		ib_umem_release(db->u.user_page->umem);
		kfree(db->u.user_page);
	}

	mutex_unlock(&context->db_page_mutex);
}

/*
 * Read-only accessor for the page-aligned source userspace VA of the
 * doorbell-page user_page that backs @db. Returns 0 for kernel-mode db
 * slots (the db->u.pgdir branch -- no user_page). Used by
 * MLX5_IB_METHOD_VFMIG_QUERY_CQ (and future per-uobject QUERY verbs) to
 * round-trip the source VA into struct mlx5_ib_restore_*_req.db_addr
 * without exposing struct mlx5_ib_user_db_page across translation units.
 *
 * No locking: db->u.user_page is set once at uobject create / restore
 * time and cleared in mlx5_ib_db_unmap_user only after the parent
 * uobject's destroy verb has serialised against other users; callers
 * reach @db through a UVERBS_ACCESS_READ-pinned uobject.
 */
u64 mlx5_ib_db_user_virt(const struct mlx5_db *db)
{
	const struct mlx5_ib_user_db_page *page = db->u.user_page;

	/*
	 * This kernel keys doorbell pages by struct ib_uverbs_buffer_desc
	 * rather than a bare user VA. Only VA-typed descriptors carry a
	 * page-aligned source userspace VA (see mlx5_ib_db_map_user_desc's
	 * normalization); FD-typed (dmabuf) pages have none, so report 0.
	 */
	if (!page || page->desc.type != IB_UVERBS_BUFFER_TYPE_VA)
		return 0;
	return (u64)page->desc.addr;
}

/*
 * Restore-time variant of mlx5_ib_db_map_user. The fast-path (a
 * previously-restored uobject in the same ucontext already pinned this
 * DBR page) is identical to mlx5_ib_db_map_user: refcount up and reuse
 * db->u.user_page / db->dma. The miss-path differs in two ways:
 *
 *   1. ib_umem_pin() (no DMA mapping) instead of ib_umem_get(): at
 *      restore time the IOVA must match what LOAD_VHCA_STATE installed
 *      for the placeholder, which mlx5_vfmig_bind_user_dbr applies
 *      synchronously (same asymmetry as mlx5_ib_umem_restore_mr vs the
 *      create-time mlx5_ib_reg_user_mr).
 *   2. mlx5_vfmig_bind_user_dbr() instead of relying on
 *      mlx5_ib_db_map_user's source-side retag. It consumes the
 *      awaiting_bind placeholder LOAD replayed for
 *      VFMIG_HUOBJ_KEY(DBR, virt & PAGE_MASK) -- the key the SAVE-side
 *      mlx5_vfmig_retag_user_dbr emitted.
 *
 * The bind is a hard prerequisite for FW data-path use of the doorbell
 * (the FW writes doorbell records via the IOVA in cqc.dbr_addr /
 * qpc.dbr_umem_id), so bind failure propagates to the verb body and
 * aborts the restore. The resulting db is matched 1-to-1 by the
 * standard mlx5_ib_db_unmap_user (no parallel restore variant needed).
 */
int mlx5_ib_db_map_user_restore(struct mlx5_ib_ucontext *context,
				unsigned long virt, struct mlx5_db *db)
{
	struct ib_uverbs_buffer_desc desc = {
		.type = IB_UVERBS_BUFFER_TYPE_VA,
		.addr = virt & PAGE_MASK,
		.length = PAGE_SIZE,
	};
	struct mlx5_ib_dev *dev = to_mdev(context->ibucontext.device);
	struct mlx5_ib_user_db_page *page;
	int err = 0;

	mutex_lock(&context->db_page_mutex);

	list_for_each_entry(page, &context->db_page_list, list)
		if (current->mm == page->mm &&
		    page->desc.type == IB_UVERBS_BUFFER_TYPE_VA &&
		    page->desc.addr == (virt & PAGE_MASK))
			goto found;

	page = kzalloc(sizeof(*page), GFP_KERNEL);
	if (!page) {
		err = -ENOMEM;
		goto out;
	}

	page->desc   = desc;
	page->refcnt = 0;
	page->umem = ib_umem_pin(context->ibucontext.device, virt & PAGE_MASK,
				 PAGE_SIZE, 0);
	if (IS_ERR(page->umem)) {
		err = PTR_ERR(page->umem);
		kfree(page);
		goto out;
	}

	err = mlx5_vfmig_bind_user_dbr(dev->mdev, page->desc.addr,
				       &page->umem->sgt_append.sgt);
	if (err) {
		ib_umem_release(page->umem);
		kfree(page);
		goto out;
	}

	mmgrab(current->mm);
	page->mm = current->mm;

	list_add(&page->list, &context->db_page_list);

found:
	db->dma = sg_dma_address(page->umem->sgt_append.sgt.sgl) +
		  (virt & ~PAGE_MASK);
	db->u.user_page = page;
	++page->refcnt;

out:
	mutex_unlock(&context->db_page_mutex);

	return err;
}
