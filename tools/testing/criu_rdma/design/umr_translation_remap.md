# Remapping CPU MR translations with UMR, and why queues cannot follow

## Summary

A tracked VF's user-space DMA lives in the USER_PAGE part of its IOVA
window: MR pages, CQ, QP and SRQ buffers, and doorbell records. They
have to sit at the same IOVAs on the destination because the device
state in a SAVE image names them by IOVA. That makes USER_PAGE the
limit on how much memory a restorable process can register, and it
shrinks with the window: about 3.5 GiB at 34 bits, about 251 GiB at 39.

CPU MRs do not have to stay in the window. An mkey's translation can be
rewritten with UMR (`mlx5r_umr_update_mr_pas()`), and the dma-buf
unbind/bind verbs already restore GPU MRs that way. Doing the same for
CPU MRs would leave only queue buffers and doorbell records in
USER_PAGE, at the cost of a UMR per MR at restore and a zap at dump.

Queue buffers cannot be moved the same way today. UMR only edits mkeys,
and QPs, CQs and SRQs do not reach their buffers through an mkey: their
translation is given to firmware once, at create time, and no command
rewrites it. That is an interface limit rather than a hardware one, and
a narrow firmware request would lift it: a command that rewrites a
UMEM's translation while the vHCA is suspended.

## What has to stay in the window today

The window is [2^(b-1), 2^b) for `iova_bits` b in 34..39
(`vfmig_iova.h`, "IOVA window layout"). It is cut into:

| part             | size                     | holds                              |
|------------------|--------------------------|------------------------------------|
| kernel slots 1-6 | 6 x 510 MiB, fixed       | cmd ring, FW pages, EQs, frag bufs, doorbells, coherent |
| slot 0           | 510 MiB, never allocated |                                    |
| kcoherent carve  | 1 GiB                    | reserved for non-migrated kernel DMA |
| USER_PAGE        | the rest of the window   | user MR, CQ, QP, SRQ buffers; DBRs |

At 34 bits USER_PAGE is [0x31f200000, 0x400000000), 3598 MiB. The kernel
part is fixed whatever the window size, so every bit taken off the
window comes out of USER_PAGE.

Everything in USER_PAGE is placed by the ib_core umem-placement hook,
so each umem gets the same IOVA on the destination as on the source.
For an MR that IOVA ends up in the mkey's MTTs. For a queue it ends up
in the translation firmware built from the create command's page list.
Both travel in the SAVE image as IOVAs.

## CPU MRs: rewrite the mkey instead of reproducing the IOVAs

### The mechanism already exists

`mlx5r_umr_update_mr_pas()` (`drivers/infiniband/hw/mlx5/umr.c`) builds
an MTT or KSM list from `mr->umem` and posts a UMR WQE on mlx5_ib's UMR
QP. The WQE names the mkey (`mr->mmkey.key`), an offset into its
translation table and a mask of the mkey fields to change. The mkey's
virtual address, length, lkey and rkey stay the same; only the DMA
addresses behind them change. That is what a restored application
needs: its posted work requests and its peers' rkeys keep working.

The GPU path uses it for dma-buf MRs, in `drivers/infiniband/hw/mlx5/mr.c`:

- `mlx5_ib_unbind_dmabuf_mr()` (UVERBS_METHOD_MR_UNBIND_DMABUF), before
  SAVE: zaps the translations, detaches the dma-buf, and revokes the
  mkey with `mlx5r_umr_revoke_mr()`, leaving it disabled with its key.
- `mlx5_ib_bind_dmabuf_mr()` (UVERBS_METHOD_MR_BIND_DMABUF), after
  restore: attaches the new dma-buf and enables the mkey with one
  `MLX5_IB_UPD_XLT_ENABLE` update carrying PD, access and translations.

### A firmware constraint it found

Rewriting the translations of a *live* mkey works for an MR this
driver registered but fails for one RESTORE_MR adopted, with a vendor
syndrome that names no cause. Enabling a *disabled* mkey works for both
(comments in `mlx5_ib_unbind_dmabuf_mr()` and
`mlx5_ib_bind_dmabuf_mr()`). So the mkey has to be disabled before
SAVE, not rewritten in place after restore.

### What it would look like for CPU MRs

1. Dump, under the RX fence: for each CPU MR, zap and revoke the mkey
   as the dma-buf unbind does. The fence is what makes this safe; an
   incoming RDMA access to a zapped mkey would move the responder QP to
   an error state.
2. The SAVE image then carries disabled mkeys and no MR IOVAs.
3. Restore: pin the MR's pages with `ib_umem_get()` outside the window,
   wherever dma-iommu puts them, and enable the adopted mkey with
   `MLX5_IB_UPD_XLT_ENABLE`. The umem-placement hook places every umem
   on a tracked VF in USER_PAGE today, so it would have to let MR umems
   through, on the source as well as the destination.

Costs and limits:

- One UMR per MR at restore, its size proportional to the MR's pages,
  plus one zap and one revoke per MR at dump. This is checkpoint and
  restore time only; data-path cost is unchanged.
- The source keeps the MR disabled after SAVE. That is fine when the
  source is torn down after a migration, but a checkpoint that lets the
  source continue would need to re-enable its MRs.
