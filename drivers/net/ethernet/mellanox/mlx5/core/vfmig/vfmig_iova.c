// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/* Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved. */

/*
 * vfmig_iova: per-VF fixed-IOVA arena + deterministic slot allocator.
 * See vfmig_iova.h for the high-level rationale; this file is the
 * implementation. The VF stays on its normal managed DMA-IOMMU domain;
 * we reserve a fixed IOVA carveout in it (dma_iova_alloc_fixed) and
 * populate it with dma_iova_link(). All entry points serialize on
 * dom->lock; the DMA-IOVA API is reentrant under link/unlink, so no
 * extra serialization of the underlying layer is needed.
 */

#include <linux/align.h>
#include <linux/dma-map-ops.h>
#include <linux/dma-mapping.h>
#include <linux/gfp.h>
#include <linux/iommu.h>
#include <linux/iommu-dma.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/types.h>

#include "vfmig_iova.h"

/*
 * Byte offset of a hardware IOVA within the per-VF arena reservation.
 * dma_iova_link()/_unlink()/_sync() all address the arena by offset
 * from its base (dom->arena.addr == dom->base).
 */
#define vfmig_arena_off(dom, iova)	((size_t)((iova) - (dom)->arena.addr))
#define vfmig_dom_dev(dom)		(&(dom)->vf_pdev->dev)

/*
 * One backing page (or higher-order compound page) registered in a
 * domain. @iova/@len identify the IOMMU mapping; @page/@vaddr are the
 * host-side handles. @len is always a multiple of PAGE_SIZE.
 *
 * @slot tags which IOVA sub-window the entry belongs to; @instance_key
 * is the per-slot identifier described in the vfmig_iova.h header doc.
 * Both are stamped by alloc_slot() at create time.
 */
struct vfmig_iova_page {
	struct list_head node;	/* dom->pages, sorted by iova ascending */
	u64		 iova;
	size_t		 len;
	struct page	*page;
	void		*vaddr;
	enum vfmig_iova_slot slot;
	u64		 instance_key;

	/*
	 * @external: the backing page is owned by the caller, not by the
	 * registry. Set on entries created by the
	 * vfmig_iova_user_page_map_phys() path (umem-pinned MR / CQ / QP /
	 * SRQ buffers + doorbell records flowing through the ib_core
	 * umem-placement hook). For these entries the registry tracks the
	 * (iova, len) bookkeeping and owns the arena link, but @page and
	 * @vaddr are NULL: it
	 * does not alloc_pages() at install nor __free_pages() at destroy,
	 * and vfmig_iova_for_each() skips them (its callback dereferences
	 * @vaddr, which is meaningless here).
	 */
	bool		 external;
};

/*
 * A per-VF fixed-IOVA arena reserved inside the VF's managed DMA-IOMMU
 * domain. The VF keeps its default (dma-iommu-managed) domain, so its
 * ordinary DMA-API traffic still works; we only reserve a fixed IOVA
 * carveout in it and populate it with dma_iova_link(). @arena is the
 * reservation returned by dma_iova_alloc_fixed(); @arena.addr == @base.
 * @vf_pdev is pinned for the domain's lifetime; @vf_id derives the IOVA
 * window and labels log lines. The deterministic range
 * [base, base + NR_SLOTS * SLOT_BYTES) is partitioned across the slot
 * windows.
 */
struct vfmig_iova_domain {
	struct dma_iova_state arena;	/* reserved [base, base+PER_VF) */
	struct pci_dev	    *vf_pdev;	/* held via pci_dev_get() */
	u32		     vf_id;

	u64		     base;

	struct mutex	     lock;
	/*
	 * Per-slot bump cursor. cursor[N] is the next fresh IOVA in
	 * slot N's window, valid in [slot_base(N), slot_base(N+1)).
	 * Initialized to slot_base(N) at domain create.
	 */
	u64		     cursor[VFMIG_IOVA_NR_SLOTS];
	/*
	 * Per-slot auto-key counter, incremented on each alloc_slot
	 * call that passes instance_key == 0. Skipped when the caller
	 * passes a non-zero (caller-pinned) key.
	 */
	u64		     next_auto_key[VFMIG_IOVA_NR_SLOTS];

	struct list_head     pages;	/* of vfmig_iova_page, sorted */
	unsigned int	     n_pages;

	/*
	 * Host-page replay accounting (LOAD/destination side).
	 *
	 * @expected_count[s] is the number of HOST_PAGE records replayed
	 * into slot s by vfmig_iova_replay_page(); it records the source
	 * VF's footprint in that slot at SAVE time.
	 *
	 * @drift_armed is flipped once by vfmig_iova_arm_drift_detection()
	 * after a LOAD has replayed every promised record. While armed,
	 * alloc_slot() diagnoses (logs) a mismatch between the caller's
	 * resolved instance_key and the replayed entry it re-claims, so a
	 * destination-side allocation-sequence drift from the source is
	 * visible in dmesg. Both are zero on a fresh / SET_TRACKED-but-not-
	 * LOADed domain, where alloc_slot behaves exactly as before slice 8.
	 */
	u32		     expected_count[VFMIG_IOVA_NR_SLOTS];
	bool		     drift_armed;
};

