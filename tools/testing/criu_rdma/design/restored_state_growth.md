# Restored VFs carry their source's state forward, and it grows

## Summary

Every SAVE from a VF that was itself restored produces a firmware image
about 850 KB (about 210 pages) larger than the one it was loaded from.
The growth is linear and never shrinks. After eight generations
(VF 0 -> VF 1 -> VF 0 -> ...), the ninth restored probe fails in
CREATE_EQ, and the VF does not come up.

These are two problems, not one. The CREATE_EQ failure is fixed: each
restored probe created new EQs while the source's stayed live in
firmware, and the VF ran out of EQ numbers. Adopting the source's EQs,
described below, keeps the EQ numbers fixed, and chains now run at
least 12 generations. The image growth is unchanged by that fix. It
comes from mlx5e: every restored probe builds a new set of mlx5e
objects while the source's stay live. Removing the source's mlx5e
before SAVE keeps the image flat, and restored QPs keep working;
having CRIU do that is proposed below.

Saving from a VF that was not restored is not affected.

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
holds its predecessor's objects plus its own. Whether that is also what
grows the image is not established; for EQs it is not.

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
- **Image growth is not explained by the EQs.** It is about 205 pages
  per generation in the probe as well, but an EQ's buffer is host
  memory, carried as host-page records, so firmware's image holds only
  a small EQ context. Adopting the EQs left the growth unchanged (see
  Results).

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

### Results

With the three commits, on the VF 1 -> VF 0 -> VF 1 ... chain:

- EQ numbers stay at 0x4-0x6 for 12 generations. Every restored probe
  adopts the source's UAR and its cmd, async and completion EQs, with
  no fallback.
- The cmd EQ's consumer index carries forward, advancing by about 0xfa
  per generation.
- `test_vf_recycle.sh` cycles 4 and 5 pass all 10 rounds. Both failed
  in round 9 before.
- The image still grows by 830-920 KB per generation (4,669,440 ->
  14,061,568 bytes over 11 saves), the same as before.
- One CQ still leaks per generation. It is mlx5e's drop-RQ CQ (see
  below).
- Restored-VF teardown still logs Fatal error 3 and leaves 13 arena
  pages, as before (`teardown_reclaim_hang.md`).
- Every restored probe drops exactly 24 stale command EQEs. They are
  not dispatched, but a constant count looks systematic and may be
  what the ghost-EQE workaround covers.

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

## The image growth is mlx5e

Saving is stable: three SAVEs in a row of the same VF, normal or
restored, give byte-identical images, including one after 15 s idle. The
growth happens only across LOAD and the restored probe.

Running the chain with only some of the VF's auxiliary drivers bound
(auxiliary autoprobe off, drivers bound by hand) splits it by driver:

| Bound on top of `mlx5_core`       | Gen 0 image (bytes) | Growth per restored generation |
|-----------------------------------|---------------------|--------------------------------|
| nothing                           | 3,641,344           | +8 KB once, then 0             |
| `mlx5_ib`                         | 3,747,840           | +24 KB once, then 0            |
| `mlx5_core.eth` (mlx5e)           | 4,648,960           | +917 KB, +844 KB, +831 KB      |
| all (the usual setup)             | 4,669,440           | same as mlx5e alone            |
| all, netdev up                    | 4,812,800           | +1.11 MB, +987 KB, +938 KB, +1.0 MB |
| all, source's mlx5e unbound before each SAVE | 3,756,032 | +24 KB once, then 0       |

mlx5e creates its firmware objects at probe, even with the interface
down: flow tables, TIRs, an RQT, a TIS, a transport domain, a PD and the
drop RQ with its CQ, plus channels and more completion EQs once the
netdev is up. A restored probe creates all of them again while the
source's copies stay live, so each generation adds about one mlx5e's
worth. The restored `mlx5_ib` creates almost nothing, because GSI setup
is refused on restored VFs, which is why it does not grow.

Unbinding the source's `mlx5_core.eth` right before SAVE, and letting
the destination bind a fresh mlx5e, keeps the image flat for at least
four generations. The restored VF's RoCE port still reaches ACTIVE with
its two default GIDs, and `DESTROY_EQ` failures at teardown drop from
30 to 4 (next section).

### Adopted EQs that cannot be destroyed

Firmware refuses to destroy an EQ while CQs are still bound to it
(DESTROY_EQ: bad resource state, syndrome 0x30f433). An adopted EQ can
still have the source's leaked CQs on it, so restored-VF teardown fails
to destroy it:

- With mlx5e left bound on the source, the source's channel and drop
  CQs pin every completion EQ mlx5e used: 30 failures over four
  generations.
- With mlx5e unbound before SAVE, its CQs are gone and those EQs are
  destroyed cleanly. The 4 that remain are completion EQ 0x6 (vector
  0), once per restored teardown, pinned by generation 0's GSI and MAD
  CQs from `mlx5_ib`.

`destroy_unmap_eq()` frees the EQ buffer even when DESTROY_EQ fails, so
firmware keeps a live EQ whose pages are gone. The arena unmaps the
IOVAs, so a firmware write would fault rather than corrupt host memory.
The same was true of every source EQ ignored before adoption.

## Proposed: CRIU unbinds mlx5e before SAVE

Before the dump's SAVE, the CRIU plugin unbinds the source VF's
`mlx5_core.eth` auxiliary device. If the dump fails, or the source is
left running, it binds the device again. The destination binds a fresh
mlx5e as usual. Everything else that concerns the netdev belongs to the
orchestrator, which already assigns the VF's MAC and network setup on
restore:

