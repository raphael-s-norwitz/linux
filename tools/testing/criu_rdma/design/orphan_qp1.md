# Restored VF: the source's QP1 still receives, into the source's buffers

Status 2026-11-10. Open; to be fixed at a higher level (see Options).
Found with `probes/qp1_orphan_probe.sh`.

## Background

On a normal VF, ib_mad opens port 1 at mlx5_ib probe time. It creates the
GSI QP (QP1) with its send and receive CQs and PD, and posts receive
buffers. Those buffers are kmalloc'ed and mapped with `dma_map_single()`.
They are ordinary streaming mappings in the VF's dma-iommu domain, outside
the vfmig arena (see "Coexistence with dma-iommu" in
`drivers/net/ethernet/mellanox/mlx5/core/vfmig/vfmig_iova.h`).

SAVE_VHCA_STATE captures every firmware object on the VHCA, so the image
carries that QP1 in RTS together with its posted receive WQEs. Those WQEs
hold the source's buffer IOVAs. LOAD brings all of it back on the
destination.

On a restored VF, mlx5_ib refuses to set up a GSI QP of its own
(`mlx5_ib_dev_res_srq_init()` returns `-EOPNOTSUPP` when
`mlx5_vf_is_restored()`), so ib_mad leaves the port out ("Couldn't open
port 1"). The source's QP1 is then the only QP1 in firmware, and nothing
on the destination knows about it or its CQs.

## What happens

Any RoCE packet addressed to the restored VF's QP1 is placed by firmware
in the next posted receive WQE. Such packets include CM REQs, SIDR REQs
and other unsolicited MADs, from any node that can reach the VF's GID.
Firmware writes the payload to a source IOVA and the CQE to the source's
receive CQ, and raises a completion event if that CQ is armed.

`probes/qp1_orphan_probe.sh` (CX-7, VF 1 saved, loaded into VF 0, VF 1
rebound as the peer) sent three `rping -c` connects to the restored VF.
The restored VF has TX disabled, so the peer reaches it through a static
neighbor entry and the CM retries each REQ until it gives up:

| | control (source VF) | restored VF |
|---|---|---|
| rping | 3 of 3 connected | 3 of 3 `RDMA_CM_EVENT_UNREACHABLE` |
| `port_rcv_packets` | - | 27 -> 75 (3 connects x 16 REQ tries) |
| "Completion event for bogus CQ" | 0 | 2, both for CQ 0x1a61 |
| IOMMU faults | 0 | 0 |

The bogus CQ events show that the orphan QP1 consumed the REQs and
completed them to a CQ the destination does not track. There are only two
because the CQ is not re-armed after an event; later CQEs are written
without one.

No IOMMU fault on 48 writes means the source's buffer IOVAs are mapped in
the destination's domain. The destination's own streaming mappings, for
example mlx5e's receive pages, are allocated from the same kind of domain
in a similar order, so they very likely sit at those IOVAs. That is not
proven, but the expected result is silent memory corruption on the
destination, triggered from the network. The same applies to the CQE
writes if the GSI CQ's buffer is not in the arena.

The exposure starts when the carried RoCE RX fence is lifted on the
destination and lasts as long as the VF stays restored. Each receive WQE
is used once, so the number of writes is bounded by the source's posted
receive depth (ib_mad's `recv_queue_size`, 512 by default). After that,
firmware drops the packets for lack of WQEs.

## Related effects of the same leftover objects

- The GSI and MAD CQs, together with mlx5_ib's devr CQ `c0`, sit on comp
  EQ 0x6. They keep it busy, so DESTROY_EQ fails with syndrome 0x30f433
  ("bad resource state") on every restored teardown.
  `c500d836e6a7` keeps the EQ's pages in the vfmig domain when that
  happens, so firmware cannot write to freed memory, but the EQ itself
  remains.
- RDMA CM does not work on a restored VF, in either direction, because
  there is no MAD agent on the port. The traffic gates exchange QP
  attributes out of band (`rc_pair`, the CRIU holders).
- The leftovers come from generation 0 only. A restored VF creates no new
  GSI objects, so they do not grow per hop.

## Options

At some point the GSI and MAD QPs and CQs will need to be preserved, so
that ib_mad and RDMA CM work on a restored VF. That needs their buffers
(the QP, the CQs and the posted receive buffers) to come from the arena,
and ib_mad's per-port state to be rebuilt around the restored objects. It
is a large change and is not planned yet.

Until then, the restored QP1 must not receive anything. Candidates:

1. **Drop QP1 traffic in steering.** Install a rule that drops RoCE v2
   packets (UDP dport 4791) whose BTH destination QP is 1, before the
   carried RX fence is lifted, and keep it while the VF is restored.
   `fte_match_set_misc` has `bth_dst_qp`, gated by the
   `ft_field_support.bth_dst_qp` capability. It can be built like the RX
   fence, with the same table and the same owner. This is the
   higher-level fix assumed for now: it needs no knowledge of the
   orphan's object numbers.
2. **Move the orphan QP1 to RESET on restore.** Firmware drops packets for
   a QP in RESET and returns no WQEs. This needs the QP's firmware number,
   which is not 1 for a GSI QP. The number could be carried as a
   HOST_KOBJ record, like the Q counter.
3. **Destroy the GSI and MAD objects on restore**, using numbers carried
   the same way. Doing the same for `c0` would also let DESTROY_EQ on
   EQ 0x6 succeed. This conflicts with preserving them later.

Whatever the fix, `probes/qp1_orphan_probe.sh` is the regression test.
`port_rcv_packets` should still rise in the hazard phase (the REQs reach
the VF), with no bogus CQ events and no IOMMU faults.