static inline u64
vfmig_iova_slot_base(const struct vfmig_iova_domain *dom,
		     enum vfmig_iova_slot slot)
{
	return dom->base + (u64)slot * VFMIG_IOVA_SLOT_BYTES;
}

/*
 * USER_PAGE's effective starting IOVA after the kcoherent carve. The
 * bottom VFMIG_IOVA_KCOHERENT_BYTES of slot USER_PAGE's window are
 * reserved for the (future) non-migrated kcoherent sub-arena, so the
 * user-MR IOVA range proper begins here. Range checks and cursor
 * initialization for USER_PAGE MUST use this rather than
 * vfmig_iova_slot_base(dom, VFMIG_SLOT_USER_PAGE) directly.
 */
static inline u64
vfmig_iova_user_page_start(const struct vfmig_iova_domain *dom)
{
	return vfmig_iova_slot_base(dom, VFMIG_SLOT_USER_PAGE) +
	       VFMIG_IOVA_KCOHERENT_BYTES;
}

/*
 * End (exclusive) of @slot's window. Kernel slots are uniform 510-MiB
 * windows; VFMIG_SLOT_USER_PAGE is expand-to-fill and ends at the top of
 * the per-VF window.
 */
static inline u64
vfmig_iova_slot_end(const struct vfmig_iova_domain *dom,
		    enum vfmig_iova_slot slot)
{
	if (slot == VFMIG_SLOT_USER_PAGE)
		return dom->base + VFMIG_IOVA_PER_VF;
	return dom->base + (u64)(slot + 1) * VFMIG_IOVA_SLOT_BYTES;
}

/*
 * Inverse of vfmig_iova_slot_base(): which slot does @iova fall into, or
 * VFMIG_SLOT_INVALID if it is outside the deterministic slot range (below
 * dom->base, or at/above the top of the per-VF window). Used by replay to
 * cross-check that a wire record's claimed slot agrees with the
 * destination's own IOVA partitioning.
 */
static enum vfmig_iova_slot
vfmig_iova_slot_from_iova(const struct vfmig_iova_domain *dom, u64 iova)
{
	u64 idx;

	if (iova < dom->base || iova >= dom->base + VFMIG_IOVA_PER_VF)
		return VFMIG_SLOT_INVALID;
	idx = (iova - dom->base) / VFMIG_IOVA_SLOT_BYTES;
	if (idx <= VFMIG_SLOT_INVALID)
		return VFMIG_SLOT_INVALID;
	/*
	 * Asymmetric layout: everything at or above slot_base(USER_PAGE)
	 * is either the reserved kcoherent carve (not a deterministic slot
	 * -> INVALID, so replay can never install there) or the USER_PAGE
	 * expand-to-fill window.
	 */
	if (idx >= VFMIG_SLOT_USER_PAGE) {
		if (iova < vfmig_iova_user_page_start(dom))
			return VFMIG_SLOT_INVALID;	/* kcoherent range */
		return VFMIG_SLOT_USER_PAGE;
	}
	return (enum vfmig_iova_slot)idx;
}

/* dom->lock held. Returns the entry mapped at exactly @iova, or NULL. */
static struct vfmig_iova_page *
vfmig_iova_find_locked(struct vfmig_iova_domain *dom, u64 iova)
{
	struct vfmig_iova_page *p;

	list_for_each_entry(p, &dom->pages, node) {
		if (p->iova == iova)
			return p;
		if (p->iova > iova)
			return NULL;	/* sorted: gone past it */
	}
	return NULL;
}

/* dom->lock held. Inserts @new keyed by iova; sorted ascending. */
static void
vfmig_iova_insert_locked(struct vfmig_iova_domain *dom,
			 struct vfmig_iova_page *new)
{
	struct vfmig_iova_page *p;

	list_for_each_entry(p, &dom->pages, node) {
		if (p->iova > new->iova) {
			list_add_tail(&new->node, &p->node);
			dom->n_pages++;
			return;
		}
	}
	list_add_tail(&new->node, &dom->pages);
	dom->n_pages++;
}

/*
 * dom->lock held. Allocate a backing page or higher-order compound,
 * link it at @iova for @len bytes into the arena, and append the
 * registry entry. Does NOT advance the cursor; callers do that
 * themselves.
 *
 * @slot is used to validate that @iova falls inside that slot's window;
 * a callsite passing the wrong slot for an IOVA returns -ERANGE.
 *
 * Returns 0 with *out_p set on success, negative errno otherwise.
 */
static int
vfmig_iova_install_page_locked(struct vfmig_iova_domain *dom,
			       enum vfmig_iova_slot slot, u64 instance_key,
			       u64 iova, size_t len, gfp_t gfp,
			       struct vfmig_iova_page **out_p)
{
	struct vfmig_iova_page *p;
	unsigned int order;
	int err;

