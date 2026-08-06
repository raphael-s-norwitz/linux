/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/* Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved. */

/*
 * Per-VF fixed-IOVA arena + deterministic slot allocator for
 * mlx5 host-driven VF migration.
 *
 * Why
 * ---
 * SAVE_VHCA_STATE captures, inside the firmware blob, the source
 * VHCA's own IOVAs (cmd ring, host pages, EQ/UAR buffers). On native
 * (non-VFIO, non-VM) mlx5_core the destination's dma-iommu layer hands
 * out fresh, unrelated IOVAs, so after LOAD the firmware would
 * dereference addresses that point nowhere. The fix, built in layers:
 *
 *   1. Keep each migratable VF on its normal managed DMA-IOMMU domain
 *      and reserve a fixed IOVA carveout in it with
 *      dma_iova_alloc_fixed() (this file's domain lifecycle). The VF's
 *      ordinary DMA-API traffic still works; only the carveout is
 *      PF-managed.
 *   2. Lay out a fixed per-VF IOVA window at a high, well-known base so
 *      the source's IOVAs are reproducible on the destination.
 *   3. Provide a slot-tagged allocator (vfmig_iova_alloc_slot) that the
 *      mlx5_core probe path uses instead of dma_alloc_coherent for
 *      every DMA buffer the firmware records an IOVA for. Each caller
 *      declares which kind of allocation it is (CMD_RING, FW_PAGE, ...)
 *      and gets a deterministic IOVA from that slot's dedicated
 *      sub-window (routing of the individual call sites lands in later
 *      patches).
 *
 * Determinism contract
 * --------------------
 * Every converted call site declares which slot its allocation belongs
 * to (enum vfmig_iova_slot). Each slot owns its own VFMIG_IOVA_SLOT_BYTES
 * sub-window and its own bump cursor, so adding, removing, or reordering
 * allocations in one slot cannot shift IOVAs in another -- the per-slot
 * windows are a fixed partition. Within a slot, source/destination IOVA
 * equivalence rests on call-site discipline: the same set of allocations
 * of the same sizes in the same order. The instance_key argument lets a
 * caller pin a specific (slot, key) -> IOVA mapping when it has a stable
 * identifier; passing 0 falls back to per-slot auto-numbering.
 *
 * Coexistence with dma-iommu
 * --------------------------
 * The VF keeps its dma-iommu-managed default domain, so ordinary
 * dma_alloc_coherent() / dma_map_*() on the VF continue to work
 * (streaming netdev traffic, cmd mailboxes) outside the carveout. The
 * reserved arena is populated with dma_iova_link(); those IOVAs are the
 * only PF-managed, migration-recorded addresses. Because the reservation
 * must grab its exact fixed range before any other DMA races it, the
 * carveout is reserved at SET_TRACKED.
 */

#ifndef __MLX5_CORE_VFMIG_IOVA_H__
#define __MLX5_CORE_VFMIG_IOVA_H__

#include <linux/types.h>

struct pci_dev;
struct vfmig_iova_domain;

/*
 * Slot identity. Each value names one category of converted allocation;
 * the (slot, instance_key) pair plus size identifies a specific
 * allocation within that category.
 *
 * Slots are introduced one at a time, together with the call site that
 * routes through them (see the layered restore plan). Always append new
 * values before VFMIG_SLOT_NR and never renumber existing enumerators --
 * the numeric value is the slot's IOVA base offset within the per-VF
 * window, and stable IOVAs are the point of this subsystem.
 *
 *   VFMIG_SLOT_INVALID  -- sentinel; alloc_slot rejects it.
 *   VFMIG_SLOT_CMD_RING -- cmd ring DMA buffer (one per VF), allocated
 *                          by mlx5_cmd_enable during probe.
 *   VFMIG_SLOT_FW_PAGE  -- FW-owned pages handed over via MANAGE_PAGES
 *                          OP_GIVE (boot/init/dynamic), one allocation
 *                          per page (see alloc_system_page).
 */
enum vfmig_iova_slot {
	VFMIG_SLOT_INVALID	= 0,
	VFMIG_SLOT_CMD_RING	= 1,
	VFMIG_SLOT_FW_PAGE	= 2,
	VFMIG_SLOT_NR,		/* count; drives VFMIG_IOVA_NR_SLOTS */
};

/*
 * IOVA window layout (hardware-visible addresses).
 *
 *   VFMIG_IOVA_BASE     -- per-PF base, 4 GiB. The window
 *                          [BASE, BASE + N * PER_VF) must fit entirely
 *                          inside the IOMMU's geometry aperture, whose
 *                          upper bound the iommu driver derives from the
 *                          hardware address width (e.g. 39-bit on some
 *                          Intel VT-d -> [0, 0x7fffffffff]).
 *                          dma_iova_alloc_fixed() returns -ERANGE outside
 *                          it, so the window is validated against the
 *                          live aperture at reservation
 *                          (vfmig_iova_domain_create()) and SET_TRACKED
 *                          fails with -EOPNOTSUPP + a printed diagnostic
 *                          rather than a later opaque cmd-ring DMA
 *                          failure. Base 4 GiB leaves the bottom 4 GiB
 *                          untouched and makes IOVAs visually distinct
 *                          from anything the default allocator produces.
 *                          VFs of the same PF get distinct sub-windows
 *                          (BASE + vf_id * PER_VF) so an IOVA value alone
 *                          identifies the owning VF in dmesg.
 *   VFMIG_IOVA_PER_VF   -- IOVA space per VF, configurable via
 *                          CONFIG_MLX5_VFMIG_IOVA_PER_VF_GIB (GiB;
 *                          default 4). Ample for the cmd ring +
 *                          MANAGE_PAGES host pages + EQ/UAR buffers a
 *                          later layer maps.
 *   VFMIG_IOVA_GRANULE  -- minimum allocation alignment; matches
 *                          PAGE_SIZE (also the mlx5 hardware page size).
 */