- The page size chosen for the new translation may differ from the
  source's. `log_page_size` goes out in the same enable, as the
  dma-buf bind already relies on.
- ODP MRs and data-direct MRs are out of scope. The dma-buf path
  refuses data-direct MRs for the same reason: the second mkey on the
  data-direct device would have to be rewritten too.

## Why QP, CQ and SRQ buffers cannot follow

### UMR reaches only mkeys

A UMR WQE has no field for a QP, CQ or SRQ. Its target is an mkey
index, and the translation it writes is that mkey's.

### Queue translations are fixed at create

A queue gets its translation in one of two ways, both set at create:

- An inline page list. The work queue context (`struct mlx5_ifc_wq_bits`
  in `include/linux/mlx5/mlx5_ifc.h`, and the CQ and QP equivalents)
  carries `log_wq_pg_sz`, `page_offset` and a trailing `pas[]` array.
  mlx5_ib fills it from the user's umem with `mlx5_ib_populate_pas()`
  (`qp.c`, `cq.c`). Firmware copies it into translation tables it owns.
- A UMEM object. The contexts can name one instead (`wq_umem_id` and
  `wq_umem_valid`, `cq_umem_valid`, `dbr_umem_id`). A UMEM
  (`struct mlx5_ifc_umem_bits`) is a standalone MTT list. It has
  CREATE_UMEM (0xa08) and DESTROY_UMEM (0xa0a) and no modify command.

The doorbell record is a raw 64-bit IOVA in the context (`dbr_addr`),
not a page list, unless `dbr_umem_id` is used.

### One precedent: CQ resize

MODIFY_CQ with `op_mod` MLX5_CQ_OPMOD_RESIZE (`mlx5_ib_resize_cq()`,
`cq.c`) takes a new page list for a live CQ, so firmware can already
repoint an existing queue. Its semantics are a move to a new buffer:
completions already written stay in the old one for software to drain.
mlx5_ib skips a resize to the same size. It is not usable as "same
pages, new IOVAs" without firmware confirming that is legal, and send
queues, receive queues and SRQs have nothing like it.

### Not a hardware limit

The device translates WQE fetches and CQE writes with the same kind of
MTT entries an mkey uses; what differs is that the queue owns them and
no command edits them. This is our reading and needs firmware to
confirm. For migration the usual objection to remapping, DMA in flight
through the old translation, does not apply: between LOAD_VHCA_STATE
and resume the vHCA does no DMA.

## Asking firmware

In increasing order of firmware work:

1. **MODIFY_UMEM**: rewrite a UMEM's MTTs, valid at least while the
   vHCA is suspended or stopped, with firmware invalidating whatever it
   caches from them. On our side, user QP, CQ and SRQ buffers and their
   doorbell records would be created through UMEMs instead of inline
   page lists; the fields exist in the create commands already. Restore
   would issue one MODIFY_UMEM per queue before resume, the queue
   counterpart of the MR enable above.
2. **Remap during LOAD_VHCA_STATE**: pass firmware a table from source
   IOVAs to destination IOVAs and let it patch every object in the
   image. This covers inline page lists and raw `dbr_addr` too, with no
   driver change to how queues are created.
3. **Queue buffers through an mkey**: let a queue name its buffer by
   lkey and offset, so UMR covers it. The most general, and the largest
   change: a new context format and mkey checks on every WQE fetch.

Option 1 is the one to lead with: it adds one command on an existing
object and leaves the queue data path alone.

## What stays in the window regardless

None of this moves the kernel part of the window. The command ring, EQs
and the driver's own queues are kernel buffers firmware addresses by
IOVA, and the pages given to firmware with MANAGE_PAGES are used by
firmware directly; no UMEM path reaches those. The kernel slots and the
kcoherent carve, about 4.5 GiB, stay.

With CPU MRs on UMR, USER_PAGE holds only queue buffers and doorbell
records, which are small next to registered memory, so a 34-bit window
would leave far more room than a process's queues need. With option 1
as well, USER_PAGE could go entirely.

## Plan

1. Measure how USER_PAGE splits between MRs and queues for the
   workloads we care about, from the umem-placement records in a SAVE
   image, to size the gain before writing code.
2. Find out whether revoking a live adopted mkey works. If it does, the
   zap could move from dump to restore and the source's MRs could stay
   live after SAVE.
3. Prototype CPU MR unbind and bind on the dma-buf verbs' shape:
   a vfmig-only verb pair, or the CRIU plugin calling into
   `mlx5r_umr_revoke_mr()` and `mlx5r_umr_update_mr_pas()` through
   mlx5_ib, with a harness that restores an MR outside the window and
   moves traffic through it.
4. Raise option 1 with the firmware team, with the CQ resize precedent
   and the suspended-vHCA argument above, and ask whether CQ resize to
   the same size with new IOVAs is legal in the meantime.

## Open questions

- Does firmware translate queue buffers with the same MTT machinery as
  mkeys? Option 1's cost depends on it.
- Is the adopted-mkey rewrite failure specific to live mkeys, or would
  it also hit a MODIFY_UMEM on an adopted UMEM?
- Does the UMR QP on a restored VF exist early enough to enable MRs
  before the application resumes? The RX fence currently releases it
  before SAVE (`MLX5_VFMIG_RX_FENCE_F_IB_RELEASE`), and the next UMR
  recreates it.
