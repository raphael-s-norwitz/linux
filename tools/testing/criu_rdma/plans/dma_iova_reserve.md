# DESIGN: reserving restorable IOVA ranges with `dma_iova_alloc_fixed()`

> **Status (2026-08-25): proposal; not implemented.**
>
> This document records the proposed DMA API needed to replace VFMIG's
> `dma_ops` override with an ordinary DMA-IOMMU-managed address space.

## 1. Problem

VFMIG must restore device-visible addresses because the mlx5 firmware image
contains IOVAs for objects such as user MRs, queue buffers, doorbell records,
and selected mlx5_core resources. Destination physical pages may differ from
the source pages, but the IOVAs referenced by restored firmware must remain
the same.

The normal DMA API chooses a new IOVA for each mapping. The existing
IOVA-based DMA API separates IOVA allocation from physical-page mapping, but
`dma_iova_try_alloc()` can only allocate an arbitrary range:

```c
bool dma_iova_try_alloc(struct device *dev,
			struct dma_iova_state *state,
			phys_addr_t phys, size_t size);
```

Its `phys` argument is used only to preserve the physical address's offset
within the IOMMU granule. Passing zero is valid for page-aligned mappings, but
the API cannot reserve a source-selected address during restore. Its boolean
return also conflates unsupported devices with allocation failure, which is
appropriate for an optional DMA optimization but not a mandatory migration
operation.

## 2. Proposed API

```c
int dma_iova_alloc_fixed(struct device *dev,
			 struct dma_iova_state *state,
			 dma_addr_t addr,
			 size_t size);
```

Both sides of migration reserve the *same* caller-chosen base (see Part II
S11); there is no arbitrary-address mode, so the fixed semantics are baked
into the name rather than carried by a flag:

```c
/* Source and destination both pin the same base. */
ret = dma_iova_alloc_fixed(dev, &arena, VFMIG_ARENA_IOVA_BASE, arena_size);
```

The verb is deliberately `alloc`, not `reserve`: this returns an
independently-owned, freeable allocation, and the name must not be confused
with the platform-window `reserve_iova()` it is built beside (see S18). It
also stays in the `dma_iova_*` family and mirrors the kernel's existing
layering, where `dma_iova_try_alloc()` wraps the `alloc_iova_fast()`
primitive; here `dma_iova_alloc_fixed()` wraps the new `alloc_iova_fixed()`
primitive (S3).

The proposed return values are:

* `0`: range reserved and recorded in `state`.
* `-EOPNOTSUPP`: the device is not using DMA-IOMMU.
* `-EINVAL`: invalid size, alignment, DMA mask, bus limit, or aperture.
* `-EBUSY`: the requested range overlaps an existing allocation or reservation.
* `-ENOMEM`: allocator metadata allocation failed.

The reservation allocates only IOVA address space. It allocates no physical
memory and installs no IOMMU page-table entries.

## 3. Implementation model

The existing arbitrary allocator (`dma_iova_try_alloc()`) reaches the IOVA
layer through:

```text
iommu_get_dma_domain()
  -> domain->iova_cookie
    -> cookie->iovad
      -> iommu_dma_alloc_iova()
        -> alloc_iova_fast()
```

`dma_iova_alloc_fixed()` follows the same descent to obtain `cookie->iovad`
(reusing the DMA-mask, bus-limit, aperture, and deferred-attach handling),
but replaces the final `alloc_iova_fast()` with a new IOVA-layer primitive:

```c
int alloc_iova_fixed(struct iova_domain *iovad,
		     unsigned long pfn_lo,
		     unsigned long pfn_hi);
```

It must atomically insert exactly the requested interval or fail: `0` on an
exact reservation, `-EBUSY` on any overlap, `-ENOMEM` on metadata OOM. It
accounts for the IOVA range caches implicitly -- a cached pfn always still
occupies its rbtree node (S17), so a clean overlap scan of the tree suffices
and no rcache flush is required.

There is **no `struct iova **` out-parameter** (an earlier sketch had one).
A fixed reservation is a *single* contiguous rbtree node -- not multiple
`struct iova`s; the `**` was merely an out-pointer to that one node -- and
the caller never needs to hold it. The node is discoverable by address, and
the paired free (S19) looks it up by pfn; handing back a raw `struct iova *`
would only add a lifetime hazard (it can be freed/merged underneath the
holder). The address itself is already returned one layer up in
`struct dma_iova_state`, exactly like `dma_iova_try_alloc()` -- so the public
API needs no `struct iova` at all. This also matches how dma-iommu already
treats IOVAs everywhere else: it allocates and frees them *by address*
(`iommu_dma_alloc_iova()` returns a bare `dma_addr_t`), never by node handle.