	if (!IS_ALIGNED(iova, VFMIG_IOVA_GRANULE) ||
	    !IS_ALIGNED(len, VFMIG_IOVA_GRANULE) ||
	    len == 0)
		return -EINVAL;
	if (slot <= VFMIG_SLOT_INVALID || slot >= VFMIG_SLOT_NR)
		return -EINVAL;
	if (iova < vfmig_iova_slot_base(dom, slot) ||
	    iova + len > vfmig_iova_slot_end(dom, slot))
		return -ERANGE;
	if (vfmig_iova_find_locked(dom, iova))
		return -EEXIST;

	/*
	 * The backing page must be lowmem so page_address() works and the
	 * DMA-IOVA layer can link its physical address; reject the
	 * offending flags here with a clear errno rather than deep in the
	 * link path.
	 */
	if (gfp & (__GFP_COMP | __GFP_DMA | __GFP_DMA32 | __GFP_HIGHMEM)) {
		dev_warn_ratelimited(&dom->vf_pdev->dev,
				     "vfmig_iova: install_page: rejected gfp 0x%x (must not include __GFP_HIGHMEM/COMP/DMA/DMA32)\n",
				     gfp);
		return -EINVAL;
	}

	p = kzalloc(sizeof(*p), gfp);
	if (!p)
		return -ENOMEM;

	order = get_order(len);
	p->page = alloc_pages(gfp | __GFP_ZERO, order);
	if (!p->page) {
		err = -ENOMEM;
		goto err_free_p;
	}
	p->vaddr	= page_address(p->page);
	p->iova		= iova;
	p->len		= len;
	p->slot		= slot;
	p->instance_key	= instance_key;

	err = dma_iova_link(vfmig_dom_dev(dom), &dom->arena,
			    page_to_phys(p->page), vfmig_arena_off(dom, iova),
			    len, DMA_BIDIRECTIONAL, 0);
	if (err)
		goto err_free_page;
	err = dma_iova_sync(vfmig_dom_dev(dom), &dom->arena,
			    vfmig_arena_off(dom, iova), len);
	if (err) {
		dma_iova_unlink(vfmig_dom_dev(dom), &dom->arena,
				vfmig_arena_off(dom, iova), len,
				DMA_BIDIRECTIONAL, 0);
		goto err_free_page;
	}

	vfmig_iova_insert_locked(dom, p);
	*out_p = p;
	return 0;

err_free_page:
	__free_pages(p->page, order);
err_free_p:
	kfree(p);
	return err;
}

/*
 * dom->lock held. Install one external (caller-owned-page) registry
 * entry at @iova, linking @phys for @len bytes into the arena. Used by
 * the ib_core umem-placement hook to plumb umem-pinned pages into the
 * per-VF carveout.
 *
 * Differs from vfmig_iova_install_page_locked in that it does not
 * alloc_pages(): the caller supplies @phys directly (from sg_phys()).
 * The entry is flagged @external = true so destroy_page_locked skips
 * __free_pages, and @page / @vaddr are left NULL (the registry has no
 * kernel-virtual handle to caller-owned memory). Same window/alignment
 * validation, same -EEXIST on duplicate IOVA, same -ERANGE on
 * slot/IOVA mismatch.
 */
static int
vfmig_iova_install_external_phys_locked(struct vfmig_iova_domain *dom,
					enum vfmig_iova_slot slot,
					u64 instance_key, u64 iova,
					phys_addr_t phys, size_t len,
					gfp_t gfp,
					struct vfmig_iova_page **out_p)
{
	struct vfmig_iova_page *p;
	int err;

	if (!IS_ALIGNED(iova, VFMIG_IOVA_GRANULE) ||
	    !IS_ALIGNED(len, VFMIG_IOVA_GRANULE) ||
	    !IS_ALIGNED(phys, VFMIG_IOVA_GRANULE) ||
	    len == 0)
		return -EINVAL;
	if (slot <= VFMIG_SLOT_INVALID || slot >= VFMIG_SLOT_NR)
		return -EINVAL;
	{
		u64 lo = (slot == VFMIG_SLOT_USER_PAGE)
			? vfmig_iova_user_page_start(dom)
			: vfmig_iova_slot_base(dom, slot);
		if (iova < lo || iova + len > vfmig_iova_slot_end(dom, slot))
			return -ERANGE;
	}
	if (vfmig_iova_find_locked(dom, iova))
		return -EEXIST;

	p = kzalloc(sizeof(*p), gfp);
	if (!p)
		return -ENOMEM;

	p->page		= NULL;
	p->vaddr	= NULL;
	p->iova		= iova;
	p->len		= len;
	p->slot		= slot;
	p->instance_key	= instance_key;
	p->external	= true;

