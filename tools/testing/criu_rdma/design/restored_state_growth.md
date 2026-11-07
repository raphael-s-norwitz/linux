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
objects while the source's stay live. Suspending the source's mlx5e
before SAVE keeps the image flat, and restored QPs keep working. The
RoCE RX fence now does that (`MLX5_VFMIG_RX_FENCE_F_ETH_SUSPEND`), and
the source's QPs keep working too if it is left running; see "Fix:
suspend mlx5e under the RX fence" below.

With mlx5e suspended, two smaller leaks of the same kind remained, and
both are fixed. A restored ucontext allocated a new transport domain
and orphaned the source's, about 9 KB per hop; it now adopts the
source's. And mlx5_ib's UMR QP and CQ, created on the first MR
operation, came across with no owner, about 24 KB per hop; the RX fence
now releases them (`MLX5_VFMIG_RX_FENCE_F_IB_RELEASE`). See "The
remaining growth: transport domain and UMR" below.

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
| all, source's mlx5e suspended before each SAVE | 3,756,032 | +24 KB once, then 0     |
| all, suspended by the RX fence flag (fence carried) | 3,805,184 | +24 KB once, then 0 |
| all, source's netdev set down before each SAVE | 4,673,536 | +918 KB, +885 KB, +860 KB, +860 KB |

mlx5e creates its firmware objects at probe, even with the interface
down: flow tables, TIRs, an RQT, a TIS, a transport domain, a PD and the
drop RQ with its CQ, plus channels and more completion EQs once the
netdev is up. A restored probe creates all of them again while the
source's copies stay live, so each generation adds about one mlx5e's
worth. The restored `mlx5_ib` creates almost nothing, because GSI setup
is refused on restored VFs, which is why it does not grow.

Unbinding or suspending the source's mlx5e right before SAVE, and
letting the destination bind a fresh mlx5e, keeps the image flat for at
least four generations. The restored VF's RoCE port still reaches
ACTIVE with its two default GIDs, and `DESTROY_EQ` failures at teardown
drop from 30 to 4 (next section). Suspend means the auxiliary driver's
`suspend` callback, as `mlx5_detach_device()` calls it: mlx5e detaches
the netdev and destroys its mdev resources (PD, transport domain, mkey,
TISes, bfregs), but the netdev stays registered.

Setting the netdev down is not enough. It closes the channels, but the
flow tables, TIRs, RQT, TIS, PD and transport domain stay, and the image
still grows by about 860 KB per generation. No userspace operation
reaches mlx5e's `suspend` alone: the auxiliary bus has no PM hooks, and
mlx5_core calls it only from whole-device detach (PCI sleep, health
recovery, devlink reload, firmware reset), which also removes mlx5_ib
and its QPs.

The table's fence-flag row is 49 KB larger than the plain suspend
because that probe also raises the RX fence before each SAVE, and the
image carries its table. It is still flat.

### Adopted EQs that cannot be destroyed

Firmware refuses to destroy an EQ while CQs are still bound to it
(DESTROY_EQ: bad resource state, syndrome 0x30f433). An adopted EQ can
still have the source's leaked CQs on it, so restored-VF teardown fails
to destroy it:

- With mlx5e left bound on the source, the source's channel and drop
  CQs pin every completion EQ mlx5e used: 30 failures over four
  generations.
- With mlx5e unbound or suspended before SAVE, its CQs are gone and those EQs are
  destroyed cleanly. The 4 that remain are completion EQ 0x6 (vector
  0), once per restored teardown, pinned by generation 0's GSI and MAD
  CQs from `mlx5_ib`.

`destroy_unmap_eq()` frees the EQ buffer even when DESTROY_EQ fails, so
firmware keeps a live EQ whose pages are gone. The arena unmaps the
IOVAs, so a firmware write would fault rather than corrupt host memory.
The same was true of every source EQ ignored before adoption.

## Fix: suspend mlx5e under the RX fence

`MLX5_VFMIG_IOC_RX_FENCE` takes a new flag,
`MLX5_VFMIG_RX_FENCE_F_ETH_SUSPEND`:

