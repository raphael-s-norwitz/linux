# Restored VFs carry their source's state forward, and it grows

## Summary

Every SAVE from a VF that was itself restored produces a firmware image
about 850 KB (about 210 pages) larger than the one it was loaded from.
The growth is linear and never shrinks. After eight generations
(VF 0 -> VF 1 -> VF 0 -> ...), the ninth restored probe fails in
CREATE_EQ, and the VF does not come up.

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

## What we think is happening (unconfirmed)

The restored probe skips function-level setup (ENABLE_HCA, SET_ISSI,
boot pages, INIT_HCA), but `mlx5_load()` then creates the driver's
kernel objects as usual: EQs, CQs, QPs and so on. The source driver's
objects came across in the image and are still live in firmware. The
destination driver never learns about them, so it never destroys them.
Each generation therefore holds its predecessor's objects plus its own,
and the next SAVE includes both:

- Growth per generation is about one driver's worth of objects, and it
  is roughly constant.
- Eventually one of firmware's per-function limits runs out, and
  CREATE_EQ is the first command to hit it.

Two other observations probably have the same cause:

- **Teardown** (`teardown_reclaim_hang.md`): firmware never answers
  MANAGE_PAGES(TAKE) after TEARDOWN_HCA on a restored VF. Objects that
  firmware still holds, and that the driver never destroys, would
  explain that.
- **13 pages:** RECYCLE_VF finds 13 arena pages left after every
  restored-VF teardown, and none after a normal teardown. Every restored
  image also replays exactly 13 dma_coherent buffers, against 29 from a
  VF that ran normally. They may be the same buffers.

## Next steps to debug

1. **Check whether the objects are leaking.** Record the EQ, CQ and QP
   numbers the driver gets
   (`/sys/kernel/debug/mlx5/<vf-bdf>/{EQs,CQs,QPs}`) on a normal VF and
   on generations 1, 2 and 3. If the numbers climb with each
   generation, the source's objects are still allocated.
2. **Separate saving from chaining.** SAVE the same restored VF twice
   in a row, and a normal VF twice in a row. If the size is stable, the
   growth comes from loading, not from saving or uptime.
3. **Decode syndrome 0x223532**, with the firmware team or the PRM, to
   find out which limit or parameter CREATE_EQ rejects.
4. **Identify the 13 leftover arena entries.** Have RECYCLE_VF log the
   slot and length of each one, and compare them with the 13 replayed
   dma_coherent buffers.
5. **Decide what the destination does with the source's kernel
   objects.** It must either adopt them in place of creating new ones,
   or destroy them before `mlx5_load()` runs. This is the fix, and it is
   tracked separately from RECYCLE_VF and the vhca_id work. It is also
   the likely fix for the teardown problem, so settle it before changing
   any teardown code.

Reproduce with:

    sudo CYCLES=4 tools/testing/criu_rdma/save_load/test_vf_recycle.sh