The existing `reserve_iova()` is not sufficient as the public implementation.
It is intended to mark platform-owned windows unavailable, may merge
overlapping reservations, does not provide an independently owned runtime
allocation, and does not by itself provide the DMA-IOMMU validation above.

## 4. Mapping within the reservation

No new page-mapping API is required for the initial implementation.
`dma_iova_link()` already maps a physical range at an offset within a
`dma_iova_state`:

```c
ret = dma_iova_link(dev, &arena, phys, object_offset, object_size,
		    DMA_BIDIRECTIONAL, attrs);
```

The resulting device address is:

```text
arena.addr + object_offset
```

Multiple links may be batched before making the page-table updates visible:

```c
for_each_page(page) {
	ret = dma_iova_link(dev, &arena, page_to_phys(page), offset,
			    PAGE_SIZE, direction, attrs);
	if (ret)
		goto rollback;
	offset += PAGE_SIZE;
}

ret = dma_iova_sync(dev, &arena, object_offset, object_size);
```

`dma_iova_sync()` synchronizes IOMMU page-table updates; it is not a CPU-cache
ownership operation. `dma_iova_link()` and `dma_iova_unlink()` retain the
normal DMA direction, protection, SWIOTLB, and non-coherent architecture
handling.

Individual objects are removed with:

```c
dma_iova_unlink(dev, &arena, object_offset, object_size,
		direction, attrs);
```

After every object has been unlinked, `dma_iova_free_fixed()` releases the
complete arena reservation (a non-caching free; see S19).

## 5. Existing precedents

Current `dma_iova_*` users fall into two patterns:

* **VFIO mlx5** maps separately allocated pages backing migration-image and
  page-tracker MKEYs into one dense IOVA range. Sequential IOVAs are written
  into the MKEY MTT, all page-table updates share one `dma_iova_sync()`, and
  `dma_map_page()` per page is the fallback. This is an optimization; the MTT
  can also contain unrelated DMA addresses.
* **The block layer** similarly coalesces a request's physical vectors into
  one DMA range, reducing device descriptor pressure and batching IOTLB
  synchronization.
* **HMM/ODP** is the closest arena precedent. It reserves an IOVA state for
  the full faultable range, then dynamically links and unlinks individual PFNs
  at index-derived offsets as pages enter and leave the working set. HMM
  rejects devices that require CPU cache synchronization, as users of this
  path cannot perform streaming-DMA ownership transitions for every faulted
  page; it also rejects DMA-address-limited devices to avoid SWIOTLB buffering.
* **The dma-buf physical-vector helper** uses `DMA_ATTR_MMIO` to place P2P
  device-memory ranges behind an IOVA-backed SG entry when traffic traverses
  the host bridge. Its current multi-range loop passes offset zero for every
  vector and should not be treated as a VFMIG model until that apparent
  overlap is resolved.
* **mlx5_core VFMIG staging buffers** duplicate the VFIO mlx5 MKEY pattern.
  These are temporary SAVE/LOAD command buffers, not the deterministic IOVAs
  referenced by restored firmware objects.

None of these callers supplies a numerical base IOVA. They accept the base
chosen by `dma_iova_try_alloc()` and control only offsets within it. VFMIG
combines HMM's long-lived, dynamically populated arena with VFIO mlx5's dense
page-linking pattern; restoring the arena at a caller-specified base is the
new capability.

## 6. VFMIG lifecycle

```text
Source SET_TRACKED
  -> reserve arbitrary arena
  -> suballocate mlx5 objects within the arena
  -> link physical pages at object offsets
  -> save arena base, size, and object offsets

Destination SET_TRACKED / LOAD
  -> reserve the saved arena base and size with FIXED
  -> recreate or pin destination physical pages
  -> link them at their saved object offsets
  -> restore firmware state that references those IOVAs

Teardown
  -> unlink every live object
  -> free the arena reservation
```

VFMIG may keep its slot or range allocator for suballocation. The DMA layer
owns the outer arena in the normal DMA-IOMMU allocator; VFMIG owns placement
inside that arena.

## 7. Coherent allocations

This proposal initially covers existing or pinned pages mapped with
`dma_iova_link()`. It does not by itself provide a fixed-IOVA replacement for
`dma_alloc_coherent()`.

