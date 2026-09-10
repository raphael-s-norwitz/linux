/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/* Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved. */

/*
 * Per-VF unmanaged IOVA domain + deterministic slot allocator for
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

#include <linux/bits.h>
#include <linux/types.h>

struct iommu_domain;
struct pci_dev;
struct sg_table;
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
 *   VFMIG_SLOT_EQ_BUF   -- EQ frag buffers (async/cmd/comp EQs). Kept
 *                          separate so adding or removing an EQ on the
 *                          destination cannot shift the IOVAs of other
 *                          consumers (see create_map_eq / eq.c).
 *   VFMIG_SLOT_FRAG_BUF -- work-queue frag buffers (RQ/SQ/CQ rings),
 *                          allocated by the mlx5_wq_*_create helpers
 *                          (wq.c).
 *   VFMIG_SLOT_DB_PAGE  -- doorbell pgdir pages (mlx5_db_alloc_node /
 *                          mlx5_alloc_db_pgdir, alloc.c).
 *   VFMIG_SLOT_DMA_COHERENT -- catch-all for coherent host buffers not
 *                          routed to a per-purpose slot: the exported
 *                          mlx5_frag_buf_alloc_node() ABI used by
 *                          mlx5_ib / vfio_pci_mlx5 / vdpa. With this
 *                          slot every coherent allocation on a tracked
 *                          VF's probe path routes through the allocator,
 *                          so a fresh tracked VF can bind.
 *   VFMIG_SLOT_USER_PAGE -- user-space-pinned MR / CQ / QP / SRQ buffers
 *                          and doorbell records. ib_umem_get lands here
 *                          via the ib_core umem-placement hook; the
 *                          backing pages are umem-owned, so entries here
 *                          are external and skip page alloc/free. Unlike
 *                          the fixed kernel slots this slot is
 *                          "expand-to-fill": it spans from the kcoherent
 *                          carve's end (VFMIG_IOVA_KCOHERENT_BYTES) up to
 *                          the transient arena's base, so raising
 *                          CONFIG_MLX5_VFMIG_IOVA_PER_VF_GIB grows the
 *                          user-MR budget without shifting any kernel
 *                          slot's IOVAs. No call site routes through it
 *                          yet; this patch only carves the window.
 */
enum vfmig_iova_slot {
	VFMIG_SLOT_INVALID	= 0,
	VFMIG_SLOT_CMD_RING	= 1,
	VFMIG_SLOT_FW_PAGE	= 2,
	VFMIG_SLOT_EQ_BUF	= 3,
	VFMIG_SLOT_FRAG_BUF	= 4,
	VFMIG_SLOT_DB_PAGE	= 5,
	VFMIG_SLOT_DMA_COHERENT	= 6,
	VFMIG_SLOT_USER_PAGE	= 7,
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
 *                          Intel VT-d -> [0, 0x7fffffffff]). iommu_map()
 *                          returns -ERANGE outside it, so the window is
 *                          validated against the live aperture at attach
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
 * The deterministic per-VF range has an asymmetric layout since
 * VFMIG_SLOT_USER_PAGE was added:
 *
 *   - Kernel slots 0..VFMIG_IOVA_KERNEL_NR_SLOTS-1 (every slot except
 *     USER_PAGE) are fixed-size, each VFMIG_IOVA_SLOT_BYTES (510 MiB)
 *     wide. Slot N occupies [base + N*SLOT_BYTES, base + (N+1)*SLOT_BYTES).
 *     Slot 0 (VFMIG_SLOT_INVALID) is reserved and never allocated from.
 *   - The last slot, VFMIG_SLOT_USER_PAGE, is "expand-to-fill": its
 *     window runs from slot_base(USER_PAGE) up to the transient arena's
 *     base. The bottom VFMIG_IOVA_KCOHERENT_BYTES are reserved for the
 *     (future) non-migrated kcoherent sub-arena, so the user-MR IOVA
 *     range proper starts at slot_base(USER_PAGE) + KCOHERENT_BYTES.
 *
 * The 510 MiB kernel slot size is pinned (not scaled with PER_VF): the
 * kernel call-site footprint is bounded by hardware capabilities, not by
 * how much IOVA the admin hands us, and pinning it keeps a SAVE blob's
 * kernel-slot IOVAs PER_VF-independent -- only USER_PAGE's upper bound
 * scales with CONFIG_MLX5_VFMIG_IOVA_PER_VF_GIB.
 */