	err = dma_iova_link(vfmig_dom_dev(dom), &dom->arena, phys,
			    vfmig_arena_off(dom, iova), len,
			    DMA_BIDIRECTIONAL, 0);
	if (err) {
		kfree(p);
		return err;
	}
	err = dma_iova_sync(vfmig_dom_dev(dom), &dom->arena,
			    vfmig_arena_off(dom, iova), len);
	if (err) {
		dma_iova_unlink(vfmig_dom_dev(dom), &dom->arena,
				vfmig_arena_off(dom, iova), len,
				DMA_BIDIRECTIONAL, 0);
		kfree(p);
		return err;
	}

	vfmig_iova_insert_locked(dom, p);
	*out_p = p;
	return 0;
}

/*
 * dom->lock held. Tear down a single registry entry: dma_iova_unlink,
 * release backing pages, free the bookkeeping struct. List unlink is
 * the caller's responsibility (so we can be called from list iteration).
 *
 * @unmap governs the arena side only: pass false when the VF's managed
 * DMA domain has already been torn down (device removal reclaimed the
 * iovad, its mappings and the reservation), so dma_iova_unlink() would
 * dereference a NULL iommu_group. The backing pages and bookkeeping are
 * ours and are always freed.
 */
static void
vfmig_iova_destroy_page_locked(struct vfmig_iova_domain *dom,
			       struct vfmig_iova_page *p, bool unmap)
{
	if (unmap)
		dma_iova_unlink(vfmig_dom_dev(dom), &dom->arena,
				vfmig_arena_off(dom, p->iova), p->len,
				DMA_BIDIRECTIONAL, 0);
	if (p->page)
		__free_pages(p->page, get_order(p->len));
	kfree(p);
}

/* -------- exported API -------------------------------------------------- */

int vfmig_iova_domain_create(struct pci_dev *vf_pdev, u32 vf_id,
			     struct vfmig_iova_domain **out)
{
	struct vfmig_iova_domain *dom;
	u64 base;
	unsigned int s;
	int err;

	if (!vf_pdev || !out)
		return -EINVAL;
	if ((u64)vf_id >= U32_MAX / 2)	/* defensive: catch overflow */
		return -EINVAL;

	base = VFMIG_IOVA_BASE + (u64)vf_id * VFMIG_IOVA_PER_VF;
	if (base < VFMIG_IOVA_BASE)	/* wrapped */
		return -ERANGE;

	/*
	 * The arena lives inside the VF's managed dma-iommu domain: its IOVA
	 * allocator is what dma_iova_alloc_fixed() reserves from and its page
	 * tables are what dma_iova_link() programs. A VF in IOMMU passthrough
	 * (an identity default domain, e.g. from iommu=pt) has neither -- the
	 * device emits physical addresses, which are neither reservable nor
	 * reproducible across a migration. Refuse tracking with an actionable
	 * message instead of the opaque -EOPNOTSUPP the reservation call
	 * would otherwise return.
	 */
	if (!use_dma_iommu(&vf_pdev->dev)) {
		dev_warn(&vf_pdev->dev,
			 "vfmig_iova: vf %u is in IOMMU passthrough (identity domain); migratable VFs require a managed DMA-IOMMU domain -- disable iommu=pt or set this VF's iommu_group type to DMA\n",
			 vf_id);
		return -EOPNOTSUPP;
	}

	/*
	 * The arena keeps the VF on its managed DMA-IOMMU domain and backs
	 * every migratable buffer with dma_iova_link(IOMMU_CACHE). That is
	 * a valid substitute for dma_alloc_coherent() only on a coherent
	 * device -- exactly the envelope user RDMA already requires (MR
	 * pages are mapped once and accessed by HCA + CPU with no
	 * dma_sync). Refuse tracking on a non-coherent device rather than
	 * silently handing the firmware uncached memory.
	 */
	if (!dev_is_dma_coherent(&vf_pdev->dev)) {
		dev_warn(&vf_pdev->dev,
			 "vfmig_iova: vf %u device is not DMA-coherent; migration unsupported\n",
			 vf_id);
		return -EOPNOTSUPP;
	}

	dom = kzalloc(sizeof(*dom), GFP_KERNEL);
	if (!dom)
		return -ENOMEM;

	mutex_init(&dom->lock);
	INIT_LIST_HEAD(&dom->pages);
	dom->vf_id = vf_id;
	dom->base  = base;

	/*
	 * Per-slot bump cursors: each starts at its slot's window base.
	 * Slot 0 (VFMIG_SLOT_INVALID) gets a cursor too, to keep the
	 * indexing trivial -- alloc_slot rejects SLOT_INVALID before it
	 * ever touches cursor[0]. next_auto_key[] is zero-initialized by
	 * kzalloc above; first auto-assignment yields key 1.
	 */
	for (s = 0; s < VFMIG_IOVA_NR_SLOTS; s++)
		dom->cursor[s] = vfmig_iova_slot_base(dom,
						      (enum vfmig_iova_slot)s);
	/*
	 * USER_PAGE is expand-to-fill with a kcoherent carve at its base,
	 * so its cursor starts past the carve, not at slot_base.
	 */
	dom->cursor[VFMIG_SLOT_USER_PAGE] = vfmig_iova_user_page_start(dom);

