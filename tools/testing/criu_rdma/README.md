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