#define VFMIG_IOVA_NR_SLOTS		((unsigned int)VFMIG_SLOT_NR)
#define VFMIG_IOVA_KERNEL_NR_SLOTS	(VFMIG_IOVA_NR_SLOTS - 1U)
#define VFMIG_IOVA_SLOT_BYTES		(510ULL << 20)	/* 510 MiB, fixed */

/*
 * Transient sub-window: the topmost slice of each VF's IOVA window,
 * reserved for vfmig_iova_transient_get/put (short-lived, freelist-
 * recycled, single-page allocations -- cmd mailbox blocks). Sized to
 * hold the worst-case cmd-mailbox-cache footprint (~3900 pages); 16 MiB
 * == 4096 pages leaves headroom. It sits above the deterministic slot
 * range and must not overlap it (see static_assert below).
 */
#define VFMIG_IOVA_TRANSIENT_BYTES	(16ULL << 20)	/* 16 MiB */

/*
 * KCOHERENT sub-arena: reserved from the BOTTOM of VFMIG_SLOT_USER_PAGE's
 * window for the (future) non-migrated kernel-DMA arena that will back
 * the per-VF dma_ops .alloc/.free/.map_phys callbacks (e.g. mlx5e on a
 * tracked VF). Allocations there are never recorded in a SAVE manifest
 * and their IOVAs are not stable across migration.
 *
 * This patch only RESERVES the range; the arena allocator itself lands
 * in a later patch. Reserving it here pins USER_PAGE's effective start
 * (vfmig_iova_user_page_start()) so the user-MR sub-window base will not
 * shift when the arena is wired up.
 *
 * Introducing this carve shifts USER_PAGE's base up by KCOHERENT_BYTES,
 * a wire-incompatible change for USER_PAGE entries in any pre-existing
 * SAVE blob -- acceptable because USER_PAGE replay is not wired yet, so
 * no blob in the wild carries USER_PAGE records.
 */
#define VFMIG_IOVA_KCOHERENT_BYTES	BIT_ULL(30)	/* 1 GiB */

static_assert(VFMIG_IOVA_PER_VF >
	      (u64)VFMIG_IOVA_KERNEL_NR_SLOTS * VFMIG_IOVA_SLOT_BYTES +
	      VFMIG_IOVA_KCOHERENT_BYTES +
	      VFMIG_IOVA_TRANSIENT_BYTES,
	      "CONFIG_MLX5_VFMIG_IOVA_PER_VF_GIB too small: must fit the fixed 510-MiB kernel slots + the kcoherent carve + the transient arena + at least one user-MR IOVA");
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
 *		  backing page must be lowmem and iommu_map-compatible).
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

/*
 * Map a caller-owned physical address into the VFMIG_SLOT_USER_PAGE
 * window. Backs the ib_core umem-placement hook on tracked VFs: one
 * arena link per umem-pinned scatter-gather segment (user MR / CQ / QP /
 * SRQ buffers, doorbell records). Unlike vfmig_iova_alloc_slot() this
 * does not allocate backing memory -- @phys is supplied by the caller
 * (from sg_phys()) and stays pinned by the umem for the mapping's life.
 * The registry entry is flagged external (page/vaddr NULL), so
 * vfmig_iova_for_each() skips it and domain teardown does not free the
 * page.
 *
 * @phys must be VFMIG_IOVA_GRANULE-aligned; @len is rounded up to the
 * granule. On success *@iova_out is the allocated IOVA (page-aligned).
 * Bumps the USER_PAGE bump cursor by ALIGN(@len, GRANULE); no IOVA
 * reuse on unmap at this stage.
 *
 * Errors: -EINVAL bad args / misaligned @phys; -ENOSPC USER_PAGE window
 * exhausted; -EEXIST cursor IOVA already mapped (kernel bug); <0 from
 * iommu_map / allocation.
 */
