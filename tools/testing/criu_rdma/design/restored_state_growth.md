# Restored VFs carry their source's state forward, and it grows

## Summary

Every SAVE from a VF that was itself restored produces a firmware image
about 850 KB (about 210 pages) larger than the one it was loaded from.
The growth is linear and never shrinks. After eight generations
(VF 0 -> VF 1 -> VF 0 -> ...), the ninth restored probe fails in
CREATE_EQ, and the VF does not come up. The cause is confirmed: each
restored probe creates new kernel objects while the source's stay live
in firmware, and the VF runs out of EQ numbers. The fix, adopting the
source's EQs, is described below.

Saving from a VF that was not restored is not affected. The immediate
goal, moving a workload back and forth a couple of times, works today;
a long chain of migrations does not.

## Observation

`save_load/test_vf_recycle.sh`, cycle 4. Each round saves from the VF
restored in the previous round and loads into the other VF. Sizes are
the `vfmig: saved vf N (vhca_id ...) state: <bytes>` line, which prints
the image size firmware returns from SAVE_VHCA_STATE. Host-page records
are not included in it.

| round | saved from               | FW image (bytes) | growth  | host records replayed |
|-------|--------------------------|------------------|---------|-----------------------|
| 1     | VF that ran normally     | 4,648,960        |         | 201 (29 dma_coherent) |
| 2     | restored, generation 1   | 5,578,752        | 929,792 | 185 (13 dma_coherent) |
| 3     | generation 2             | 6,418,432        | 839,680 | 185                   |
| 4     | generation 3             | 7,249,920        | 831,488 | 185                   |
| 5     | generation 4             | 8,110,080        | 860,160 | 185                   |
| 6     | generation 5             | 8,966,144        | 856,064 | 185                   |
| 7     | generation 6             | 9,818,112        | 851,968 | 185                   |
| 8     | generation 7             | 10,657,792       | 839,680 | 185                   |
| 9     | generation 8             | 11,522,048       | 864,256 | 185                   |

Cycle 5 does the same with a UUID handoff each round and failed in the
same round. The UUID path plays no part in this.

What grows is firmware's share of the image. The number of host DMA
buffers replayed at LOAD stays at 185 in every generation.

## Failure

Round 9, during the restored probe of VF 1, right after LOAD was
applied:

    CREATE_EQ(0x301) op_mod(0x0) failed, status bad parameter(0x3), syndrome (0x223532), err(-22)
    setup_async_eq:616: failed to create async EQ -22
    mlx5_init_one failed with error code -22

Every earlier round in both cycles took the restored path, brought up
its ib device and stayed healthy.

## What is happening

The restored probe skips function-level setup (ENABLE_HCA, SET_ISSI,
boot pages, INIT_HCA), but `mlx5_load()` then creates the driver's
kernel objects as usual. The source driver's objects came across in the
image and are still live in firmware. The destination driver never
learns about them, so it never destroys them. Each generation therefore
holds its predecessor's objects plus its own, and the next SAVE
includes both.

This is confirmed for EQs by a probe that chains VF 1 -> VF 0 -> VF 1
... and records `/sys/kernel/debug/mlx5/<vf-bdf>/{EQs,CQs,QPs}` plus the
saved image size at each generation:

- **EQs leak, three per generation.** The driver creates a cmd EQ
  (0x100 entries), an async EQ (0x2000) and completion EQ 0 (0x800).
  Their numbers climb by three each generation: 0x4-0x6, 0x7-0x9,
  0xa-0xc, 0xd-0xf, 0x10-0x12. At generation 9 the cmd EQ gets 0x1f
  and the async EQ would need 0x20, which matches the CREATE_EQ
  failure above. Syndrome 0x223532 is still not decoded, but the
  timing fits running out of EQ numbers.