#if IS_ENABLED(CONFIG_MLX5_VFMIG)

#define VFMIG_IOVA_BASE		0x100000000ULL		/* 4 GiB */
#define VFMIG_IOVA_PER_VF \
	((u64)CONFIG_MLX5_VFMIG_IOVA_PER_VF_GIB << 30)
#define VFMIG_IOVA_GRANULE	PAGE_SIZE

/*
 * The deterministic per-VF range is partitioned across
 * VFMIG_IOVA_NR_SLOTS fixed-size slot windows, each
 * VFMIG_IOVA_SLOT_BYTES wide. Slot N occupies
 *   [base + N * SLOT_BYTES, base + (N + 1) * SLOT_BYTES).
 * Slot 0 (VFMIG_SLOT_INVALID) is reserved and never allocated from.
 * NR_SLOTS tracks the enum, so appending a slot enumerator grows the
 * deterministic window (and the per-slot cursor arrays) by one slot;
 * existing slots keep their IOVA base offset.
 *
 * The 510 MiB slot size is pinned (not scaled with PER_VF): the kernel
 * call-site footprint is bounded by hardware capabilities, not by how
 * much IOVA the admin hands us, and pinning it keeps a SAVE blob's
 * kernel-slot IOVAs PER_VF-independent.
 */
#define VFMIG_IOVA_NR_SLOTS	((unsigned int)VFMIG_SLOT_NR)
#define VFMIG_IOVA_SLOT_BYTES	(510ULL << 20)	/* 510 MiB, fixed */

static_assert(VFMIG_IOVA_PER_VF >
	      (u64)VFMIG_IOVA_NR_SLOTS * VFMIG_IOVA_SLOT_BYTES,
	      "CONFIG_MLX5_VFMIG_IOVA_PER_VF_GIB too small: must fit all fixed 510-MiB slots");
static_assert(VFMIG_IOVA_SLOT_BYTES >= (8ULL << 20),
	      "VFMIG_IOVA_SLOT_BYTES must be >= 8 MiB to host worst-case kernel allocations");

/*
 * Reserve the fixed per-VF IOVA carveout in @vf_pdev's managed DMA-IOMMU
 * domain and return the handle in *@out. Returns 0 on success (with a
 * pinned reference on @vf_pdev held for the domain's lifetime),
 * -EOPNOTSUPP if the VF is not a DMA-coherent, dma-iommu-managed device,
 * or a negative errno if the exact IOVA range could not be reserved.
 */
int  vfmig_iova_domain_create(struct pci_dev *vf_pdev, u32 vf_id,
			      struct vfmig_iova_domain **out);

/*
 * Unlink + free every registry page, release the arena reservation, and
 * drop the pinned VF reference. Safe with @dom == NULL.
 */
void vfmig_iova_domain_destroy(struct vfmig_iova_domain *dom);

/*
 * Allocate @size bytes of DMA-able memory from @slot's sub-window at the
 * slot's next deterministic IOVA. Backing pages are kernel-owned and
 * zeroed; the mapping is linked into the per-VF arena with
 * dma_iova_link(DMA_BIDIRECTIONAL) (IOMMU_CACHE on the coherent device
 * the arena requires).
 *
 * @instance_key: caller-pinned per-slot identity, or 0 for per-slot
 *		  auto-numbering.
 * @gfp:	  must not include __GFP_HIGHMEM/COMP/DMA/DMA32 (the
 *		  backing page must be lowmem and dma_iova_link-compatible).
 * @iova_out:	  the deterministic IOVA the firmware will see.
 * @vaddr_out:	  the kernel-virtual base of the backing memory.
 *
 * Returns 0 on success, -ENOSPC if @slot's window is exhausted,
 * -EINVAL on a bad slot / argument, or a negative errno from the page
 * allocator / iommu core.
 */
int  vfmig_iova_alloc_slot(struct vfmig_iova_domain *dom,
			   enum vfmig_iova_slot slot, u64 instance_key,
			   size_t size, gfp_t gfp,
			   dma_addr_t *iova_out, void **vaddr_out);

/*
 * Free a mapping previously returned by vfmig_iova_alloc_slot(). @slot,
 * @iova and @size must match the alloc. Safe with @dom == NULL; logs a
 * warning on an unknown IOVA or size mismatch.
 */
void vfmig_iova_free_slot(struct vfmig_iova_domain *dom,
			  enum vfmig_iova_slot slot,
			  dma_addr_t iova, size_t size);

#else /* !CONFIG_MLX5_VFMIG */

/*
 * Stubs for the not-config-gated call sites (e.g. cmd.c). The tracked-VF
 * routing is reached only when mlx5_vf_get_vfmig_iova_domain() returns
 * non-NULL, which its !CONFIG stub never does, so these are unreachable
 * at CONFIG_MLX5_VFMIG=n and exist purely to keep those call sites free
 * of #ifdef.
 */
static inline int
vfmig_iova_alloc_slot(struct vfmig_iova_domain *dom,
		      enum vfmig_iova_slot slot, u64 instance_key,
		      size_t size, gfp_t gfp,
		      dma_addr_t *iova_out, void **vaddr_out)
{
	return -EOPNOTSUPP;
}

static inline void
vfmig_iova_free_slot(struct vfmig_iova_domain *dom,
		     enum vfmig_iova_slot slot,
		     dma_addr_t iova, size_t size)
{
}

#endif /* CONFIG_MLX5_VFMIG */

#endif /* __MLX5_CORE_VFMIG_IOVA_H__ */