Coherent allocation couples three lifetimes that a restorable arena separates:

1. CPU-visible backing memory and architecture-specific cache attributes.
2. IOMMU page-table mappings for an individual object.
3. The outer IOVA reservation retained across many object allocations.

A future fixed-IOVA coherent helper could reuse dma-iommu's page allocation,
`arch_dma_prep_coherent()`, CPU remapping, and IOMMU mapping internals, but it
would need a paired free operation that removes only the object's mapping and
backing pages without returning the containing arena to the IOVA allocator.
Atomic pools, non-coherent ARM devices, DMA attributes, and memory encryption
would all need defined behavior. This is deliberately separate from the
minimal reservation proposal.

## 8. Coherent-only initial profile

VFMIG can avoid proposing a fixed-IOVA variant of `dma_alloc_coherent()` in
its initial implementation by requiring a DMA-coherent device:

```c
if (!use_dma_iommu(dev))
	return -EOPNOTSUPP;
if (!dev_is_dma_coherent(dev))
	return -EOPNOTSUPP;
```

On such a device, `dma_iova_link()` installs mappings with `IOMMU_CACHE`,
ordinary cached kernel mappings are valid for CPU/device-shared pages, and
the architecture cache-maintenance operations in the link/unlink path are
unnecessary. VFMIG can therefore:

1. Reserve the migratable arena with `dma_iova_alloc_fixed()`.
2. Allocate ordinary, page-aligned backing pages for persistent kernel
   resources and link them into their assigned arena offsets with
   `DMA_BIDIRECTIONAL`.
3. Link pinned user pages into their saved offsets in the same way.
4. Keep non-restorable resources, such as command mailboxes, on the ordinary
   DMA API outside the arena.
5. Reject VFMIG on genuinely non-coherent devices.

The reserved arena must satisfy `dev->coherent_dma_mask` when it contains
resources that previously came from `dma_alloc_coherent()`. Call sites must
also be audited for assumptions beyond cache coherence, including contiguous
CPU virtual mappings, atomic allocation, special DMA attributes, highmem,
and memory encryption. The current mlx5 tracked resources are favorable:
the command ring is one page, and EQ, queue, and doorbell buffers are already
managed as page fragments.

This is a deliberately constrained implementation, not a general claim that
streaming mappings replace coherent allocation. `dma_iova_link()` belongs to
the streaming DMA API family even when its hardware mapping is coherent.
DMA maintainers may require an explicit coherent-at-reserved-IOVA API to
preserve the formal coherent-allocation contract. If so, that becomes a
follow-on to the reservation API rather than a prerequisite for validating
the underlying IOVA design.

The check is on the device property, not the CPU architecture. An arm64
platform may provide fully coherent PCIe DMA, while an embedded x86-attached
device or a non-PCI device may have different constraints. The authoritative
runtime predicate is `dev_is_dma_coherent(dev)`.

## 9. Initial scope

The minimal proposed change is:

1. Add an exact, collision-detecting internal IOVA allocation primitive.
2. Add `dma_iova_alloc_fixed()` for reserving the caller-chosen fixed range.
3. Use existing `dma_iova_link()`, `dma_iova_sync()`,
   `dma_iova_unlink()`, and `dma_iova_free_fixed()` for page-backed objects.
4. Keep fixed-IOVA coherent allocation and optional page/SG convenience
   wrappers as follow-on work.

This removes the need to override `dma_ops` for page-backed VFMIG objects,
keeps the device on the normal DMA-IOMMU path, and makes restored IOVA
ownership explicit in the DMA layer.

---

# PART II: Decisions & implementation shape (2026-09-01)

> This appendix records the decisions taken while turning the proposal
> above into an implementable plan. Where it differs from Part I, Part II
> wins. Part I remains the API rationale; Part II is the build plan.

## 10. What today's model is, and what this replaces

The shipping model (`vfmig_iova.*` + `vfmig_dma_ops.c`) attaches an
`IOMMU_DOMAIN_UNMANAGED` to each tracked VF, which **displaces** the
dma-iommu managed default domain. The instant that happens the VF has no
working DMA API, so the driver must re-implement the entire DMA API on
top of its own domain: the slot allocator stands in for
`dma_alloc_coherent`, the `dma_ops` shim stands in for streaming /
coherent / sg, and a transient arena stands in for cmd mailboxes.