- **The leaked EQs share buffers with the new ones.** The replayed EQ
  buffer area stays at 164 pages (10,496 entries of 64 bytes, exactly
  one generation's three EQs) in every generation. The destination
  reclaims those pages by allocation order and creates its EQs on
  them, so firmware may write events for a leaked EQ into a buffer the
  destination reads as one of its own. This probably explains the
  "ghost EQE on cmd_eq slot 0" that `vfmig_load_skip_cmd_use_events`
  works around.
- **CQs leak one per generation; QPs do not leak.**
- **Image growth matches.** About 205 pages per generation in the probe
  (4,669,440 -> 5,603,328 -> 6,443,008 -> 7,274,496 bytes).

### Restored CQs end up on the wrong EQ

Every CQ names one completion EQ in firmware (`c_eqn`), fixed at
CREATE_CQ; there is no command to move it. `mlx5_ib_restore_cq()` keeps
the source's cqn but registers it on the destination's newly created
completion EQ from `mlx5_comp_eqn_get()`. Firmware keeps delivering its
completions to the source's EQ, which nothing re-arms, so a restored CQ
most likely stops raising interrupts after the first event
(unconfirmed). QPs have no direct EQ binding: completions go through
their CQs, and QP events arrive on the async EQ and are looked up by
qpn.

Two other observations probably have the same cause:

- **Teardown** (`teardown_reclaim_hang.md`): firmware never answers
  MANAGE_PAGES(TAKE) after TEARDOWN_HCA on a restored VF. Objects that
  firmware still holds, and that the driver never destroys, would
  explain that.
- **13 pages:** RECYCLE_VF finds 13 arena pages left after every
  restored-VF teardown, and none after a normal teardown. Every restored
  image also replays exactly 13 dma_coherent buffers, against 29 from a
  VF that ran normally. They may be the same buffers.

## Fix: adopt the source's EQs

Destroying the source's EQs before `mlx5_load()` would also stop the
leak, but restored CQs point at them, so they must be adopted instead.
The series:

1. **Tag EQ buffers.** After CREATE_EQ on a tracked VF, the arena entry
   holding the EQ's first page gets the key `VFMIG_EQ_KEY(role, vector,
   eqn)`. The key already travels in the HOST_PAGE record, so a
   restored probe can find the source's EQs without a wire format
   change.
2. **Take over the source's UAR.** An EQ rings its doorbell through the
   UAR named in its context, and that cannot be changed. On a restored
   VF, `mlx5_load()` reads the source cmd EQ's `uar_page` and allocates
   the device bfreg on that UAR instead of issuing ALLOC_UAR. It falls
   back to a fresh UAR when there is no tagged EQ or the takeover
   fails.
3. **Adopt the EQs.** `create_map_eq()` takes over the tagged source EQ
   for its role and vector instead of creating one. It checks the
   source EQ's context, claims the replayed buffer pages without
   zeroing them, resumes at firmware's consumer counter and skips
   CREATE_EQ. Stale command completions are dropped, since they name
   the source's command slots. Tagged completion EQs are adopted up
   front in `mlx5_eq_table_create()`, so `mlx5_comp_eqn_get()` returns
   the EQ that restored CQs already point at. Any mismatch falls back
   to creating a fresh EQ. A new `claimed` flag on arena entries keeps
   the allocation cursor from handing adopted pages out again.

Expected result, not yet verified on hardware: EQ numbers stay at
0x4-0x6 across generations, the chain gets past generation 9, and image
growth drops by the EQ share.
CQ leaks, the other recreated objects, and the ghost-EQE workaround are
handled afterwards.

### Why adoption queries firmware for each EQ

Adoption issues one QUERY_EQ per EQ, three per restored probe; the cmd
EQ's result from step 2 is reused in step 3. Most of the EQ context
could travel in the image instead:

| Field                                              | Changes after LOAD?                         |
|----------------------------------------------------|---------------------------------------------|
| `uar_page`, `log_eq_size`, `intr`, `log_page_size` | No; set at CREATE_EQ and never modified     |
| `producer_counter`                                 | Yes, but not needed; owner bits suffice     |
| `status`, `st`                                     | Yes; an EQ can overflow before the probe    |
| `consumer_counter`                                 | No; only our doorbell writes move it        |

The query stays because:

- **`consumer_counter` has to be exact.** Adoption must resume where
  firmware thinks the consumer is. Firmware captures it atomically with
  the rest of the EQ state. A driver-side copy taken during SAVE races
  the source's EQ handler, which is still running, and the EQ buffer
  only recovers it modulo twice the EQ size.
- **`status` can change after SAVE.** An EQ that overflowed between LOAD
  and the probe must not be adopted.
- **The image is not authenticated.** HOST_PAGE records pass through
  userspace and CRIU's storage, protected only by a CRC. Trusting a
  stale record would arm a doorbell for a nonexistent EQ and leave the
  VF with a dead command EQ, instead of falling back cleanly.

If firmware confirms that it accepts a consumer index that is right
only modulo twice the EQ size, the async and completion EQs could be
adopted without a query, using static fields from the arena keys.

## Next steps to debug

1. **Separate saving from chaining.** SAVE the same restored VF twice
   in a row, and a normal VF twice in a row. If the size is stable, the
   growth comes from loading, not from saving or uptime.
2. **Decode syndrome 0x223532**, with the firmware team or the PRM, to
   confirm that CREATE_EQ is rejecting an exhausted EQ number.
3. **Identify the 13 leftover arena entries.** Have RECYCLE_VF log the
   slot and length of each one, and compare them with the 13 replayed
   dma_coherent buffers.
4. **Extend adoption past EQs.** Find the CQ that leaks each generation
   and adopt or destroy it, then check the remaining kernel objects.
   This is also the likely fix for the teardown problem, so settle it
   before changing any teardown code.

Reproduce with:

    sudo CYCLES=4 tools/testing/criu_rdma/save_load/test_vf_recycle.sh
