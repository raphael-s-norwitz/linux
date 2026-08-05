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
 * fixed IOVA carveout in it with dma_iova_try_alloc(), so a later patch
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

#include <linux/bits.h>
#include <linux/types.h>

struct pci_dev;
struct vfmig_iova_domain;

/*
 * IOVA window layout (hardware-visible addresses).
 *
 *   VFMIG_IOVA_DMA_MASK_BITS -- the DMA mask, 39 bits, the VF carries
 *                          when its window is reserved.
 *   VFMIG_IOVA_PER_VF   -- IOVA space per VF, 256 GiB: half of what that
 *                          mask reaches.
 *   VFMIG_IOVA_BASE     -- base of every VF's window, 256 GiB.
 *
 * The window is reserved with the public dma_iova_try_alloc(), which
 * takes no address. Its allocator aligns a request to the request's
 * size rounded up to a power of two and searches down from the DMA
 * mask, so 256 GiB under a 39-bit mask can only land at
 * [256 GiB, 512 GiB): the next aligned candidate down starts at IOVA 0,
 * which the allocator never hands out. The result therefore does not
 * depend on the IOMMU's address width, as long as its aperture reaches
 * 512 GiB. It fails instead if that range is not free -- a platform
 * reservation such as a PCI host-bridge window inside it, or a smaller
 * aperture. The placement is the allocator's behavior, not an API
 * guarantee, so vfmig_iova_domain_create() checks the returned range
 * and fails SET_TRACKED rather than use a window anywhere else.
 *
 * Each VF reserves the same [BASE, BASE + PER_VF) in its own managed
 * domain, so an image replays onto any VF of the PF.
 */
#define VFMIG_IOVA_DMA_MASK_BITS	39
#define VFMIG_IOVA_PER_VF	BIT_ULL(VFMIG_IOVA_DMA_MASK_BITS - 1)
#define VFMIG_IOVA_BASE		VFMIG_IOVA_PER_VF

static_assert(VFMIG_IOVA_BASE + VFMIG_IOVA_PER_VF ==
	      BIT_ULL(VFMIG_IOVA_DMA_MASK_BITS),
	      "the window must be the size-aligned top of the reservation mask");

/*
 * Reserve the fixed per-VF IOVA carveout in @vf_pdev's managed DMA-IOMMU
 * domain and return the handle in *@out. @vf_pdev must be unbound and its
 * device_lock held by the caller. Sets @vf_pdev's DMA mask to
 * VFMIG_IOVA_DMA_MASK_BITS and leaves it there; the driver that binds
 * next may raise it. Returns 0 on success (with a pinned reference on
 * @vf_pdev held for the domain's lifetime), -EOPNOTSUPP if the VF is not
 * a DMA-coherent, dma-iommu-managed device or its IOMMU aperture or bus
 * DMA limit stops short of the window, -EIO if the mask cannot be set,
 * -ENOSPC if the window could not be reserved, or -ERANGE if it was
 * reserved anywhere but [VFMIG_IOVA_BASE, VFMIG_IOVA_BASE + PER_VF).
 */
int  vfmig_iova_domain_create(struct pci_dev *vf_pdev, u32 vf_id,
			      struct vfmig_iova_domain **out);

/*
 * Release @dom's arena reservation, drop the pinned VF reference, and free
 * it. The VF must be unbound. Safe with @dom == NULL.
 */
void vfmig_iova_domain_destroy(struct vfmig_iova_domain *dom);

#endif /* __MLX5_CORE_VFMIG_IOVA_H__ */