The `dma_iova_alloc_fixed()` model inverts this: **leave dma-iommu in charge
of the VF's domain and only fence off a fixed sub-range.** dma-iommu
still services every non-migratable mapping (streaming RX/TX, cmd
mailboxes, non-tracked coherent) with arbitrary IOVAs *outside* the
arena -- which is fine, because those IOVAs never appear in a SAVE blob.
The driver special-cases only the objects whose IOVA is baked into the
firmware image.

### Component mapping

| Current (unmanaged-domain override) | Proposed (managed domain + reserved arena) |
|---|---|
| unmanaged `iommu_domain` attached, displacing dma-iommu | **gone** -- keep the managed domain, never attach |
| `vfmig_iova_domain_create` / attach / detach lifecycle | `dma_iova_alloc_fixed(dev, &arena, BASE, SIZE)` / `dma_iova_free_fixed` |
| per-slot sub-windows + bump cursors | **survives** as *offsets within the arena*; VFMIG owns placement, dma-iommu owns the arena |
| `vfmig_iova_alloc_slot` (alloc pages + own `iommu_map`) | alloc lowmem pages + `dma_iova_link(arena.addr + slot_off, ..., DMA_BIDIRECTIONAL, IOMMU_CACHE)` |
| `dma_ops.map_sg` -> `user_page_map_phys` | user pages: `dma_iova_link` the umem sg at the object's saved offset |
| `dma_ops.map_phys` (mlx5e streaming RX/TX) | **deleted** -- ordinary `dma_map_page` on the managed domain |
| `dma_ops.alloc/free` (kcoherent arena) | **deleted** -- ordinary `dma_alloc_coherent` (proven dead on a tracked VF) |
| transient arena (cmd mailboxes) | **deleted** -- stock DMA API, outside the arena (see Part I S9.4) |
| kcoherent carve at bottom of USER_PAGE | **gone** -- non-migrated buffers use the managed domain |
| `replay_page` / `replay_external` / `bind_user_object` | **survives** as SAVE/LOAD placement, re-expressed as `dma_iova_link` at saved offsets |

Net effect: the entire "we are the DMA API" machinery collapses (all of
`vfmig_dma_ops.c`, the transient arena, the domain attach/detach). The
slot/offset **bookkeeping** carries forward, because the determinism
contract is still "same objects, same sizes, same offsets, same base."

## 11. Decision: always-FIXED at a hard-coded base

Both sides always reserve the *same* driver-chosen base with
`dma_iova_alloc_fixed()`; there is no arbitrary-source / FIXED-destination
asymmetry. Rationale: SAVE blobs become base-stable and the "was the
source's arbitrary base reproducible on the destination?" question
disappears.

Because each VF is a distinct `struct device` with its **own** managed
domain and its own `iovad`, every VF reserves the *same* base
independently -- no per-VF offset is needed, and a blob becomes
VF-index-independent (source VF 3 restores onto destination VF 7).

```c
/* per-VF managed domain: 4 GiB base clears the SW-MSI cookie, is
 * visually distinct from stock allocations, and sits well inside a
 * 39-bit VT-d aperture (512 GiB). */
#define VFMIG_ARENA_IOVA_BASE  0x100000000ULL
#define VFMIG_ARENA_IOVA_SIZE  ((u64)CONFIG_MLX5_VFMIG_IOVA_PER_VF_GIB << 30)
```

The reserve is **gated on grabbing the exact requested range**: if
`dma_iova_alloc_fixed()` returns `-EBUSY` (overlap with the msi_cookie,
an `iommu_dma_get_resv_regions()` window, or an already-allocated IOVA),
`SET_TRACKED` fails with a printed diagnostic rather than proceeding.
This makes reservation ordering a correctness requirement: **reserve the
arena before any other DMA traffic on that VF**, and on the destination
before the VF probes.

## 12. Decision: coherent-only device gate (a fundamental, not a narrowing)

The gate is:

```c
if (!use_dma_iommu(dev))       return -EOPNOTSUPP;
if (!dev_is_dma_coherent(dev)) return -EOPNOTSUPP;
```