- **RAISE** puts the fence up, then suspends the VF's mlx5e through
  `mlx5_adev_eth_suspend()`, which calls the ETH auxiliary driver's
  `suspend` under the same locks as `mlx5_detach_device()` (VF devlink,
  devcom, auxiliary device). If mlx5e is not bound, the raise succeeds
  and clears the flag in the returned `flags`. If the suspend fails, the
  fence comes back down and the raise fails.
- **LIFT** resumes mlx5e, then takes the fence down. If the resume
  fails, the fence stays up so the lift can be retried. Resuming an
  mlx5e that is not suspended does nothing.
- The kernel keeps no record of either, as with the fence. The caller
  keeps the handles and the returned flag.
- Old kernels reject the flag with `-EINVAL`, so the CRIU plugin can
  fall back to unbinding mlx5e.

The fence ioctl requires the VF RUNNING, so the kernel enforces the
order: drain, fence and suspend, SUSPEND_VHCA, SAVE, RESUME_VHCA, then,
if the source keeps running, resume and lift. Tying the suspend to the
fence also keeps the plugin to one state per VF: its existing lift on
an aborted dump becomes the rollback, and `--leave-running` needs the
same lift after a successful dump. After a dump whose tasks are killed,
the fence and the suspend stay until the orchestrator unbinds and
recycles the VF.

The suspend keeps the netdev registered, and that is the point.
ib_core removes RoCE GIDs only on `NETDEV_UNREGISTER` or an address
going away, not on carrier loss. So the netdev, its ifindex, its
addresses and all GIDs survive at the same indexes, and the source's QPs
work again after the resume. Unbinding does not allow that:

- **An unbind is a point of no return for the source's QPs.** Deleting
  a RoCE GID clears the firmware entry at once, but ib_core keeps the
  slot reserved (`GID_TABLE_ENTRY_PENDING_DEL`) while a QP references
  it. After a rebind, the IP's GID lands at a new index (3 became 4),
  and QPs connected before the unbind fail with `transport retry
  counter exceeded`, even when idle during it.
- **No unbind under SUSPEND_VHCA.** With the VF suspended, the unbind
  blocks in `unregister_netdevice: waiting for ethN to become free` and
  wedges the VF.

Two more things were measured with the fence, using an RC QP pair to a
peer VF in another netns:

- **The fence does not depend on mlx5e.** It sits in mlx5_core's own
  RDMA_RX steering, which mlx5e never touches. It drops inbound RoCE
  with mlx5e bound, and still does after an mlx5e unbind and rebind.
  Raised after the suspend, the plugin's fence lands in flow table 0x1,
  group 0x0, instead of table 0x4, group 0x16, because mlx5e's tables
  are gone.
- **Without the fence, mlx5e teardown still stops traffic.** In legacy
  eswitch mode, it empties the VF's unicast MAC list, so the eswitch no
  longer delivers to the VF. A peer's in-flight requests are then lost
  and hit `transport retry counter exceeded`. The suspend therefore
  comes after the fence, with nothing left unacknowledged.

Also fixed: removing mlx5e after a suspend destroyed its mdev resources
twice. `_mlx5e_remove()` unregisters the netdev, which calls
`_mlx5e_suspend()` with the device not present and
`MLX5E_STATE_DESTROYING` set. The orchestrator's unbind of a suspended
source then freed PD 0 and transport domain 0 (DEALLOC_PD and
DEALLOC_TRANSPORT_DOMAIN errors). `mlx5e_destroy_mdev_resources()` now
returns early once the resources are gone.

### Who does what

| Step                                                                 | Owner        |
|----------------------------------------------------------------------|--------------|
| Raise the fence with `F_ETH_SUSPEND` before SAVE                     | CRIU plugin  |
| Lift it with the flag after `--leave-running` or a failed dump       | CRIU plugin  |
| Unbind mlx5e instead, on kernels without the flag                    | CRIU plugin  |
| Unbind and recycle the source after a dump whose tasks were killed   | Orchestrator |
| VF admin MAC on the destination, same as the source's                | Orchestrator |
| IP addresses, namespace, bond or bridge placement, routes            | Orchestrator |
| Telling peers (ARP, neighbours)                                      | Orchestrator |