int  vfmig_iova_user_page_map_phys(struct vfmig_iova_domain *dom,
				   phys_addr_t phys, size_t len, gfp_t gfp,
				   dma_addr_t *iova_out);

/*
 * Reverse of vfmig_iova_user_page_map_phys(): iommu_unmap the external
 * entry at @iova and drop it from the registry. @iova must be the value
 * _map_phys() returned; @len is rounded up to the granule. Returns
 * -ENOENT if no entry is mapped at @iova, -EINVAL if the entry is not a
 * USER_PAGE external mapping, 0 on success. Safe with @dom == NULL
 * (returns -EINVAL).
 */
int  vfmig_iova_user_page_unmap_phys(struct vfmig_iova_domain *dom,
				     dma_addr_t iova, size_t len);

/*
 * Allocate a single transient page from @dom's transient arena (the top
 * of the per-VF window). Transient allocations are short-lived, single
 * page (@size must be <= PAGE_SIZE), freelist-recycled, and -- unlike
 * slot allocations -- do NOT have a deterministic IOVA: they never
 * appear in a SAVE blob. Intended for the cmd mailbox block cache.
 *
 * @gfp:	must not include __GFP_HIGHMEM/COMP/DMA/DMA32.
 * @vaddr_out:	kernel-virtual base of the page.
 * @iova_out:	IOVA the firmware will see.
 *
 * Returns 0 on success, -ENOMEM if the arena is exhausted, -EINVAL on a
 * bad argument, or a negative errno from the page allocator / iommu core.
 */
int  vfmig_iova_transient_get(struct vfmig_iova_domain *dom,
			      size_t size, gfp_t gfp,
			      void **vaddr_out, dma_addr_t *iova_out);

/*
 * Return a page previously handed out by vfmig_iova_transient_get() to
 * the arena freelist (the mapping stays installed for reuse). @iova must
 * be an arena IOVA and @size <= PAGE_SIZE. Safe with @dom == NULL; logs
 * a warning on an out-of-range IOVA or a double free.
 */
void vfmig_iova_transient_put(struct vfmig_iova_domain *dom,
			      dma_addr_t iova, size_t size);

/*
 * Callback for vfmig_iova_for_each(): invoked once per deterministic-slot
 * registry entry, in IOVA-ascending order, with @dom->lock held. @vaddr is
 * the kernel-virtual base of the backing pages (safe to memcpy @len bytes
 * from). Returning non-zero stops the walk and is propagated to the caller.
 * The SAVE path uses this to snapshot the source VF's domain into HOST_PAGE
 * wire records.
 */
typedef int (*vfmig_iova_for_each_fn)(enum vfmig_iova_slot slot,
				      u64 instance_key, dma_addr_t iova,
				      const void *vaddr, size_t len, void *ctx);

/*
 * Walk every deterministic-slot registry entry in @dom in IOVA-ascending
 * order, invoking @cb for each. Returns 0 when the whole registry was
 * walked, the first non-zero @cb return otherwise, or -EINVAL on a bad
 * argument.
 */
int  vfmig_iova_for_each(struct vfmig_iova_domain *dom,
			 vfmig_iova_for_each_fn cb, void *ctx);

/*
 * User-object identity taxonomy for USER_PAGE external entries. A
 * tracked VF's user MR (and, as support lands, CQ / QP / SRQ buffers and
 * doorbell records) flow through vfmig_dma_ops.map_sg as external
 * registry entries with an auto-numbered @instance_key (kind byte 0 ==
 * KIND_NONE). A later slice retags each entry, post-FW-create, with
 * VFMIG_HUOBJ_KEY(kind, fw_id) so SAVE can emit a stable (kind, fw_id)
 * identity the destination replays. @instance_key packs an 8-bit kind in
 * the top byte (so at most 256 kinds) and the 56-bit FW resource id
 * (mkey_index / cqn / qpn / srqn / dbr VA) below. Additional kinds are
 * appended as their retag callsites land.
 */