| Step                                                      | Owner        |
|-----------------------------------------------------------|--------------|
| Unbind `mlx5_core.eth` before SAVE; rebind on rollback    | CRIU plugin  |
| VF admin MAC on the destination, same as the source's     | Orchestrator |
| IP addresses, namespace, bond or bridge placement, routes | Orchestrator |
| Telling peers (ARP, neighbours)                           | Orchestrator |

The netdev disappears whoever removes it, so doing it in the kernel
buys nothing for rollback. Userspace is not slower, and it avoids
defining a PF-to-VF lock order inside SAVE. If the kernel should
enforce it, a SAVE flag or a QUERY_VF report of a bound mlx5e is
enough. Reloading mlx5e on RESUME instead would not help: the image
would still carry the source's mlx5e objects, and work on SAVE is the
cost that matters to the source.

Carrying a journal of the source's kernel objects so the destination
could destroy them was rejected as far more work.

### Validation

A self-loop RC QP gate did the CRIU step by hand. The QP is connected
through the source's RoCEv2 GID for an IPv4 address, and sends itself a
zero-length message before the dump. The orchestrator's part was done by
setting the same admin MAC on both VFs through the PF, and adding the
same IP on the destination before `criu restore`. It ran once with mlx5e
left bound and once unbound before SAVE, VF 0 -> VF 1 and VF 0 -> VF 0.
All four passed:

- Unbinding mlx5e removes the netdev, takes port 1 DOWN and deletes all
  four GIDs, and the QP stays in RTS.
- On the destination, the default GIDs and the IP's GIDs come back at
  the same indexes, so the restored QP's GID index still names its GID.
- After restore the QP is still RTS and completes another send to
  itself. No earlier gate sent traffic on a restored QP.
- The saved firmware state drops from 4,866,048 to 3,858,432 bytes, and
  LOAD replays no mlx5e buffers (`frag_buf` 0 instead of 585).

The MAC matters: the QP resolved its destination MAC at RTR, and that
stays in its firmware context, so the destination VF must come up with
the source's MAC.

Not covered: more than one address, IPv6 or VLANs, where the
destination could add GIDs in a different order (CRIU could record the
source's GID table and check the destination's before restoring QPs);
a remote peer; and whether the RoCE RX fence still blocks traffic
during SAVE. With mlx5e unbound, the fence goes into flow table 0x1,
group 0x0, instead of table 0x4, group 0x16.

## Open questions

- **GID order with several addresses.** One IPv4 address keeps its GID
  index (see Validation). Several addresses, IPv6 or VLANs may not.
- **RoCE RX fence without mlx5e.** The fence lands in a different flow
  table when mlx5e is unbound; confirm it still drops incoming RoCE
  traffic during SAVE.
- **kmod warning during SAVE.** Every dump logs three
  `kmod_dup_request_exists_wait` warnings from `criu`, with or without
  mlx5e bound, which taints the kernel.
- **GSI and MAD CQs from generation 0** pin completion EQ 0x6. They
  cannot be removed on the source without unregistering the IB device.
  The destination would need to destroy them, which needs their IDs.
- **EQ buffer freed after a failed DESTROY_EQ.** Keep an adopted EQ's
  pages mapped until RECYCLE_VF when firmware refuses to destroy it.
- **RDMA CM on restored VFs.** GSI setup is refused on restored VFs, so
  port 1 has no MAD agent and RDMA CM, which uses GSI QP1 on RoCE too,
  cannot work there.
- **Restored CQ's EQ binding.** `mlx5_ib_restore_cq()` should check the
  restored CQ's `c_eqn` against the adopted EQ and refuse on mismatch.
- **24 stale command EQEs** dropped on every restored probe. The count
  is constant, so it is probably systematic, and it may be what the
  ghost-EQE workaround (`vfmig_load_skip_cmd_use_events`) covers. With
  the cmd EQ adopted, that workaround may no longer be needed.
- **Teardown.** Restored-VF teardown still logs Fatal error 3 and
  leaves 13 arena pages, with or without mlx5e on the source
  (`teardown_reclaim_hang.md`).
- **FW_PAGE cursor.** After the cursor reset at LOAD, the FW_PAGE slot's
  cursor can hand out imported replayed firmware pages again, since the
  import does not go through `alloc_slot()`.
- **Syndrome 0x223532.** Confirm with the firmware team that CREATE_EQ
  was rejecting an exhausted EQ number.

## Fewer firmware round trips on the restore path

The restore path issues more firmware commands than it needs to. Each
command is a round trip through the command interface, and a restored
VF also runs that interface in polling mode. Known sources:

- **Adoption queries.** One QUERY_EQ per adopted EQ. Up-front adoption
  queries every tagged completion vector, which with the netdev up is
  many. Restored CQs do not need it: `mlx5_comp_eqn_get()` adopts a
  vector's EQ on first use anyway. Up-front adoption only ensures
  unused source EQs are destroyed at teardown; doing it lazily, and
  destroying the leftovers in one pass at teardown, would trade the
  queries for destroys of EQs nobody used.
- **Static EQ fields could come from the image** if firmware confirms
  the consumer-index question above.
- **mlx5e is built twice per hop.** Torn down on the source at SAVE and
  created from scratch on the destination. Removing it before SAVE
  keeps the image flat, but does not reduce the commands.
- **Polling mode.** With the cmd EQ adopted, the restored VF could
  switch the command interface to events, if the stale-EQE question is
  settled.

The first step is a per-opcode count of the commands a restored probe
issues, compared with a normal probe, to see which of these dominate.

Reproduce with:

    sudo CYCLES=4 tools/testing/criu_rdma/save_load/test_vf_recycle.sh