The destination binds a fresh mlx5e as usual; nothing is suspended
there.

The MAC matters: a connected QP resolved its destination MAC at RTR,
and that stays in its firmware context. LOAD does not carry the VF's
admin MAC, and a destination given another MAC brings up that MAC and
its default GIDs, so the restored QP fails with `transport retry counter
exceeded`. RECYCLE_VF keeps the admin MAC; `sriov_numvfs=0` clears it.
The IP has to be on the destination before `criu restore`, since adding
it creates its RoCE GIDs.

Reloading mlx5e on RESUME instead would not help: the image would still
carry the source's mlx5e objects. Carrying a journal of the source's
kernel objects so the destination could destroy them was rejected as far
more work.

### Validation

A self-loop RC QP gate stands in for the CRIU plugin, which does not
pass the flag yet: it raises its own fence with the flag before
`criu dump`, and the plugin's plain fence stacks on top. The QP is
connected through the source's RoCEv2 GID for an IPv4 address and sends
itself a zero-length message before the dump. The orchestrator's part
is done by setting the same admin MAC on both VFs through the PF, and
adding the same IP on the destination before `criu restore`.

- **Unbind before SAVE**, VF 0 -> VF 1 and VF 0 -> VF 0: both pass. The
  netdev goes, port 1 goes DOWN, all four GIDs are deleted, and the QP
  stays in RTS. The destination's GIDs come back at the same indexes.
  The saved firmware state drops from 4,866,048 to 3,858,432 bytes, and
  LOAD replays no mlx5e buffers (`frag_buf` 0 instead of 585).
- **Suspend under the fence**, VF 0 -> VF 1: passes. While suspended,
  the netdev stays with its IP, port 1 is DOWN, all four GIDs stay and
  the QP is RTS. After restore the QP is RTS and completes another send
  to itself.
- **Suspend and `--leave-running`**: passes. After the dump, lifting the
  gate's fence with the flag resumes mlx5e, lifting the plugin's fence
  reopens RX, the IP's GID is still at index 3, and the source's QP
  completes a send. The plugin leaves its fence up after a successful
  `--leave-running` dump today; it must lift it, with the flag.
- **Suspend with a peer** (fence raised and lifted with the flag, no
  dump): QPs connected before the raise, whether used or idle before it,
  pass traffic both ways after the lift. A send from the peer during
  the suspend is dropped and the peer's QP goes to ERR, as expected.
- **Unbind with a peer**: once the GIDs are gone, neither side's sends
  arrive. Both senders' QPs go to ERR, and rebinding does not bring the
  QPs back.

Not covered: more than one address, IPv6 or VLANs, where the
destination could add GIDs in a different order (CRIU could record the
source's GID table and check the destination's before restoring QPs);
a peer on another host; and switchdev eswitch mode.

## The remaining growth: transport domain and UMR

With mlx5e suspended before SAVE, the CRIU loop harness
(`test/rdma/run_vfmig_qp_traffic_loop.sh` in the CRIU tree, 20 hops,
alternating VF 0 and VF 1) still showed linear growth. Like the EQs and
mlx5e, both cases are objects that LOAD brings back while the
destination creates its own.

### Transport domain

A restore-mode `mlx5_ib_alloc_ucontext()` adopts the source's UARs, but
it allocated a new transport domain (TD). The source's TD stayed
allocated in firmware with no owner. The image grew by about 8.9 KB per
hop, even for a bare ucontext.

The ucontext meta now reports the TD and its owner's uid
(`MLX5_IB_VFMIG_UCTX_META_F_TDN`, from QUERY_UCONTEXT, or from
QUERY_DYN_UARS through its optional META attribute). The plugin passes
them back with `MLX5_IB_ALLOC_UCTX_VFMIG_ADOPT_TD`, and the ucontext
adopts that TD instead of allocating one. Growth drops from +8,875 to
+683 bytes per hop for the bare ucontext, and from +9,110 to +683 for
the QP holder. The +683 is one or two 4 KB pages of jitter, which go
down as well as up.

