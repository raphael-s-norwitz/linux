# Restored QPs and CQs run in firmware before their buffers are bound

## Summary

On the destination, the restored VHCA runs from the VF's bind probe, but
the user buffers of its QPs, CQs and MRs are unmapped in the IOMMU until
CRIU's RESTORE_QP, RESTORE_CQ and RESTORE_MR bind them, late in the
restore. Firmware that touches one of those buffers in between reaches
unmapped IOVA. The RoCE RX fence stops incoming traffic in that window,
but not the requester's own timers. An RC send that is retrying at SAVE
fails on the destination before its QP is restored. The QP is in ERR
after the restore with no error CQE on its CQ, so the process cannot
tell what happened to the send.

Keeping the VHCA stopped until the buffers are bound would close the
window, but July's work showed that a restored VF cannot be bound, nor
its datapath toggled again, that way (see "Why DEFER_RESUME does not
help").

## Observation

`probes/cq_early_replay.sh` (CX-7, firmware 28.48.1000, QEMU guest,
2026-11-24). `cq_early_holder` arms a CQ, E, that is the send and receive
CQ of a self-loop RC QP. The QP sends once with no receive posted, so it
answers itself with RNR NAKs and, with `rnr_retry` 7, retries for ever.
Its ACK timeout is 16 (about 268 ms) and `retry_cnt` is 7.

- **Control, no migration.** With the fence raised on the live source VF
  through `vfmig_fence`, the send fails within 5 s with "transport retry
  counter exceeded" on E. So the fence drops the QP's loopback, and the
  QP fails on its ACK timeouts.
- **Migration.** A second holder is dumped (`criu dump ok`, fence up
  about 30 ms before SUSPEND), loaded onto VF 1 and restored 5 s later.
  The destination's fence is up from LOAD until RESUME_IBDEV, about
  5.5 s. After the restore:
  - the QP reads ERR in mlx5 debugfs;
  - E is empty, and its `cqc.st` still reads 0x9 (armed), so firmware
    never raised a completion event for it;
  - posting the receive the send waited for gives no completion in 10 s.

  `mlx5_core_adopt_cq()` delivered no early event for E, and there was
  no "Async event for bogus CQ" or "CQ error" line.
- **Migration, with the QP event warning.** A rerun on a kernel that
  warns when an event for a QP not yet adopted is dropped ("vfmig: event
  0x.. for QP 0x.. before its adoption lost") gave the same result and
  no such warning. So firmware raised no affiliated QP event (WQ
  catastrophic, access or invalid request error) once mlx5_ib's notifier
  was registered, about 0.26 s after the VHCA resumed. One raised before
  that would also go unseen.

So the QP reached ERR without a completion event on E and without an
affiliated event.

The DESTROY_EQ failure on comp EQ 6 and "Fatal error 3" at teardown in
the same runs are the known restored-VF teardown issues
(`restored_state_growth.md`, `teardown_reclaim_hang.md`).

## Mechanism

Destination order:

1. The prerestore tool stages LOAD; the VF's bind probe applies it in
   `mlx5_vfmig_vf_apply_pending_load()` and resumes the VHCA, STOP to
   RUNNING ("applied ... resumed" in the log). The VF's comp and async
   EQs are adopted in `mlx5_load()` right after.
2. LOAD installs an awaiting-bind placeholder in the vfmig IOVA domain
   for each user buffer: `vfmig_iova_replay_external()` reserves the
   IOVA range and links nothing (`dma_iova_link()` runs only at bind).
3. CRIU restores the process. RESTORE_CQ, RESTORE_QP and RESTORE_MR run
   in the restorer (pie), because they pin the process's restored
   memory; that is when the placeholders get bound.
4. RESUME_DEVICES_LATE, the dma-buf bind pass, then RDMA_RESUME_IBDEV
   lifts the RX fence.

Between 1 and 3 the requester runs with every user buffer unmapped. A
retransmission rereads the send WQE from the SQ buffer, and a completion
is written to the CQ buffer; both reach unmapped IOVA. In a guest the
IOMMU faults land on the host and are not seen here.

What exactly failed is inferred, not observed: either the WQE fetch
faulted and firmware failed the QP, or the retry counter ran out and the
error CQE was dropped. Both leave the QP in ERR. That E was never fired
favours the first, or a CQE write that did not count as one. A fetch
fault would normally raise a WQ catastrophic or access error event,
which the rerun did not see; if that is what happened, firmware either
raised it in the first 0.26 s after the resume or raised none.

## What it affects

- **RC sends retrying at SAVE because of RNR NAKs.** The source plugin
  waits for every RC QP's sends to be acknowledged before the fence
  (`vfmig_wait_qp_acked()`, `last_acked_psn == next_send_psn - 1`). The
  rerun's dump log shows that check passing on its first query with
  `next_send_psn=0 last_acked_psn=0xffffff`, although the send, PSN 0,
  had not completed: an RNR NAK rewinds `next_send_psn` to the NAKed
  PSN while the requester waits out the RNR timer. The holder's timer
  is 491 ms, so the QP is in that state almost all the time, and the
  PSN check cannot see a send held by RNR NAKs. RNR NAKs are rare in a
  well-behaved application, so a dump racing one is an edge case; the
  holder forces it.
- **RC sends whose ACK the source fence dropped.** The drain is meant to
  exclude them; any it misses fail the same way.
- **UD and UC sends still queued in firmware at SAVE**
  (`event_files.md`, "mlx5 CQs across LOAD"). They complete without an
  ACK, so the fence does not hold them; their completions go to an
  unmapped CQ buffer.
- **Event files.** A completion EQE needs a CQE first, so the early-EQE
  replay in `mlx5_core_adopt_cq()` covers only the gap inside
  RESTORE_CQ between the umem bind and the adoption. The end-to-end
  replay probe cannot produce one and is parked.

## Why DEFER_RESUME does not help

The kernel can leave a loaded VHCA at STOP
(`MARK_RESTORED { DEFER_RESUME }`, c7120f899436) for CRIU to resume once
everything is in place. CRIU tried this and its neighbours on its
`criu-rdma-dev-poc` branch (June and July 2026) and backed out:

- **STOP leaves the VF's command ring dead** (CRIU 84811ae22). The bind
  probe, mlx5_ib's init and every RESTORE_* issue commands, so a VF
  loaded with DEFER_RESUME cannot finish binding.
- **RUNNING_P2P does not keep the command ring serviceable either**, so
  stopping only the initiator at LOAD has the same problem (CRIU
  ab263c2b0).
- **Toggling a restored VHCA again wedges it.** SUSPEND(INITIATOR) then
  RESUME(INITIATOR) on a restored, running VHCA both succeed, but the
  firmware requester is not re-armed; the next UMR post hangs in
  `mlx5r_umr_post_send_wait()` (CRIU ab263c2b0, confirmed as a firmware
  property). One monotonic STOP to RUNNING at bind is all a restored
  VHCA takes today.

So the VHCA has to run from the bind probe, before the buffers exist.

## Options

1. **Source: detect a send still in flight and fail the dump.** The
   PSNs alone are not enough, since an RNR NAK rewinds them.
   `VFMIG_QUERY_QP`'s `RESP_SQ_STATE` reports the rest of the QPC's
   requester state (`ssn`, `hw_sq_wqebb_counter`, `sw_sq_wqebb_counter`,
   `cur_rnr_retry`, `cur_retry_count` and their configured values) from
   the same QUERY_QP the drain already issues, so checking it costs no
   extra firmware command. Waiting longer is not a fix: an RNR NAK
   means the responder is short of receives, and if the responder is
   in the same dump it is frozen and the send never drains. So the
   plugin should, once the PSN drain passes, check the field that marks
   an RNR-held send and fail the dump with the QP named (or warn). The
   process keeps running and the caller can retry, instead of a silent
   loss after restore. Which field is reliable on CX-7 is not known
   yet: `probes/qp_sq_state_probe <ibdev> <sgid_index>` compares the
   candidates across fresh, idle, RNR-held and freed QPs on a kernel
   with `RESP_SQ_STATE`. If none separates them, document the edge case
   instead. The kernel side is in this tree; the check belongs to the
   CRIU plugin's `vfmig_wait_qp_acked()`. Whether the counters cover
   queued UD sends is a separate question.
2. **Destination: make the loss visible.** After the restore, read each
   restored QP's state and report those that reached ERR before their
   adoption, and count the QP events dropped before adoption (now
   warned). This does not save the send, but turns a silent failure into
   a reported one.
3. **Firmware: a per-function requester freeze that keeps the command
   ring serviceable and can be lifted once on a restored VHCA.** That is
   what DEFER_RESUME was meant to be. It is a firmware request, not
   something the kernel or CRIU can do alone.
4. **Destination: back the placeholders with scratch pages.** This
   avoids the IOMMU fault but writes the lost CQEs into a page nobody
   reads, so it hides the problem. Not recommended.

Option 1 is the only one that keeps a send from being lost silently
with today's firmware; option 2 is cheap and worth having either way.

## Open questions

- Did the QP fail on a WQE fetch or on its retry counter? No affiliated
  event was seen after mlx5_ib registered its notifier. The host's IOMMU
  fault log for the VF during the window, or firmware's own error
  counters, would tell the two apart.
- Does SUSPEND_VHCA on the source flush or hold UD sends still queued,
  and are their completions written before SAVE?
- Which `RESP_SQ_STATE` field marks an RNR-held send? Run
  `probes/qp_sq_state_probe` (non-root, any mlx5 device with a RoCE
  GID) on a kernel with `RESP_SQ_STATE`; it exits 0 and prints the rule
  if one separates RNR-held from idle QPs, 1 if none does. Record the
  answer in the `struct mlx5_ib_vfmig_qp_sq_state` comment and here,
  then hand it to the CRIU plugin's drain.

## Status

- `cq_early_replay.sh` cannot pass while this is open: its QP fails
  before RESTORE_CQ, so no completion event reaches the replay. It is
  kept as the reproducer for this note, not as a gate.
- The replay in `mlx5_core_adopt_cq()` stays; it covers the gap inside
  RESTORE_CQ.
