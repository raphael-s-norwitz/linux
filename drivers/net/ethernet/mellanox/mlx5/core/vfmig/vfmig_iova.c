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
 * carveout in it with dma_iova_try_alloc() that a later allocator
 * populates with dma_iova_link(). @arena is the reservation returned by
 * dma_iova_try_alloc(); @arena.addr == @base. @vf_pdev is pinned for
 * the domain's lifetime; @vf_id labels log lines and plays no part in
 * the IOVA layout; [@base, @end) is the per-VF IOVA window carved for a
 * later allocator.
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
	u64 limit, aperture_end;
	struct iommu_domain *dma_dom;
	struct vfmig_iova_domain *dom;
	int err;

	if (!vf_pdev || !out)
		return -EINVAL;

	/*
	 * The arena lives inside the VF's managed dma-iommu domain: its IOVA
	 * allocator is what dma_iova_try_alloc() reserves from and its page
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

	/*
	 * Every VF reserves the same window: each VF is its own IOMMU
	 * group with its own managed domain, so identical ranges do not
	 * collide, and a saved image's IOVAs replay onto any VF of the PF
	 * rather than only the VF index it was saved from. VFs that do
	 * share a domain fail the reservation below instead of overlapping.
	 */
	dom->base  = VFMIG_IOVA_BASE;
	dom->end   = dom->base + VFMIG_IOVA_PER_VF;

	dom->vf_pdev = pci_dev_get(vf_pdev);

	/*
	 * The window's placement follows from the DMA mask at reservation
	 * time (see "IOVA window layout" in vfmig_iova.h), and an unbound
	 * VF carries whatever mask it last had: PCI's 32-bit default on a
	 * fresh VF, or the 64 bits mlx5 set on one bound before. Set it
	 * for every reservation and leave it: the mask only bounds future
	 * allocations, and the driver that binds next raising it keeps the
	 * window within reach.
	 */
	err = dma_set_mask(&vf_pdev->dev,
			   DMA_BIT_MASK(VFMIG_IOVA_DMA_MASK_BITS));
	if (!err && dma_get_mask(&vf_pdev->dev) !=
		    DMA_BIT_MASK(VFMIG_IOVA_DMA_MASK_BITS))
		err = -EIO;
	if (err) {
		dev_warn(&vf_pdev->dev,
			 "vfmig_iova: vf %u cannot take a %u-bit DMA mask (mask now 0x%llx): %d\n",
			 vf_id, VFMIG_IOVA_DMA_MASK_BITS,
			 dma_get_mask(&vf_pdev->dev), err);
		goto err_pci_put;
	}

	/*
	 * The allocator also stops at the IOMMU aperture and the bus DMA
	 * limit. Either one below the window's top would make the
	 * reservation below fail or land lower; say which.
	 */
	limit = dom->end - 1;
	dma_dom = iommu_get_domain_for_dev(&vf_pdev->dev);
	aperture_end = dma_dom->geometry.force_aperture ?
		       (u64)dma_dom->geometry.aperture_end : U64_MAX;
	if (aperture_end < limit ||
	    (vf_pdev->dev.bus_dma_limit && vf_pdev->dev.bus_dma_limit < limit)) {
		dev_warn(&vf_pdev->dev,
			 "vfmig_iova: vf %u IOVA window [0x%llx, 0x%llx) is out of reach: IOMMU aperture ends at 0x%llx, bus DMA limit 0x%llx\n",
			 vf_id, dom->base, dom->end, aperture_end,
			 (u64)vf_pdev->dev.bus_dma_limit);
		err = -EOPNOTSUPP;
		goto err_pci_put;
	}

	/*
	 * Reserve before any other DMA traffic on this VF. With the mask,
	 * aperture and bus limit all reaching the window's top, a failure
	 * is a platform reservation inside the window; SET_TRACKED fails
	 * here with a printed reason rather than deep inside a later
	 * cmd-ring DMA.
	 */
	if (!dma_iova_try_alloc(&vf_pdev->dev, &dom->arena, 0,
				VFMIG_IOVA_PER_VF)) {
		dev_warn(&vf_pdev->dev,
			 "vfmig_iova: vf %u could not reserve IOVA window [0x%llx, 0x%llx) under a %u-bit DMA mask\n",
			 vf_id, dom->base, dom->end,
			 VFMIG_IOVA_DMA_MASK_BITS);
		err = -ENOSPC;
		goto err_pci_put;
	}
	if (dom->arena.addr != dom->base ||
	    dma_iova_size(&dom->arena) != VFMIG_IOVA_PER_VF) {
		dev_warn(&vf_pdev->dev,
			 "vfmig_iova: vf %u IOVA window reserved at [0x%llx, 0x%llx), not [0x%llx, 0x%llx)\n",
			 vf_id, (u64)dom->arena.addr,
			 (u64)dom->arena.addr + dma_iova_size(&dom->arena),
			 dom->base, dom->end);
		dma_iova_free(&vf_pdev->dev, &dom->arena);
		err = -ERANGE;
		goto err_pci_put;
	}

	dev_info(&vf_pdev->dev,
		 "vfmig_iova: vf %u IOVA window [0x%llx, 0x%llx) reserved under a %u-bit DMA mask (IOMMU aperture ends at 0x%llx, bus DMA limit 0x%llx)\n",
		 vf_id, dom->base, dom->end,
		 VFMIG_IOVA_DMA_MASK_BITS, aperture_end,
		 (u64)vf_pdev->dev.bus_dma_limit);

	*out = dom;
	return 0;

err_pci_put:
	pci_dev_put(dom->vf_pdev);
	dom->vf_pdev = NULL;
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
	 * reservation -- are already gone, so dma_iova_free() would
	 * dereference a NULL iommu_group. Skip the arena side in that case
	 * and only free memory we own.
	 */
	arena_live = vf_pdev && iommu_get_domain_for_dev(&vf_pdev->dev);
	if (arena_live)
		dma_iova_free(&vf_pdev->dev, &dom->arena);

	if (vf_pdev) {
		dev_info(&vf_pdev->dev,
			 "vfmig_iova: vf %u arena %s\n", dom->vf_id,
			 arena_live ? "released" :
				      "reclaimed with VF domain");
		pci_dev_put(vf_pdev);
	}

	kfree(dom);
}