	dom->vf_pdev = pci_dev_get(vf_pdev);

	/*
	 * Reserve the fixed per-VF carveout in the VF's managed DMA-IOMMU
	 * domain. dma_iova_alloc_fixed() grabs exactly [base, base+PER_VF)
	 * or fails: an overlap with the SW-MSI cookie, an
	 * iommu_dma_get_resv_regions() window, an already-allocated IOVA,
	 * or a range past the IOMMU aperture all return an error, in which
	 * case SET_TRACKED fails here with a printed reason rather than
	 * deep inside a later cmd-ring DMA. Reserving before any other DMA
	 * traffic on this VF is a correctness requirement.
	 */
	err = dma_iova_alloc_fixed(&vf_pdev->dev, &dom->arena, base,
				   VFMIG_IOVA_PER_VF);
	if (err) {
		dev_warn(&vf_pdev->dev,
			 "vfmig_iova: vf %u could not reserve IOVA window [0x%llx, 0x%llx): %d\n",
			 vf_id, dom->base, base + VFMIG_IOVA_PER_VF, err);
		goto err_pci_put;
	}

	dev_info(&vf_pdev->dev,
		 "vfmig_iova: vf %u arena reserved on managed domain: kernel slots [0x%llx, 0x%llx) (%u x 0x%llx) + kcoherent carve 0x%llx + USER_PAGE [0x%llx, 0x%llx)\n",
		 vf_id, dom->base,
		 vfmig_iova_slot_base(dom, VFMIG_SLOT_USER_PAGE),
		 VFMIG_IOVA_KERNEL_NR_SLOTS, (u64)VFMIG_IOVA_SLOT_BYTES,
		 (u64)VFMIG_IOVA_KCOHERENT_BYTES,
		 vfmig_iova_user_page_start(dom), base + VFMIG_IOVA_PER_VF);

	*out = dom;
	return 0;

err_pci_put:
	pci_dev_put(dom->vf_pdev);
	dom->vf_pdev = NULL;
	mutex_destroy(&dom->lock);
	kfree(dom);
	return err;
}

void vfmig_iova_domain_destroy(struct vfmig_iova_domain *dom)
{
	struct vfmig_iova_page *p, *tmp;
	struct pci_dev *vf_pdev;
	bool arena_live;

	if (!dom)
		return;
	vf_pdev = dom->vf_pdev;

	/*
	 * The arena lives in the VF's managed DMA-IOMMU domain. On the
	 * SET_TRACKED{disable} path the VF (and that domain) are still
	 * present, so we must dma_iova_unlink() every mapping and release
	 * the reservation. On the teardown path this runs from
	 * mlx5_sriov_disable() *after* pci_disable_sriov() has removed the
	 * VF pci_dev: the device's iommu_group -- and with it the iovad,
	 * its page tables and our reservation -- are already gone, so any
	 * dma_iova_*() call here would dereference a NULL iommu_group. Skip
	 * the arena side in that case and only free memory we own.
	 */
	arena_live = vf_pdev && iommu_get_domain_for_dev(&vf_pdev->dev);

	mutex_lock(&dom->lock);
	list_for_each_entry_safe(p, tmp, &dom->pages, node) {
		list_del(&p->node);
		vfmig_iova_destroy_page_locked(dom, p, arena_live);
	}
	dom->n_pages = 0;
	if (arena_live)
		dma_iova_free_fixed(&vf_pdev->dev, &dom->arena);
	mutex_unlock(&dom->lock);

	if (vf_pdev) {
		dev_info(&vf_pdev->dev,
			 "vfmig_iova: vf %u arena %s\n", dom->vf_id,
			 arena_live ? "released" :
				      "reclaimed with VF domain");
		pci_dev_put(vf_pdev);
	}

	mutex_destroy(&dom->lock);
	kfree(dom);
}