### UMR QP and CQ

With the TD adopted, a PD holder that registers and deregisters an MR
each hop still grew about 24 KB per hop. With no MR it stayed flat.
Registering twice per hop grew exactly as much as once. The image also
kept one more doorbell page (`db_page=1` in the drift line).

The first MR operation on a device calls `mlx5r_umr_resource_init()`.
That creates a kernel CQ (128 entries, shown as `cqe 255` in
`rdma res show cq`) and a `REG_UMR` QP, kept until the device is
removed. Their doorbell records take one DMA-coherent page. The FRMR
pools, which replace the old mkey cache, were ruled out: they create
one mkey per pop, with no batches, and aging destroys an idle one
within 120 s.

| run (6–8 hops) | `[mlx5_ib]` CQ on the restored VF | growth per hop | `db_page` |
|---|---|---|---|
| 1 MR per hop | `cqe 255`, every hop | +21,845 B | 1 |
| no MR | none | 0 | 0 |
| 1 MR, 130 s before each dump | not recorded | +25,600 B | 1 |

The 130 s wait lets aging destroy the idle pooled mkey before SAVE.
Growth stays the same, so the UMR pair is the cause. A restored MR's
bind also creates the pair on the destination, so any workload with MRs
grows, not only this probe.

The fix releases the pair on the source.
`MLX5_VFMIG_RX_FENCE_F_IB_RELEASE` on RAISE sends
`MLX5_DRIVER_EVENT_VFMIG_IB_RELEASE` to the VF, after the fence and the
mlx5e suspend. mlx5_ib then:

- destroys the idle FRMR-pool mkeys (`ib_frmr_pools_flush_idle()`);
- takes every slot of `umrc.sem`, so no UMR is in flight;
- destroys the UMR QP and CQ (`mlx5r_umr_resource_release()`).

The next UMR creates them again from `mlx5r_umr_post_send_wait()`. It
is refused, and the flag cleared in the returned flags, while the device
has an ODP or dma-buf MR, because their invalidations post UMRs from
contexts that cannot create a QP. `probes/test_ib_release.sh` checks
the release and the re-creation.

Activating it in the plugin, and confirming that a chain with an MR
per hop is flat with `db_page=0`, was still to do at the time of
writing.

## Open questions

- **GID order with several addresses.** One IPv4 address keeps its GID
  index (see Validation). Several addresses, IPv6 or VLANs may not.
- **Port events during the suspend.** The suspend takes port 1 DOWN, so
  ib_core reports `IB_EVENT_PORT_ERR` and then `IB_EVENT_PORT_ACTIVE`
  after the resume. Applications that react to the port going down could
  tear down QPs that would have survived.
- **Stage 1 TX drop.** A restored VF's mlx5e drops every transmitted
  packet (`vfmig: TX disabled on restored VF`). With mlx5e always fresh
  on the destination, that may no longer be needed.
- **Peers on another host.** The peer tests ran between two VFs of one
  PF. A remote peer, ARP and switchdev mode are untested.
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
  created from scratch on the destination. Suspending it before SAVE
  keeps the image flat, but does not reduce the commands; with
  `--leave-running` the source also builds it again on resume.
- **Polling mode.** With the cmd EQ adopted, the restored VF could
  switch the command interface to events, if the stale-EQE question is
  settled.

The first step is a per-opcode count of the commands a restored probe
issues, compared with a normal probe, to see which of these dominate.

Reproduce with:

    sudo CYCLES=4 tools/testing/criu_rdma/save_load/test_vf_recycle.sh

The driver split, the suspend gate and the peer tests were run with the
scripts in `probes/` (`growth_split_probe.sh`, `criu_dropeth_gate.sh`,
`eth_unbind_questions.sh`, with the `vfmig_fence` and `rc_pair`
helpers; the self-loop QP holder is the CRIU tree's
`test/rdma/qp_gid_holder`). The suspend gate has since been ported to
the CRIU tree as `run_vfmig_qp_traffic_cr.sh`. The transport-domain
and UMR results came from that tree's `run_vfmig_qp_traffic_loop.sh`.
