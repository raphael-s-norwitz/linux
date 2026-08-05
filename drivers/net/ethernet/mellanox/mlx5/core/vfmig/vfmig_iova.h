/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/* Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved. */

/*
 * Per-VF fixed-IOVA arena for mlx5 host-driven VF migration.
 *
 * Why
 * ---
 * SAVE_VHCA_STATE captures, inside the firmware blob, the source VHCA's
 * own IOVAs (cmd ring, host pages, EQ/UAR buffers). On native (non-VFIO,
 * non-VM) mlx5_core the destination's dma-iommu layer hands out fresh,
 * unrelated IOVAs, so after LOAD the firmware would dereference addresses
 * that point nowhere. The foundation of the fix is to keep each
 * migratable VF on its normal managed DMA-IOMMU domain and reserve a
 * fixed IOVA carveout in it with dma_iova_alloc_fixed(), so a later patch
 * can lay out deterministic, reproducible IOVAs inside it that survive a
 * SAVE on one host and a LOAD on another.
 *
 * Scope of this file
 * ------------------
 * Only the arena lifecycle: reserve on SET_TRACKED{enable=1}, release on
 * SET_TRACKED{enable=0} / SR-IOV teardown / PF unload. The deterministic
 * IOVA allocator and the SAVE/LOAD page replay that consume the carveout
 * land in later patches.
 *
 * Coexistence with dma-iommu
 * --------------------------
 * The VF keeps its dma-iommu-managed default domain, so ordinary
 * dma_alloc_coherent() / dma_map_*() on the VF continue to work
 * (streaming netdev traffic, cmd mailboxes) outside the carveout. Only
 * the reserved arena is PF-managed; a later layer populates it with
 * dma_iova_link(). Because the reservation must grab its exact fixed
 * range before any other DMA races it, the carveout is reserved at
 * SET_TRACKED.
 */

#ifndef __MLX5_CORE_VFMIG_IOVA_H__
#define __MLX5_CORE_VFMIG_IOVA_H__

#include <linux/types.h>

struct pci_dev;
struct vfmig_iova_domain;

/*
 * Per-VF IOVA window: [BASE + vf_id * PER_VF, BASE + (vf_id + 1) * PER_VF).
 * BASE is 4 GiB -- well clear of anything the default allocator hands out
 * and leaving the bottom 4 GiB untouched; PER_VF is 4 GiB, ample for the
 * cmd ring + MANAGE_PAGES host pages + EQ/UAR buffers a later layer maps.
 * The range is reserved inside the VF's managed DMA-IOMMU domain at attach
 * time (vfmig_iova_domain_create()).
 */
#define VFMIG_IOVA_BASE		0x100000000ULL	/* 4 GiB */
#define VFMIG_IOVA_PER_VF	(4ULL << 30)	/* 4 GiB per VF */

/*
 * Reserve the fixed per-VF IOVA carveout in @vf_pdev's managed DMA-IOMMU
 * domain and return the handle in *@out. @vf_pdev must be unbound and its
 * device_lock held by the caller. Returns 0 on success (with a pinned
 * reference on @vf_pdev held for the domain's lifetime), -EOPNOTSUPP if
 * the VF is not a DMA-coherent, dma-iommu-managed device, or a negative
 * errno if the exact IOVA range could not be reserved.
 */
int  vfmig_iova_domain_create(struct pci_dev *vf_pdev, u32 vf_id,
			      struct vfmig_iova_domain **out);

/*
 * Release @dom's arena reservation, drop the pinned VF reference, and free
 * it. The VF must be unbound. Safe with @dom == NULL.
 */
void vfmig_iova_domain_destroy(struct vfmig_iova_domain *dom);

#endif /* __MLX5_CORE_VFMIG_IOVA_H__ */