int vfmig_iova_alloc_slot(struct vfmig_iova_domain *dom,
			  enum vfmig_iova_slot slot, u64 instance_key,
			  size_t size, gfp_t gfp,
			  dma_addr_t *iova_out, void **vaddr_out)
{
	struct vfmig_iova_page *p;
	size_t aligned;
	u64 iova, slot_end;
	u64 caller_key;
	int err;

	if (!dom || !iova_out || !vaddr_out || size == 0)
		return -EINVAL;
	if (slot <= VFMIG_SLOT_INVALID || slot >= VFMIG_SLOT_NR)
		return -EINVAL;

	aligned = ALIGN(size, VFMIG_IOVA_GRANULE);

	mutex_lock(&dom->lock);

	/*
	 * Auto-assign instance_key if the caller passed 0. Per-slot
	 * counter, so adding allocations in another slot doesn't perturb
	 * this slot's keys. Caller-pinned (non-zero) keys are recorded
	 * as-is and don't bump the counter. @caller_key remembers the
	 * pre-resolution value so the drift diagnostic can tell a 0/auto
	 * caller from a pinned one.
	 */
	caller_key = instance_key;
	if (instance_key == 0)
		instance_key = ++dom->next_auto_key[slot];

	iova = dom->cursor[slot];
	slot_end = vfmig_iova_slot_end(dom, slot);
	if (iova + aligned > slot_end) {
		dev_warn_ratelimited(&dom->vf_pdev->dev,
				     "vfmig_iova: vf %u slot %u exhausted at cursor 0x%llx (slot_end 0x%llx, asked %zu)\n",
				     dom->vf_id, slot, iova, slot_end,
				     aligned);
		err = -ENOSPC;
		goto out_unlock;
	}

	/*
	 * Lookup-or-alloc at the per-slot cursor. An entry already mapped
	 * at @iova came from a prior vfmig_iova_replay_page(): the
	 * destination's probe is re-claiming a page the source snapshotted,
	 * so hand back the replayed page (which carries the source's
	 * contents) instead of installing a fresh zeroed one -- this is
	 * what makes a *migrated* VF's cmd ring / FW pages / EQ buffers
	 * functional after LOAD. The size must match the source's
	 * allocation at this slot position; a mismatch is a determinism
	 * break, not a page we can safely hand back.
	 */
	p = vfmig_iova_find_locked(dom, iova);
	if (p) {
		if (p->len != aligned) {
			dev_warn(&dom->vf_pdev->dev,
				 "vfmig_iova: vf %u slot %u replay/alloc size mismatch at IOVA 0x%llx: replayed %zu, requested %zu\n",
				 dom->vf_id, slot, iova, p->len, aligned);
			err = -EINVAL;
			goto out_unlock;
		}
		/*
		 * Diagnostic only: once drift detection is armed (LOAD
		 * finished replaying), a resolved-key mismatch means the
		 * destination's pinned/auto allocation sequence in this
		 * slot diverged from the source's. Log it but still
		 * re-claim the page -- strict -EPROTO enforcement (and the
		 * kcoherent fallback for legitimate post-restore growth)
		 * arrives with the USER_PAGE work.
		 */
		if (dom->drift_armed && p->instance_key != instance_key)
			dev_warn_ratelimited(&dom->vf_pdev->dev,
				"vfmig_iova: vf %u slot %u DRIFT: caller key %s (resolved 0x%llx) but replayed entry at IOVA 0x%llx carries key 0x%llx\n",
				dom->vf_id, slot,
				caller_key == 0 ? "0/auto" : "pinned",
				(unsigned long long)instance_key,
				(unsigned long long)iova,
				(unsigned long long)p->instance_key);
		p->slot		= slot;
		p->instance_key	= instance_key;
		dom->cursor[slot] = iova + aligned;
		*iova_out  = p->iova;
		*vaddr_out = p->vaddr;
		err = 0;
		goto out_unlock;
	}

	err = vfmig_iova_install_page_locked(dom, slot, instance_key,
					     iova, aligned, gfp, &p);
	if (err)
		goto out_unlock;

	dom->cursor[slot] = iova + aligned;
	*iova_out  = p->iova;
	*vaddr_out = p->vaddr;
	err = 0;

out_unlock:
	mutex_unlock(&dom->lock);
	return err;
}

void vfmig_iova_free_slot(struct vfmig_iova_domain *dom,
			  enum vfmig_iova_slot slot,
			  dma_addr_t iova, size_t size)
{
	struct vfmig_iova_page *p;
	size_t aligned;

	if (!dom)
		return;
	if (slot <= VFMIG_SLOT_INVALID || slot >= VFMIG_SLOT_NR) {
		dev_warn(&dom->vf_pdev->dev,
			 "vfmig_iova: free_slot: bad slot %u for IOVA 0x%llx\n",
			 slot, (u64)iova);
		return;
	}
	aligned = ALIGN(size, VFMIG_IOVA_GRANULE);

	mutex_lock(&dom->lock);
	p = vfmig_iova_find_locked(dom, iova);
	if (!p) {
		dev_warn(&dom->vf_pdev->dev,
			 "vfmig_iova: vf %u free of unknown IOVA 0x%llx (slot %u)\n",
			 dom->vf_id, (u64)iova, slot);
		goto out_unlock;
	}
	WARN_ON_ONCE(p->slot != slot);
	if (p->len != aligned) {
		dev_warn(&dom->vf_pdev->dev,
			 "vfmig_iova: vf %u free size mismatch at IOVA 0x%llx (slot %u): have %zu, asked %zu\n",
			 dom->vf_id, (u64)iova, slot, p->len, aligned);
		/* still proceed: the entry is what it is */
	}
	list_del(&p->node);
	dom->n_pages--;
	vfmig_iova_destroy_page_locked(dom, p, true);

out_unlock:
	mutex_unlock(&dom->lock);
}

