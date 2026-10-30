# Restored-VF teardown: MANAGE_PAGES(TAKE) hangs, health poll rescues it

## Symptom

Unbinding a VF that was restored with LOAD_VHCA_STATE (for example via
`numvfs=0`) always logs:

    mlx5_core <vf>: poll_health:803:(pid 0): Fatal error 3 detected

Fatal error 3 is `MLX5_SENSOR_NIC_DISABLED`. It happens for both same-slot
and cross-VF restores, but never on a VF that was not restored. Teardown
takes 1.3-2 s longer than normal.

## Cause

A function_graph trace of `remove_one` (`save_load/test_xvf_save_load.sh`)
shows:

| step                                    | normal VF | restored VF |
|-----------------------------------------|-----------|-------------|
| `mlx5_cmd_teardown_hca`                 | 146 ms    | 116 ms      |
| `reclaim_pages` in `reclaim_startup`    | 0.26 ms   | 1266 ms     |
| `TEARDOWN_HCA` -> `stop_health_poll`    | 0.15 s    | 1.38 s      |

FW never completes the first MANAGE_PAGES(TAKE) issued after TEARDOWN_HCA on
the restored vHCA. The command interface stays in polling mode after LOAD, so
`poll_timeout()` spins. The next 2 s health poll sees NIC_DISABLED (normal
after TEARDOWN_HCA) and sets `MLX5_DEVICE_STATE_INTERNAL_ERROR`.
`poll_timeout()` then returns `-ENXIO`, about 0.6 ms after the poll in the
trace. `reclaim_pages_cmd()` treats `-ENXIO` as "FW gone" and force-frees
every page in the driver's page tree, silently.

So the fatal message is not noise from a slow teardown: it is what unblocks a
hung command. Without the health poll, the reclaim would wait for the full
command timeout (about 60 s) before the same forced reclaim.

## Risk

The forced reclaim returns pages to the host without FW acknowledging that it
has released them. `disable_hca` follows immediately, so this is probably
benign today, but it is unverified.

## Next steps

1. Confirm the `-ENXIO` path: add `poll_timeout`, `reclaim_pages_cmd` and
   `mlx5_cmd_do` to the trace filter with `funcgraph-retval`.
2. Instrument the reclaim: log `func_vhca_id`, the requested `npages` and
   `fw_pages`/page-tree size for the restored VF, and compare with a normal VF.
3. Work out why FW ignores TAKE. The lead suspect is the FW_PAGE entries
   replayed at LOAD and imported into `priv->page_root`: FW may not consider
   them reclaimable, or may expect a different function id or ownership.
4. Fix at the source, so FW answers TAKE or the driver skips reclaim for pages
   FW drops at disable_hca. Do not just reorder teardown: stopping the health
   poll earlier would turn the 1.3 s rescue into a 60 s timeout.
