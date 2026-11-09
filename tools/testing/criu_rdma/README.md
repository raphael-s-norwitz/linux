# tools/testing/criu_rdma -- out-of-band vfmig validators

Kernel-level harnesses for mlx5 vfmig that drive `/dev/mlx5_vfmig/<pf-bdf>`
directly, without CRIU. They isolate kernel and firmware behaviour from
the CRIU plugin and process restore.

## Build

```
make -C tools/testing/criu_rdma
```

Builds `tools/mlx5_vfmig`, a small CLI over the vfmig ioctls (see its
header comment for the verbs).

## Harnesses

- `save_load/test_xvf_save_load.sh` -- SAVE on one VF, recreate the VF
  set, LOAD onto another VF of the same PF, and bind it. Reports four
  stages separately: `arena` (every VF reserves the same IOVA window),
  `replay` (HOST_PAGE records replay into the destination), `fw_load`
  (LOAD_VHCA_STATE lands on the destination vHCA) and `probe` (the
  restored VF binds without allocator drift and brings up its ib
  device). `SRC_VF=DST_VF` runs the same-slot case.

  ```
  sudo PF=0000:08:00.0 SRC_VF=0 DST_VF=1 \
      tools/testing/criu_rdma/save_load/test_xvf_save_load.sh
  ```

  The image holds only the driver's kernel buffers; user verbs objects
  and RDMA traffic are out of scope. The harness resets every VF on the
  PF, so keep any traffic peer on another PF or host.

- `save_load/test_vf_recycle.sh` -- reuses VF 0 and VF 1 for SAVE/LOAD
  for `ITERS` rounds (default 10) without SR-IOV teardown, checking each
  round that `sriov_numvfs` and the virtfn links are unchanged and, with
  `trace/reset_trace.sh`, that the vacated VF is reset exactly once and
  nothing resets either VF between LOAD and the end of the restored
  probe. Cycles: 1 sysfs FLR then LOAD into a used VF, 2 the same via
  RECYCLE_VF, 3 recycle and reload a restored VF, 4 save from the VF
  restored in the previous round, 5 as 4 with a UUID handed off every
  round. Cycle 3 fails while restored-VF teardown logs a fatal health
  event (`design/teardown_reclaim_hang.md`).

  ```
  sudo PF=0000:08:00.0 tools/testing/criu_rdma/save_load/test_vf_recycle.sh
  sudo CYCLES="2 5" ITERS=20 tools/testing/criu_rdma/save_load/test_vf_recycle.sh
  ```

## Tracing

- `trace/reset_trace.sh` -- records PCI resets (`pcie_flr` and the
  `pci_reset_*` entry points) and mlx5 HCA lifecycle commands
  (ENABLE/DISABLE/INIT/TEARDOWN_HCA), with SAVE/LOAD_VHCA_STATE as window
  markers, and fails if any hit a watched VF other than as allowed.

  ```
  sudo tools/testing/criu_rdma/trace/reset_trace.sh arm
  sudo tools/testing/criu_rdma/trace/reset_trace.sh mark flr
  ...
  sudo tools/testing/criu_rdma/trace/reset_trace.sh report \
      --pf 0000:08:00.0 --vf 0000:08:00.2 --vf 0000:08:00.3 \
      --from flr --to load --allow flr@0000:08:00.2=1 \
      --allow reset@0000:08:00.2=1
  sudo tools/testing/criu_rdma/trace/reset_trace.sh disarm
  ```

  It reads struct offsets from the tree's `vmlinux` and `mlx5_core.ko`
  with gdb, so run it from the tree the running kernel was built from.

## Probes

One-off experiments behind `design/restored_state_growth.md`, kept so the
findings there can be reproduced. They are functional only, with no timing.
They reset SR-IOV on `$PF`, so keep traffic off it. `make ... probes` also
builds the helpers that need libibverbs.

- `probes/test_ib_release.sh` -- checks `MLX5_VFMIG_RX_FENCE_F_IB_RELEASE`
  on VF 0 with `umr_release_probe`: the first MR creates mlx5_ib's UMR CQ,
  the fence raise with the flag removes it, the next MR creates it again,
  and a LIFT with the flag is refused.
- `probes/run_criu_gates.sh` -- runs the CRIU tree's hardware vfmig gates
  (`CRIU_TREE=...`, `GATES="..."`) and summarizes each one's exit code and
  kernel warnings.
- `probes/growth_split_probe.sh` -- chains SAVE/LOAD over restored
  generations with only some of the VF's auxiliary drivers bound, or with
  mlx5e unbound, suspended or down before SAVE. It prints the image growth
  per generation.
- `probes/eq_gen_probe.sh` -- records the EQs, CQs and QPs on a normal VF
  and on restored generations, to show leaked source EQs.
- `probes/eth_unbind_questions.sh` -- RC traffic between VF 0 and VF 1,
  with VF 1 in its own namespace, across an mlx5e unbind or suspend, the
  RX fence, and RECYCLE_VF.
- `probes/criu_dropeth_gate.sh` -- a CRIU dump and restore of a
  self-loop RC QP, with the source's mlx5e unbound or suspended
  (`CRIU_REPO=...`). It has since been ported to the CRIU tree as
  `run_vfmig_qp_traffic_cr.sh`.

Helpers: `probes/vfmig_fence` raises and lifts the RX fence, optionally
with `eth`. `probes/rc_pair` drives RC QP pairs between two ib devices
from stdin. `probes/q_counter_room <ibdev>` prints how many more queue
counters the device can allocate (as root, through DEVX); run on each
restored VF of a chain, it drops by one per hop while a counter leaks.

```
make -C tools/testing/criu_rdma probes
sudo PF=0000:08:00.0 tools/testing/criu_rdma/probes/test_ib_release.sh
```