This is **not** a new hardware restriction. User RDMA is already
coherent-dependent: the verbs user data path has no cache-management
hooks -- an MR's pages are pinned and DMA-mapped once by `ib_umem_get`,
after which the HCA and the CPU access them concurrently with no
`dma_sync`. RDMA correctness already rests on hardware cache coherence,
so VFMIG requiring `dev_is_dma_coherent` matches the envelope RDMA
already lives in. `dev_is_dma_coherent` is also *exactly* the predicate
that makes "alloc lowmem pages + `dma_iova_link(BIDIRECTIONAL,
IOMMU_CACHE)`" a valid substitute for `dma_alloc_coherent`: equivalent on
a coherent device; wrong on a non-coherent one (where
`dma_alloc_coherent` returns an uncached/remapped CPU mapping a link
cannot reproduce).

### Fundamental limitation, stated plainly

VFMIG-over-`dma_iova_link` supports only `dev_is_dma_coherent` devices --
the same set user RDMA MRs already require. Non-coherent support is out
of scope for the initial model.

### If non-coherent ever matters (documented escape hatches)

1. **Fixed-IOVA coherent helper** (Part I S7): a new dma-iommu call doing
   page alloc + `arch_dma_prep_coherent` + CPU remap + IOMMU map at a
   caller-specified IOVA, with a paired free that removes only the object
   (not the arena). Heaviest; the "real" API maintainers may require.
2. **Alias / double-map**: let `dma_alloc_coherent` allocate normally at
   its arbitrary IOVA (preserving all arch coherent correctness), then
   install a *second* IOMMU PTE at the deterministic arena IOVA pointing
   at the same physical pages. Firmware sees the arena IOVA; the CPU
   coherent contract is satisfied by the original allocation. Costs a
   duplicate PTE + a wasted natural IOVA but re-uses the stock coherent
   allocator and can extend to non-coherent. Fallback of record.

Initial plan ships coherent-only; both escape hatches are follow-on.

## 13. Lifecycle simplification (the win from dropping the unmanaged domain)

Today `SET_TRACKED` forces the VF **unbound** solely because the
unmanaged domain leaves it with no usable DMA. With the managed domain
retained, a tracked VF has working DMA, so that constraint can relax:

- **Destination LOAD simplifies**: the driver binds and allocates its
  buffers *normally* into the reserved arena, potentially collapsing the
  replay-before-probe -> `reset_cursor` -> `arm_drift_detection` dance.
- **Source can stay live longer**: tracking is a passive reservation, not
  a DMA takeover, so quiescing can move to the final SAVE instead of a
  hard up-front unbind.
- **Teardown hazards vanish**: no unmanaged-domain attach/detach, no
  `remove_one()` detach ordering, no empty-group WARN (the
  `detach_dev_if_unbound` machinery goes away).
- **netdev/mlx5e can coexist** with tracking (streaming maps just work).

Caveat: migratable buffers still route to deterministic offsets and the
dataplane is still quiesced for the final SAVE; only the DMA-plumbing
reason for forced-unbind is removed. Exactly which lifecycle constraints
relax is TBD and must be settled per SAVE/LOAD step, not assumed.

## 14. Cutover strategy: incremental, harness-gated, then flip

The per-VF domain identity is **all-or-nothing** (a device has one DMA
domain and one `dma_ops`), so the two models cannot coexist per-slot at
runtime -- the flip is atomic per VF. Development is still staged:

1. **Core primitive (minimal, correct, not upstream-polished).** Add
   `alloc_iova_fixed()` (exact + collision-detecting, rcache-aware) in the
   iova layer and a thin `dma_iova_alloc_fixed()` / `dma_iova_free_fixed()`. Just
   enough to be driven by tests -- defer the full errno taxonomy, the S7
   coherent helper, and SG convenience wrappers. Gated behind kunit +
   the device harness (Section 15), NOT yet wired into VFMIG.
2. **Managed-arena path behind a Kconfig / module-param**, parallel to
   the shipping unmanaged path. Convert the slot allocator to place its
   buffers via `dma_iova_link` into the reserved arena; convert the
   umem/user path off `dma_ops.map_sg` to `dma_iova_link`. Each step is
   validated by the existing save_load / uobject harnesses on the new
   path before the next.
3. **Flip the default** once the new path passes the full harness suite,
   then delete `vfmig_dma_ops.c`, the transient arena, and the
   unmanaged-domain lifecycle.

Every step that adds an interface or refactor must land with a test that
exercises it (kunit or a criu_rdma harness), matching the repo's
"no dormant helper" discipline.

## 15. Validation plan

**kunit (fast, logical invariants of the allocator):**

1. `dma_iova_alloc_fixed()` succeeds; a second *overlapping* reserve
   returns `-EBUSY`.
2. After reserve, a burst of `dma_map_page` never returns an IOVA inside
   `[BASE, BASE+SIZE)` -- including under heavy rcache churn (see
   Section 17 for why the rcache cannot alias a fenced range).
3. `dma_iova_link(BASE+off)` => `iommu_iova_to_phys(BASE+off) == phys`;
   `unlink` + `sync` => `iova_to_phys == 0`.
4. `dma_iova_free_fixed` then re-`alloc_iova_fixed` the same base succeeds
   (no lingering reservation -- proves the free was non-caching).
5. **Flush check**: unlink+free, re-link the same offset to a *different*
   phys, confirm `iova_to_phys` reflects the new phys (catches a missed
   flush-queue drain / stale rcache translation).

**Device-level harness (real IOMMU, criu_rdma):** a kernel test-hook verb
set -- `arena_reserve` / `arena_link` / `arena_probe` -- mirroring the
`pasid/` harness pattern, driving a real VF with sentinel-page
discrimination to confirm the firmware actually dereferences the arena
IOVA and that stale translations do not survive teardown.

## 16. Minimal end-to-end spike (what proves the model)

The cheapest thing that answers "does the arena work end to end" is:
reserve the arena at `SET_TRACKED`, route the **cmd ring** (one page,
process context, the simplest tracked buffer) through `dma_iova_link`
into the arena on both source and destination, and confirm a
SAVE/LOAD round-trips its IOVA. If that survives on real hardware, the
coherent bet and the reserve/link/flush mechanics are proven; the
remaining slots are mechanical conversions.

## 17. Empirical finding: is fixed-address reservation practical? Yes.

The core risk was "can we actually secure an exact IOVA range from the
allocator, given the per-CPU rcache?" Reading `drivers/iommu/iova.c`
(7.2) settles it, and a KUnit
(`tools/testing/criu_rdma/kunit/iova_kunit.c`, built as an out-of-tree
module so `drivers/iommu` carries no Kconfig/Makefile test wiring) proves it:

* `free_iova_fast()` -> `iova_rcache_insert()` -> `iova_magazine_push()`
  only stores the freed **pfn** in a magazine. It does **not** remove the
  `iova` node from the rbtree.
* `iova_rcache_get()` -> `iova_magazine_pop()` returns that pfn; nothing
  is re-inserted because the node was never removed.
* An rbtree node is erased only when a magazine is **drained**
  (`iova_magazine_free_pfns()` -> `remove_iova()`): depot trim,
  CPU-hotplug, or `put_iova_domain()`.

Therefore a cached pfn **always still occupies its rbtree node**. The
rbtree is the single authoritative occupancy map and already accounts for
cached IOVAs, so:

* `alloc_iova()` (slow path) walks the tree and steps over every occupied
  node, cached or not.
* `iova_rcache_get()` can only ever return pfns that have a live rbtree
  node -- it can never hand out an address that is not already
  tree-occupied.

**Consequence:** a fixed reservation on a genuinely-free range is airtight
against both the slow and fast paths with **no rcache flush required**.
The rcache is not an independent landmine.

The real gap is narrower: `reserve_iova()` is not an exact allocator. On
any overlap it silently *adjusts/merges* (`__adjust_overlap_range()`) and
may return a pre-existing node, so a caller cannot tell it got exactly
`[base, base+size)` nor independently own/free it. So `alloc_iova_fixed()`
reduces to a small, clean primitive:

> Under `iova_rbtree_lock`, scan `[pfn_lo, pfn_hi]` for any overlapping
> node; if none, insert an exact node and return 0; otherwise return
> `-EBUSY`. No rcache interaction; a natural sibling of `reserve_iova()`
> and `alloc_iova()`.

The one real requirement is atomicity: the overlap scan and the insert
must hold `iova_rbtree_lock` together so a concurrent `alloc_iova()`
cannot claim part of the range between check and insert (exactly how
`reserve_iova()` already brackets its work).

### KUnit status

`iova_kunit.c` ships four live characterization cases that run today and
prove the above:

1. `reserve_fences_slow_path` -- reserved window is skipped by `alloc_iova`.
2. `cached_iova_retains_rbtree_node` -- `find_iova()` still resolves a
   fast-freed pfn (the crux invariant).
3. `reserve_fences_fast_path` -- heavy rcache churn never yields an IOVA
   inside the reserved window.
4. `reserve_merges_over_existing` -- documents that `reserve_iova()`
   returns the merged existing node (why we need `alloc_iova_fixed()`).

It also carries a dormant contract suite (guarded by
`IOVA_KUNIT_TEST_ALLOC_FIXED`) specifying `alloc_iova_fixed()`:

* `fixed_exact_on_free_range` -- exact node inserted on a free range.
* `fixed_ebusy_on_overlap` -- any overlap (a subset) returns `-EBUSY`.
* `fixed_ebusy_on_cached_overlap` -- overlap with an rcache-live node
  returns `-EBUSY` (the rbtree-is-authoritative case).
* `fixed_free_roundtrip` -- freeing the exact node re-permits the range.
* `fixed_fences_allocators` -- once owned, neither `alloc_iova` nor
  `alloc_iova_fast` ever hands out any of the range (the positive
  fencing analogue of cases 1/3).
* `fixed_free_releases_to_allocator` -- after free the range is genuinely
  released, i.e. an ordinary allocation may reoccupy it (free is the
  non-caching `__free_iova`, not a mere not-busy flag).

That suite activates in the same series that lands the primitive -- the
test is written first, by design. Note there is deliberately **no**
"`reserve_iova` fails on our range" case: `reserve_iova` cannot fail (it
merges on overlap), so the meaningful negative is `-EBUSY` from
`alloc_iova_fixed` plus the allocator-fencing cases above.

## 18. Why not build directly on `reserve_iova()`?

This is the natural "why a new primitive instead of the one that already
takes a `(pfn_lo, pfn_hi)`?" question. The answer starts with a reframe
that dissolves most of the confusion.

### 18.1 `iova.c` is an address-space bookkeeper, not a memory allocator

Nothing in `iova.c` -- not even `alloc_iova` -- allocates memory or
installs a page-table mapping. The layer manages exactly one thing: which
IOVA numbers (pfns) are currently in use, tracked in one rbtree. Physical
pages and the real `iommu_map(iova -> phys)` translation happen one layer
up, in `dma-iommu.c`. So "`reserve_iova` doesn't allocate anything" is
true -- but so is `alloc_iova`, in the memory sense. There are three
distinct *verbs* over the same rbtree:

| verb | what it claims | fails on conflict? | who frees |
|---|---|---|---|
| `alloc_iova` | **any** free range | n/a (finds a gap) | caller, dynamically |
| `reserve_iova` | a **specific** range, permanently | **no** -- merges/adjusts | never (domain lifetime) |
| `alloc_iova_fixed` (new) | a **specific** range, owned | **yes** -- `-EBUSY` | caller, dynamically |

A `malloc` analogy: `iova_domain` is an arena where you never get *memory*
back, only an *address* the device will DMA to. `alloc_iova` is
`malloc(size)` (any free address); `reserve_iova` is "pretend `[x,y]` was
already `malloc`'d, forever" (so `malloc` never returns it);
`alloc_iova_fixed` is "`malloc` at exactly `x`, or fail."

### 18.2 What `reserve_iova` is actually for

Its two real callers both run at domain init and both fence the dynamic
allocator off address space the platform already owns:

* `iova_reserve_pci_windows()` -- PCI host-bridge MEM windows and
  dma-ranges, so an allocated IOVA never collides with P2P / non-xlated
  bridge addresses.
* `iova_reserve_iommu_regions()` -- IOMMU resv regions: RMRRs,
  direct-mapped regions, and the hardware MSI doorbell (`IOMMU_RESV_MSI`).
  (`IOMMU_RESV_SW_MSI` is skipped -- dma-iommu manages that doorbell
  dynamically via the msi_page path.)

These regions are **permanent domain properties**, may legitimately abut
or overlap (two adjacent RMRRs, an MSI window touching a PCI window), and
are never individually freed. That is exactly why `reserve_iova` *merges*
on overlap (`__adjust_overlap_range()` grows an existing node to swallow
the request) and *never fails*: coalescing permanent exclusions into
fewer nodes is correct for its job.

### 18.3 Could VFMIG build on it anyway? Almost -- and where it breaks

In the **no-overlap** case `reserve_iova` already gives us everything: it
hits `__insert_new_range`, inserts an exact node `[pfn_lo, pfn_hi]`,
returns it, and we could later `__free_iova` it cleanly. If we could
*guarantee* no overlap, `reserve_iova` + `__free_iova` would suffice.

The whole problem is that guarantee, and it is not merely "we don't learn
about a conflict." It is what silently happens **when there is one**:

1. On overlap, `reserve_iova` grows a *pre-existing* node to include our
   range and returns **that** node. If our arena base ever collides with a
   platform reservation (MSI doorbell, RMRR, PCI window), our range is
   merged *into the MSI node* and we are handed the MSI node back.
2. We now believe we "own" a node that is actually the platform's shared
   reservation. When we `__free_iova` it at untrack/teardown, we remove
   the **entire merged node** -- un-reserving the MSI doorbell IOVA.
   `alloc_iova` is then free to hand that address to a streaming map:
   interrupt/DMA corruption, latent and silent.
3. The merge is **undetectable from the return value**: `reserve_iova`
   returns a node either way with no "did I merge?" flag, and a
   coincidental exact pre-existing node is indistinguishable from a fresh
   insert. Detection cannot be bolted on afterward; it has to happen
   *inside*, during the scan.

A DIY "scan for overlap myself, then call `reserve_iova`" avoids (3) but
is a check-then-act across two separate `iova_rbtree_lock` acquisitions --
racy unless the check and insert hold the lock **together**. In VFMIG's
narrow "reserve at `SET_TRACKED`, before any VF DMA, single per-VF domain"
ordering it would probably work, but it is fragile (breaks the day
anything maps before we reserve) and it re-implements the primitive minus
its atomicity, paying `reserve_iova`'s O(n) full-tree walk twice.

### 18.4 Conclusion

The missing piece is not just "return `-EBUSY`"; it is that the
silent-merge behavior **actively corrupts co-owned reservations on free**
and the merge is invisible to the caller. Detection is a safety
requirement, not a convenience. `alloc_iova_fixed` is therefore *literally
`reserve_iova`'s scan-and-insert* with two changes: (a) return `-EBUSY` on
any overlap instead of merging, and (b) hold `iova_rbtree_lock` across the
check and the insert. Same layer, same locking discipline, an
independently-freeable node -- a strictly safer sibling, not a new
subsystem.

## 19. Decision: the paired free is non-caching (`dma_iova_free_fixed`)

The reserve has a matching free at both layers, and it must **not** be the
ordinary caching free.

**iova layer:** pair `alloc_iova_fixed()` with the existing
`free_iova(iovad, pfn)`. That is already the non-caching primitive -- it does
`find + remove_iova` under `iova_rbtree_lock` and drops the node
immediately. Its caching sibling `free_iova_fast()` instead pushes the pfn
into a per-CPU rcache magazine and leaves the rbtree node in place until a
later drain (S17). No new iova-layer symbol is needed: `free_iova()` *is* the
fixed free.

**DMA layer:** add a dedicated `dma_iova_free_fixed()` rather than reuse
`dma_iova_free()`. Stock `dma_iova_free()` routes through
`iommu_dma_free_iova()` -> `free_iova_fast()` -- the caching path. The fixed
free must bypass the rcache unconditionally:

```c
void dma_iova_free_fixed(struct device *dev, struct dma_iova_state *state)
{
	struct iommu_domain *domain = iommu_get_dma_domain(dev);
	struct iova_domain *iovad = &domain->iova_cookie->iovad;

	free_iova(iovad, iova_pfn(iovad, state->addr));   /* non-caching */
}
```

Why not just reuse `dma_iova_free()` and lean on the fact that a multi-GiB
arena exceeds the rcache size cap and therefore falls through
`free_iova_fast()` to `free_iova()` anyway?

* **It is a silent size coincidence, not a contract.** `iova_rcache_insert()`
  rejects only sizes with `order_base_2(size) >= IOVA_RANGE_CACHE_MAX_SIZE`,
  so today's GiB arena never caches -- but a small arena (a kunit, a shrunk
  `CONFIG_MLX5_VFMIG_IOVA_PER_VF_GIB`, a future per-object fixed reservation)
  would silently take the caching path and the bug below would appear.
* **A caching free is actively wrong for us.** The freed base pfn would sit
  in a magazine with its rbtree node still present, so an immediate
  re-`alloc_iova_fixed()` at the same base sees the lingering node and
  returns `-EBUSY` -- breaking destination re-LOAD / re-track and the
  `fixed_free_roundtrip` kunit invariant. It would also leak our arena pfn
  into the general streaming rcache pool.

The fixed free also needs **no size argument**: `free_iova()` removes the
whole node found at the base pfn, and the base is granule-aligned so there is
no `iova_start_pad` to unwind (unlike `dma_iova_free()`, which recomputes it).

**Teardown ordering is a contract.** `dma_iova_free_fixed()` releases only
the IOVA *address range* (the rbtree node); it tears down no page tables.
Every object must be `dma_iova_unlink()`-ed (and synced) first, or live PTEs
would point into a range that is now free for reallocation -- a double-map
hazard. Order is fixed: unlink every object -> free the arena.