enum vfmig_huobj_kind {
	VFMIG_HUOBJ_KIND_NONE	= 0,	/* auto-numbered / un-retagged */
	VFMIG_HUOBJ_KIND_MR	= 1,	/* user memory region (mkey_index) */
	VFMIG_HUOBJ_KIND_CQ	= 2,	/* user completion queue (cqn) */
	VFMIG_HUOBJ_KIND_QP	= 3,	/* user queue pair (qpn) */
	VFMIG_HUOBJ_KIND_SRQ	= 4,	/* user shared receive queue (srqn) */
	VFMIG_HUOBJ_KIND_DBR	= 5,	/* user doorbell page (user VA) */
	VFMIG_HUOBJ_KIND_NR,            /* count; must stay <= 256 */
};

#define VFMIG_HUOBJ_FWID_BITS	56U
#define VFMIG_HUOBJ_FWID_MASK	((1ULL << VFMIG_HUOBJ_FWID_BITS) - 1ULL)
#define VFMIG_HUOBJ_KEY(kind, fw_id)				\
	((((u64)(kind)) << VFMIG_HUOBJ_FWID_BITS) |		\
	 ((u64)(fw_id) & VFMIG_HUOBJ_FWID_MASK))
#define VFMIG_HUOBJ_KIND(key)					\
	((u8)(((u64)(key)) >> VFMIG_HUOBJ_FWID_BITS))
#define VFMIG_HUOBJ_FWID(key)					\
	(((u64)(key)) & VFMIG_HUOBJ_FWID_MASK)

/*
 * Callback for vfmig_iova_for_each_external(): invoked once per external
 * (USER_PAGE, caller-owned) registry entry, in IOVA-ascending order,
 * with @dom->lock held. @kind / @fw_id are decoded from the entry's
 * retagged @instance_key (both zero for a still-auto-numbered entry).
 * Unlike vfmig_iova_for_each() there is no @vaddr: the backing page is
 * umem-owned and its contents are CRIU's responsibility, so the SAVE
 * path emits identity-only HOST_USER_PAGE records. Returning non-zero
 * stops the walk and is propagated to the caller.
 */
typedef int (*vfmig_iova_for_each_external_fn)(u8 kind, u64 fw_id,
					       dma_addr_t iova, size_t len,
					       void *ctx);

/*
 * Walk every external registry entry in @dom in IOVA-ascending order,
 * invoking @cb for each. Non-external (deterministic-slot) entries are
 * skipped -- they are covered by vfmig_iova_for_each(). Returns 0 when
 * the whole registry was walked, the first non-zero @cb return
 * otherwise, or -EINVAL on a bad argument.
 */
int  vfmig_iova_for_each_external(struct vfmig_iova_domain *dom,
				  vfmig_iova_for_each_external_fn cb,
				  void *ctx);

/*
 * Replay one HOST_PAGE record into @dom on the LOAD/destination side:
 * install a backing page at the wire-provided deterministic @iova in
 * @slot and memcpy @len bytes of @contents into it. Must run before the
 * destination VF probes (so its allocator re-claims the replayed pages
 * with the source's contents). @iova must fall inside @slot's window
 * (cross-checked against the destination's own partitioning) and @dom
 * must not yet be drift-armed. Returns 0, or a negative errno
 * (-ERANGE on a slot/IOVA mismatch, -EEXIST on a duplicate IOVA, etc).
 */
int  vfmig_iova_replay_page(struct vfmig_iova_domain *dom,
			    enum vfmig_iova_slot slot, u64 instance_key,
			    dma_addr_t iova, const void *contents, size_t len);

/*
 * Replay one HOST_USER_PAGE record into @dom on the LOAD/destination
 * side: install an awaiting-bind placeholder reserving [iova, iova+len)
 * in @slot (must be VFMIG_SLOT_USER_PAGE) for the (kind, fw_id) packed
 * into @instance_key (kind byte must be != NONE). Carries no contents --
 * unlike vfmig_iova_replay_page() it installs no iommu_map; the umem is
 * bound later. Must run before the domain is drift-armed. Returns 0, or a
 * negative errno (-ERANGE on an out-of-window IOVA, -EEXIST on a
 * duplicate, -EINVAL on a bad slot/identity/alignment).
 */
int  vfmig_iova_replay_external(struct vfmig_iova_domain *dom,
				enum vfmig_iova_slot slot, u64 instance_key,
				dma_addr_t iova, size_t len);

