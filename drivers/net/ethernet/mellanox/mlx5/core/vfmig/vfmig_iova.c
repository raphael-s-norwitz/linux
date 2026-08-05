// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/* Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved. */

#include <linux/dma-map-ops.h>
#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/iommu-dma.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/types.h>

#include "vfmig_iova.h"

/*
 * A per-VF fixed-IOVA arena reserved inside the VF's managed DMA-IOMMU
 * domain. The VF keeps its default (dma-iommu-managed) domain, so its
 * ordinary DMA-API traffic still works; we only reserve a fixed IOVA
 * carveout in it with dma_iova_alloc_fixed() that a later allocator
 * populates with dma_iova_link(). @arena is the reservation returned by
 * dma_iova_alloc_fixed(); @arena.addr == @base. @vf_pdev is pinned for
 * the domain's lifetime; @vf_id derives the IOVA window and labels log
 * lines; [@base, @end) is the per-VF IOVA window carved for a later
 * allocator.
 */
struct vfmig_iova_domain {
	struct dma_iova_state	 arena;	  /* reserved [base, base+PER_VF) */
	struct pci_dev		*vf_pdev; /* held via pci_dev_get() */
	u32			 vf_id;
	u64			 base;
	u64			 end;
};

int vfmig_iova_domain_create(struct pci_dev *vf_pdev, u32 vf_id,
			     struct vfmig_iova_domain **out)
{
	struct vfmig_iova_domain *dom;
	int err;

	if (!vf_pdev || !out)
		return -EINVAL;
	if ((u64)vf_id >= U32_MAX / 2)		/* defensive overflow guard */
		return -EINVAL;

	/*
	 * The arena lives inside the VF's managed dma-iommu domain: its IOVA
	 * allocator is what dma_iova_alloc_fixed() reserves from and its page
	 * tables are what a later dma_iova_link() programs. A VF in IOMMU
	 * passthrough (an identity default domain, e.g. from iommu=pt) has
	 * neither -- the device emits physical addresses, which are neither
	 * reservable nor reproducible across a migration. Refuse tracking
	 * with an actionable message instead of the opaque -EOPNOTSUPP the
	 * reservation call would otherwise return.
	 */
	if (!use_dma_iommu(&vf_pdev->dev)) {
		dev_warn(&vf_pdev->dev,
			 "vfmig_iova: vf %u is in IOMMU passthrough (identity domain); migratable VFs require a managed DMA-IOMMU domain -- disable iommu=pt or set this VF's iommu_group type to DMA\n",
			 vf_id);
		return -EOPNOTSUPP;
	}

	/*
	 * The arena keeps the VF on its managed DMA-IOMMU domain and will
	 * back every migratable buffer with dma_iova_link(IOMMU_CACHE), a
	 * valid substitute for dma_alloc_coherent() only on a coherent
	 * device -- exactly the envelope user RDMA already requires (MR
	 * pages are mapped once and accessed by HCA + CPU with no dma_sync).
	 * Refuse tracking on a non-coherent device rather than silently
	 * handing the firmware uncached memory.
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

	dom->vf_id = vf_id;
	dom->base  = VFMIG_IOVA_BASE + (u64)vf_id * VFMIG_IOVA_PER_VF;
	dom->end   = dom->base + VFMIG_IOVA_PER_VF;
	if (dom->base < VFMIG_IOVA_BASE) {	/* wrapped */
		err = -ERANGE;
		goto err_free;
	}

	dom->vf_pdev = pci_dev_get(vf_pdev);

	/*
	 * Reserve the fixed per-VF carveout in the VF's managed DMA-IOMMU
	 * domain. dma_iova_alloc_fixed() grabs exactly [base, base+PER_VF)
	 * or fails: an overlap with the SW-MSI cookie, a reserved-region
	 * window, an already-allocated IOVA, or a range past the IOMMU
	 * aperture all return an error, in which case SET_TRACKED fails here
	 * with a printed reason rather than deep inside a later cmd-ring DMA.
	 */
	err = dma_iova_alloc_fixed(&vf_pdev->dev, &dom->arena, dom->base,
				   VFMIG_IOVA_PER_VF);
	if (err) {
		dev_warn(&vf_pdev->dev,
			 "vfmig_iova: vf %u could not reserve IOVA window [0x%llx, 0x%llx): %d\n",
			 vf_id, dom->base, dom->end, err);
		goto err_pci_put;
	}

	dev_info(&vf_pdev->dev,
		 "vfmig_iova: vf %u arena reserved on managed domain, IOVA window [0x%llx, 0x%llx)\n",
		 vf_id, dom->base, dom->end);

	*out = dom;
	return 0;

err_pci_put:
	pci_dev_put(dom->vf_pdev);
	dom->vf_pdev = NULL;
err_free:
	kfree(dom);
	return err;
}

void vfmig_iova_domain_destroy(struct vfmig_iova_domain *dom)
{
	struct pci_dev *vf_pdev;
	bool arena_live;

	if (!dom)
		return;
	vf_pdev = dom->vf_pdev;

	/*
	 * The arena lives in the VF's managed DMA-IOMMU domain. On the
	 * SET_TRACKED{disable} path the VF (and that domain) are still
	 * present, so release the reservation. On the SR-IOV teardown path
	 * this runs after pci_disable_sriov() has removed the VF pci_dev:
	 * the device's iommu_group -- and with it the iovad and our
	 * reservation -- are already gone, so dma_iova_free_fixed() would
	 * dereference a NULL iommu_group. Skip the arena side in that case
	 * and only free memory we own.
	 */
	arena_live = vf_pdev && iommu_get_domain_for_dev(&vf_pdev->dev);
	if (arena_live)
		dma_iova_free_fixed(&vf_pdev->dev, &dom->arena);

	if (vf_pdev) {
		dev_info(&vf_pdev->dev,
			 "vfmig_iova: vf %u arena %s\n", dom->vf_id,
			 arena_live ? "released" :
				      "reclaimed with VF domain");
		pci_dev_put(vf_pdev);
	}

	kfree(dom);
}
