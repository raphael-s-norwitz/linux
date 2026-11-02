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