/*
 * Source-side retag: overwrite the auto-numbered @instance_key of every
 * external registry entry overlapping [@iova_base, @iova_base + @length)
 * with @new_instance_key, promoting KIND_NONE placeholders that
 * vfmig_dma_ops planted at umem-map time into (kind, fw_id)-keyed
 * entries. @iova_base and @length must be VFMIG_IOVA_GRANULE-aligned and
 * @new_instance_key's kind byte must be != NONE. A multi-page user object
 * yields one entry per source-side sg, all retagged with the same key.
 * Idempotent for an entry already carrying @new_instance_key. Returns the
 * number-agnostic 0 on success, -ENOENT if the range holds no external
 * entries (caller went through a non-vfmig DMA path), -EEXIST if an entry
 * is already claimed by a different (kind, fw_id) (source-side bug; every
 * entry retagged in this call is rolled back to KIND_NONE), or -EINVAL on
 * a bad argument. Runs on the SAVE/source side, before drift-arming.
 */
int  vfmig_iova_retag_external_range(struct vfmig_iova_domain *dom,
				     dma_addr_t iova_base, size_t length,
				     u64 new_instance_key);

/*
 * Destination-side bind: map a freshly-pinned umem @sgt onto the
 * awaiting_bind=true placeholder chain that vfmig_iova_replay_external()
 * installed for the (kind, fw_id) packed via VFMIG_HUOBJ_KEY. Resolves
 * the placeholder head through the (instance_key, iova) secondary index,
 * validates the sibling chain (all external USER_PAGE, still awaiting,
 * iova-contiguous) and that @sgt's byte coverage equals the placeholder
 * total, then issues one iommu_map per dst sg at consecutive IOVAs from
 * the head placeholder's iova and clears awaiting_bind all-or-nothing.
 * @kind must be a valid non-NONE VFMIG_HUOBJ_KIND. Returns 0, -ENOENT
 * (no placeholder), -EBUSY (already bound), -EINVAL (bad args / length
 * mismatch / mis-shaped sg), or an iommu_map errno (rolled back). On any
 * non-zero return the caller MUST NOT consume sg_dma_address.
 */
int  vfmig_iova_bind_user_object(struct vfmig_iova_domain *dom,
				 u8 kind, u64 fw_id, struct sg_table *sgt);

/*
 * Rewind every per-slot bump cursor to its slot base (and per-slot
 * auto-key counters to 0) so the destination VF's probe re-claims the
 * replayed pages from the bottom of each slot in the same order the
 * source allocated them. Called once at LOAD-fd release, after all
 * HOST_PAGE records have been replayed. Safe with @dom == NULL.
 */
void vfmig_iova_reset_cursor(struct vfmig_iova_domain *dom);

/*
 * Freeze @dom's replayed footprint: record that replay is complete so
 * subsequent alloc_slot() calls can diagnose drift between the
 * destination's claim sequence and the source's recorded one. Called
 * once at the end of a LOAD after the wire manifest CRC has verified.
 * Diagnostic only (logs the per-slot replay counts); safe with
 * @dom == NULL.
 */
void vfmig_iova_arm_drift_detection(struct vfmig_iova_domain *dom);

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

static inline int
vfmig_iova_user_page_map_phys(struct vfmig_iova_domain *dom,
			      phys_addr_t phys, size_t len, gfp_t gfp,
			      dma_addr_t *iova_out)
{
	return -EOPNOTSUPP;
}

static inline int
vfmig_iova_user_page_unmap_phys(struct vfmig_iova_domain *dom,
				dma_addr_t iova, size_t len)
{
	return -EOPNOTSUPP;
}

static inline int
vfmig_iova_transient_get(struct vfmig_iova_domain *dom,
			 size_t size, gfp_t gfp,
			 void **vaddr_out, dma_addr_t *iova_out)
{
	return -EOPNOTSUPP;
}

static inline void
vfmig_iova_transient_put(struct vfmig_iova_domain *dom,
			 dma_addr_t iova, size_t size)
{
}

#endif /* CONFIG_MLX5_VFMIG */

#endif /* __MLX5_CORE_VFMIG_IOVA_H__ */
