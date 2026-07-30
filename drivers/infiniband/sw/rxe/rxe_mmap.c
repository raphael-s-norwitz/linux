// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * Copyright (c) 2016 Mellanox Technologies Ltd. All rights reserved.
 * Copyright (c) 2015 System Fabric Works, Inc. All rights reserved.
 */

#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/errno.h>
#include <rdma/uverbs_ioctl.h>

#include "rxe.h"
#include "rxe_loc.h"

void rxe_mmap_release(struct kref *ref)
{
	struct rxe_mmap_info *ip = container_of(ref,
					struct rxe_mmap_info, ref);
	struct rxe_dev *rxe = to_rdev(ip->context->device);

	spin_lock_bh(&rxe->pending_lock);

	if (!list_empty(&ip->pending_mmaps))
		list_del(&ip->pending_mmaps);

	spin_unlock_bh(&rxe->pending_lock);

	vfree(ip->obj);		/* buf */
	kfree(ip);
}

/**
 * rxe_mmap - create a new mmap region
 * @context: the IB user context of the process making the mmap() call
 * @vma: the VMA to be initialized
 * Return zero if the mmap is OK. Otherwise, return an errno.
 */
int rxe_mmap(struct ib_ucontext *context, struct vm_area_struct *vma)
{
	struct rxe_dev *rxe = to_rdev(context->device);
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
	unsigned long size = vma->vm_end - vma->vm_start;
	struct rxe_mmap_info *ip, *pp;
	int ret;

	/*
	 * Search the device's list of objects waiting for a mmap call.
	 * Normally, this list is very short since a call to create a
	 * CQ, QP, or SRQ is soon followed by a call to mmap().
	 */
	spin_lock_bh(&rxe->pending_lock);
	list_for_each_entry_safe(ip, pp, &rxe->pending_mmaps, pending_mmaps) {
		if (context != ip->context || (__u64)offset != ip->info.offset)
			continue;

		/* Don't allow a mmap larger than the object. */
		if (size > ip->info.size) {
			rxe_dbg_dev(rxe, "mmap region is larger than the object!\n");
			spin_unlock_bh(&rxe->pending_lock);
			ret = -EINVAL;
			goto done;
		}

		goto found_it;
	}
	rxe_dbg_dev(rxe, "unable to find pending mmap info\n");
	spin_unlock_bh(&rxe->pending_lock);
	ret = -EINVAL;
	goto done;

found_it:
	/*
	 * Increment refcount and check whether it is being freed atm while
	 * holding lock to prevent UAF
	 */
	if (!kref_get_unless_zero(&ip->ref)) {
		spin_unlock_bh(&rxe->pending_lock);
		ret = -ENXIO;
		goto done;
	}

	list_del_init(&ip->pending_mmaps);
	spin_unlock_bh(&rxe->pending_lock);

	ret = remap_vmalloc_range(vma, ip->obj, 0);
	kref_put(&ip->ref, rxe_mmap_release);
	if (ret)
		rxe_dbg_dev(rxe, "err %d from remap_vmalloc_range\n", ret);

done:
	return ret;
}

/*
 * Allocate information for rxe_mmap.
 *
 * @forced_offset: if non-zero, bind the new mmap region at exactly
 * this vm_pgoff value instead of allocating a fresh one from the
 * monotonic counter. Used by the CRIU-restore path
 * (UVERBS_METHOD_RESTORE_CQ + struct rxe_restore_cq_req) so the
 * destination's mminfo.offset equals the source-side value the
 * pie restorer is mmap()ing against. Returns -EEXIST if a sibling
 * pending mmap already holds that offset.
 *
 * The collision check, offset claim, and pending_mmaps insertion
 * all happen under pending_lock to keep two concurrent forced-
 * offset allocators from both observing an empty list before
 * either of them adds. Lock order: pending_lock -> mmap_offset_lock
 * (mmap_offset_lock is leaf).
 */
struct rxe_mmap_info *rxe_create_mmap_info(struct rxe_dev *rxe, u32 size,
					   struct ib_udata *udata, void *obj,
					   u64 forced_offset)
{
	struct rxe_mmap_info *ip;
	struct rxe_mmap_info *cur;

	if (!udata)
		return ERR_PTR(-EINVAL);

	ip = kmalloc_obj(*ip);
	if (!ip)
		return ERR_PTR(-ENOMEM);

	size = PAGE_ALIGN(size);

	INIT_LIST_HEAD(&ip->pending_mmaps);
	ip->info.size = size;
	ip->context =
		container_of(udata, struct uverbs_attr_bundle, driver_udata)
			->context;
	ip->obj = obj;
	kref_init(&ip->ref);

	spin_lock_bh(&rxe->pending_lock);
	spin_lock_bh(&rxe->mmap_offset_lock);

	if (rxe->mmap_offset == 0)
		rxe->mmap_offset = ALIGN(PAGE_SIZE, SHMLBA);

	if (forced_offset) {
		list_for_each_entry(cur, &rxe->pending_mmaps, pending_mmaps) {
			if (cur->info.offset == forced_offset) {
				spin_unlock_bh(&rxe->mmap_offset_lock);
				spin_unlock_bh(&rxe->pending_lock);
				kfree(ip);
				return ERR_PTR(-EEXIST);
			}
		}
		ip->info.offset = forced_offset;
	} else {
		ip->info.offset = rxe->mmap_offset;
	}

	/*
	 * Ratchet mmap_offset past the claimed range. Subsequent
	 * legacy create_cq paths land past forced_offset's end; a
	 * subsequent forced caller with a larger pgoff just bumps
	 * further; a forced caller with a smaller pgoff bumps not at
	 * all (its range was already covered, so it must collide with
	 * something already in pending or already long-since freed).
	 */
	{
		u64 next = ip->info.offset + ALIGN(size, SHMLBA);

		if (next > rxe->mmap_offset)
			rxe->mmap_offset = next;
	}

	spin_unlock_bh(&rxe->mmap_offset_lock);

	list_add(&ip->pending_mmaps, &rxe->pending_mmaps);

	spin_unlock_bh(&rxe->pending_lock);

	return ip;
}