int vfmig_iova_user_page_map_phys(struct vfmig_iova_domain *dom,
				  phys_addr_t phys, size_t len, gfp_t gfp,
				  dma_addr_t *iova_out)
{
	struct vfmig_iova_page *p;
	size_t aligned;
	u64 iova, slot_end;
	int err;

	if (!dom || !iova_out || len == 0)
		return -EINVAL;
	if (!IS_ALIGNED(phys, VFMIG_IOVA_GRANULE))
		return -EINVAL;

	aligned = ALIGN(len, VFMIG_IOVA_GRANULE);

	mutex_lock(&dom->lock);

	iova = dom->cursor[VFMIG_SLOT_USER_PAGE];
	slot_end = vfmig_iova_slot_end(dom, VFMIG_SLOT_USER_PAGE);
	if (iova + aligned > slot_end) {
		dev_warn_ratelimited(&dom->vf_pdev->dev,
				     "vfmig_iova: vf %u USER_PAGE slot exhausted at cursor 0x%llx (slot_end 0x%llx, asked %zu)\n",
				     dom->vf_id, iova, slot_end, aligned);
		err = -ENOSPC;
		goto out_unlock;
	}

	/*
	 * The cursor tracks the next fresh USER_PAGE IOVA, so a registry
	 * hit here means another allocator (kernel slot or a leaked prior
	 * map) already sits on it -- a kernel bug. Fail
	 * cleanly rather than silently overwrite. (Stage 2's LOAD-side
	 * placeholder replay will introduce a legitimate hit path here.)
	 */
	p = vfmig_iova_find_locked(dom, iova);
	if (p) {
		dev_err_ratelimited(&dom->vf_pdev->dev,
				    "vfmig_iova: vf %u USER_PAGE cursor 0x%llx already mapped (slot %u key 0x%llx len %zu); refusing to overwrite\n",
				    dom->vf_id, iova, p->slot,
				    (unsigned long long)p->instance_key,
				    p->len);
		err = -EEXIST;
		goto out_unlock;
	}

	err = vfmig_iova_install_external_phys_locked(dom,
						      VFMIG_SLOT_USER_PAGE,
						      ++dom->next_auto_key[VFMIG_SLOT_USER_PAGE],
						      iova, phys, aligned, gfp, &p);
	if (err)
		goto out_unlock;

	dom->cursor[VFMIG_SLOT_USER_PAGE] = iova + aligned;
	*iova_out = iova;
	err = 0;

out_unlock:
	mutex_unlock(&dom->lock);
	return err;
}

int vfmig_iova_user_page_unmap_phys(struct vfmig_iova_domain *dom,
				    dma_addr_t iova, size_t len)
{
	struct vfmig_iova_page *p;
	size_t aligned;
	int err;

	if (!dom)
		return -EINVAL;

	aligned = ALIGN(len, VFMIG_IOVA_GRANULE);

	mutex_lock(&dom->lock);

	p = vfmig_iova_find_locked(dom, (u64)iova);
	if (!p) {
		dev_warn_ratelimited(&dom->vf_pdev->dev,
				     "vfmig_iova: vf %u USER_PAGE unmap: no registry entry at IOVA 0x%llx (asked %zu)\n",
				     dom->vf_id, (u64)iova, aligned);
		err = -ENOENT;
		goto out_unlock;
	}
	if (p->slot != VFMIG_SLOT_USER_PAGE || !p->external) {
		dev_warn_ratelimited(&dom->vf_pdev->dev,
				     "vfmig_iova: vf %u USER_PAGE unmap: IOVA 0x%llx is slot %u external=%d (expected USER_PAGE external)\n",
				     dom->vf_id, (u64)iova, p->slot,
				     p->external);
		err = -EINVAL;
		goto out_unlock;
	}
	if (p->len != aligned)
		dev_warn_ratelimited(&dom->vf_pdev->dev,
				     "vfmig_iova: vf %u USER_PAGE unmap size mismatch at IOVA 0x%llx: have %zu, asked %zu\n",
				     dom->vf_id, (u64)iova, p->len, aligned);

	list_del(&p->node);
	dom->n_pages--;
	vfmig_iova_destroy_page_locked(dom, p, true);
	err = 0;

out_unlock:
	mutex_unlock(&dom->lock);
	return err;
}

int vfmig_iova_replay_page(struct vfmig_iova_domain *dom,
			   enum vfmig_iova_slot slot, u64 instance_key,
			   dma_addr_t iova, const void *contents, size_t len)
{
	struct vfmig_iova_page *p;
	enum vfmig_iova_slot iova_slot;
	int err;

	if (!dom || !contents)
		return -EINVAL;
	if (slot <= VFMIG_SLOT_INVALID || slot >= VFMIG_SLOT_NR) {
		dev_warn(&dom->vf_pdev->dev,
			 "vfmig_iova: vf %u replay: slot %u out of range\n",
			 dom->vf_id, slot);
		return -EINVAL;
	}
	if (slot == VFMIG_SLOT_USER_PAGE) {
		/*
		 * USER_PAGE uses a separate replay path: its wire records
		 * (added in a later patch) carry identity only, not
		 * contents, and install as awaiting-bind placeholders. A
		 * HOST_PAGE record, which does carry contents, must never
		 * target the USER_PAGE slot.
		 */
		dev_warn(&dom->vf_pdev->dev,
			 "vfmig_iova: vf %u replay_page: USER_PAGE slot is not content-replayable\n",
			 dom->vf_id);
		return -EOPNOTSUPP;
	}

	/*
	 * Cross-check that the wire-claimed slot agrees with the slot the
	 * destination's own IOVA partitioning assigns to @iova. A mismatch
	 * means source and destination disagree about the slot layout
	 * (wire-incompatible CONFIG_MLX5_VFMIG_IOVA_PER_VF_GIB / slot set):
	 * the determinism guarantee is broken, so refuse rather than
	 * install at an unexpected slot.
	 */
	iova_slot = vfmig_iova_slot_from_iova(dom, (u64)iova);
	if (iova_slot != slot) {
		dev_warn(&dom->vf_pdev->dev,
			 "vfmig_iova: vf %u replay: wire claims slot %u for IOVA 0x%llx but destination maps it to slot %u\n",
			 dom->vf_id, slot, (u64)iova, iova_slot);
		return -ERANGE;
	}

	mutex_lock(&dom->lock);

	/*
	 * Replay after arming is anomalous: the LOAD path arms exactly
	 * once, after every promised HOST_PAGE record has been replayed. A
	 * late replay would grow expected_count[] after the footprint was
	 * declared frozen. Refuse it.
	 */
	if (WARN_ON_ONCE(dom->drift_armed)) {
		err = -EBUSY;
		goto out_unlock;
	}

	err = vfmig_iova_install_page_locked(dom, slot, instance_key,
					     (u64)iova, len, GFP_KERNEL, &p);
	if (err)
		goto out_unlock;

	memcpy(p->vaddr, contents, len);
	dom->expected_count[slot]++;

	/*
	 * Push the slot cursor past the highest replayed IOVA so a later
	 * vfmig_iova_reset_cursor() rewinds to the slot base, and so that
	 * absent a reset fresh allocs still don't collide with replays.
	 */
	if ((u64)iova + len > dom->cursor[slot])
		dom->cursor[slot] = (u64)iova + len;

out_unlock:
	mutex_unlock(&dom->lock);
	return err;
}

void vfmig_iova_reset_cursor(struct vfmig_iova_domain *dom)
{
	unsigned int s;

	if (!dom)
		return;
	mutex_lock(&dom->lock);
	for (s = 0; s < VFMIG_IOVA_NR_SLOTS; s++) {
		dom->cursor[s] = vfmig_iova_slot_base(dom,
						      (enum vfmig_iova_slot)s);
		dom->next_auto_key[s] = 0;
	}
	/* USER_PAGE cursor floors at user_page_start (past the carve). */
	dom->cursor[VFMIG_SLOT_USER_PAGE] = vfmig_iova_user_page_start(dom);
	mutex_unlock(&dom->lock);
}

void vfmig_iova_arm_drift_detection(struct vfmig_iova_domain *dom)
{
	unsigned int s;
	u32 total = 0;

	if (!dom)
		return;
	mutex_lock(&dom->lock);
	if (!dom->drift_armed) {
		dom->drift_armed = true;
		for (s = 0; s < VFMIG_IOVA_NR_SLOTS; s++)
			total += dom->expected_count[s];
		dev_info(&dom->vf_pdev->dev,
			 "vfmig_iova: vf %u drift detection armed (replays: cmd_ring=%u fw_page=%u eq_buf=%u frag_buf=%u db_page=%u dma_coherent=%u, total=%u)\n",
			 dom->vf_id,
			 dom->expected_count[VFMIG_SLOT_CMD_RING],
			 dom->expected_count[VFMIG_SLOT_FW_PAGE],
			 dom->expected_count[VFMIG_SLOT_EQ_BUF],
			 dom->expected_count[VFMIG_SLOT_FRAG_BUF],
			 dom->expected_count[VFMIG_SLOT_DB_PAGE],
			 dom->expected_count[VFMIG_SLOT_DMA_COHERENT],
			 total);
	}
	mutex_unlock(&dom->lock);
}

int vfmig_iova_for_each(struct vfmig_iova_domain *dom,
			vfmig_iova_for_each_fn cb, void *ctx)
{
	struct vfmig_iova_page *p;
	int ret = 0;

	if (!dom || !cb)
		return -EINVAL;

	mutex_lock(&dom->lock);
	list_for_each_entry(p, &dom->pages, node) {
		/*
		 * External (USER_PAGE, caller-owned) entries have no
		 * kernel-virtual @vaddr for the callback to read; SAVE's
		 * HOST_PAGE memcpy would dereference NULL. They are emitted
		 * separately via the external-iteration path, so skip them
		 * here.
		 */
		if (p->external)
			continue;
		ret = cb(p->slot, p->instance_key, p->iova, p->vaddr,
			 p->len, ctx);
		if (ret)
			break;
	}
	mutex_unlock(&dom->lock);
	return ret;
}
