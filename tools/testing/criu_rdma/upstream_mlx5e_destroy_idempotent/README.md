# mlx5e destroys its mdev resources twice after a suspend

## Summary

When mlx5e is removed while suspended, `mlx5e_destroy_mdev_resources()`
runs twice. The suspend destroyed the PD, transport domain, mkey, TISes
and bfregs and zeroed their handles. The remove destroys them again.

- In our tree, with firmware alive, this frees PD 0 and transport domain 0.
  Firmware fails DEALLOC_PD (syndrome 0x593117) and DEALLOC_TRANSPORT_DOMAIN
  (syndrome 0x86358c). This is how it was found: the orchestrator unbound a
  VF whose mlx5e the RX fence had suspended (`F_ETH_SUSPEND`,
  `../design/restored_state_growth.md`).
- On stock mlx5, the only way to suspend mlx5e is system suspend. The
  remove has to follow a resume that failed. If mlx5e's own resume failed
  after it allocated its doorbell array, the second destroy calls
  `mlx5_free_bfreg()` on freed memory and frees the array a second time.
  That is a use-after-free and a double free.

Fixed in 6ccd4989 ("net/mlx5e: make mlx5e_destroy_mdev_resources()
idempotent"), on `func-devel-5`. It does not depend on vfmig, and as of
2026-11-05 nobody has fixed it upstream. It should go to net. This doc
covers what is left to do for that.

## The code

`_mlx5e_suspend()`, en_main.c:

    if (!pre_netdev_reg && !netif_device_present(netdev)) {
            if (test_bit(MLX5E_STATE_DESTROYING, &priv->state))
                    mlx5_sd_for_each_dev(i, mdev, pos)
                            mlx5e_destroy_mdev_resources(pos);
            return -ENODEV;
    }
    mlx5e_detach_netdev(priv);           /* sets MLX5E_STATE_DESTROYING */
    mlx5_sd_for_each_dev(i, mdev, pos)
            mlx5e_destroy_mdev_resources(pos);

The "not present and DESTROYING" branch came with bdf274750fca
("net/mlx5e: Don't attach netdev profile while handling internal
error"). It covers a profile change that failed during health recovery.
There, the mdev resources exist, but the netdev was never attached.

After a real suspend, the same two conditions hold, but the resources
are already gone. `_mlx5e_remove()` with the netdev still registered
does:

    unregister_netdev(netdev);
    _mlx5e_suspend(adev, false);         /* not present + DESTROYING: destroy again */

`mlx5e_destroy_mdev_resources()` ends with `memset(res, 0, ...)`. So a
second call after a clean destroy works on zeroes:
- `kfree(NULL)` is harmless;
- `num_bfregs` is 0, so no bfreg is freed;
- `tisn_valid` is false, so no TIS is destroyed;
- the mkey, transport domain and PD are destroyed by number 0.

A second call after a failed create is worse. The create error path
undoes what it made, but leaves `res` as it was:

    err_destroy_bfregs:
            for (i = 0; i < res->num_bfregs; i++)
                    mlx5_free_bfreg(mdev, res->bfregs + i);
            kfree(res->bfregs);          /* res->bfregs and num_bfregs left set */
    err_destroy_mkey: ...                /* mkey, tdn, pdn left set */

## Who reaches it

| Path | Suspend | Then | Firmware at the second destroy | Effect |
|------|---------|------|--------------------------------|--------|
| vfmig (our tree) | `MLX5_VFMIG_IOC_RX_FENCE` with `F_ETH_SUSPEND` | unbind the VF | alive | DEALLOC_PD/TD of 0; frees whatever owns number 0 |
| stock, resume fails before mlx5e | PCI PM (`mlx5_suspend`) | `mlx5_load_one()` fails, device unbound later | `INTERNAL_ERROR`; commands are faked | none visible (zeroed `res`) |
| stock, mlx5e resume fails in `mlx5e_create_mdev_resources()` after bfregs | PCI PM | `alloc tises failed`, device unbound later | `INTERNAL_ERROR` | UAF in `mlx5_free_bfreg()`, double `kfree()` of `bfregs` |
| stock, mlx5e resume fails in `mlx5e_attach_netdev()` | PCI PM | resume error path destroys and zeroes `res` | `INTERNAL_ERROR` | none visible |

In mainline, `adrv->suspend` is called only from
`mlx5_detach_device(dev, true)`. That runs only from
`mlx5_unload_one(dev, true)`, and that only from `mlx5_suspend()`, the
PCI legacy PM hook. Devlink reload, health recovery, firmware reset and
PCI error recovery all pass `suspend=false`, which removes the auxiliary
devices instead.

When `mlx5_attach_device()` fails, `mlx5_load_one_devl_locked()` unloads
the device and leaves it in `MLX5_DEVICE_STATE_INTERNAL_ERROR`. The
auxiliary devices stay bound. `mlx5e` stays suspended, with the netdev
registered and not present. The later unbind runs
`mlx5_uninit_one()`, then `mlx5_unregister_device()`, then
`_mlx5e_remove()`. In that state commands return a faked status, which
is why the stock cases show no firmware errors. The third row is
memory corruption either way.

## The fix

Two hunks in `en_common.c`:

1. `mlx5e_destroy_mdev_resources()` returns early when `res->bfregs` is
   NULL. A successful create sets it (`kzalloc_objs()` returns
   `ZERO_SIZE_PTR`, not NULL, for zero doorbells), and the final memset
   clears it.
2. The create error path sets `res->bfregs = NULL` after the `kfree()`.

Both are needed:
- **Hunk 2 alone:** the second destroy still loops `num_bfregs` times
  over `NULL + i`, which is a NULL dereference.
- **Hunk 1 alone:** the dangling pointer passes the guard, and the
  use-after-free remains.

Keying on `bfregs` keeps `struct mlx5e_hw_objs` (in
`include/linux/mlx5/driver.h`, inside `struct mlx5_core_dev`) unchanged,
so mlx5_ib does not need rebuilding.

## Upstream status (checked 2026-11-05)

- **The bug is still there.** `en_common.c` and `en_main.c` from
  netdev/net.git match our base for `mlx5e_create_mdev_resources()`,
  `mlx5e_destroy_mdev_resources()`, `_mlx5e_resume()`, `_mlx5e_suspend()`
  and `_mlx5e_remove()`.
- **Related, not the same:**
  - 3d5918477f94 "net/mlx5e: Fix netif state handling" (2024,
    CVE-2024-38608) added `pre_netdev_reg`, so probe unwind frees
    resources when `register_netdev()` fails. It was still being
    backported to 6.6.y in 2026-09.
  - "net/mlx5e: SD, Fix missing cleanup on probe error" (Shay Drory, net
    V5, 2026-05). It explicitly leaves failed-resume handling for socket
    direct to a follow-up (`sd_suspend`/`sd_resume`), not yet posted. That
    follow-up will touch the same resume and remove paths. Check for it
    before sending.
  - kTLS and PSP fixes for `DEALLOC_PD ... bad resource state` during
    devlink reload. There the PD is still referenced by keys, not freed
    twice.
- **No report found** of the double destroy or of the dangling `bfregs`.

## What needs to be done for upstream

1. **Rebase onto net** and check that the patch still applies, in case
   the SD follow-up has landed.
2. **Rewrite the commit message for mainline.** The trigger is PM suspend,
   then a failed resume, then device removal; vfmig goes in a note at
   most. Draft:

       net/mlx5e: Don't destroy mdev resources twice after a failed resume

       If mlx5e is removed while suspended, mlx5e_destroy_mdev_resources()
       runs twice: once from the suspend, and again from _mlx5e_remove()
       -> _mlx5e_suspend(), which destroys them whenever the netdev is not
       present and MLX5E_STATE_DESTROYING is set. Both hold after a suspend.

       A device stays bound with mlx5e suspended when the PM resume fails.
       If it failed in mlx5e_create_mdev_resources() after the doorbell
       array was allocated, the error path freed the array but left
       res->bfregs and res->num_bfregs set, so the second destroy calls
       mlx5_free_bfreg() on freed memory and frees the array again.

       Skip the destroy when the resources are already gone, and clear
       res->bfregs on the create error path.

       Fixes: bdf274750fca ("net/mlx5e: Don't attach netdev profile while handling internal error")

3. **Settle the Fixes tags.** bdf274750fca added the destroy branch. The
   use-after-free needs the doorbell array from 71fb4832d50b
   ("net/mlx5e: Use multiple TX doorbells"), so consider tagging that
   too. Layouts before it, with one embedded `bfreg`, were not checked.
4. **Decide one patch or two.** Splitting them as "skip the destroy
   twice" and "clear bfregs on the error path" does not work, since each
   hunk alone still crashes (see "Both are needed"). Keep them together.
5. **Expect a reviewer to ask for an explicit flag** (for example
   `bool created` in `struct mlx5e_hw_objs`) instead of keying on
   `bfregs`. The layout argument above does not apply upstream. The
   decision for now is to send the code as it is and change it if asked.
6. **Run the reproducer** below on a debug kernel, once without the fix
   and once with it, and quote the KASAN report in the cover letter or
   below the `---`.
7. **Check SD (socket direct).** `_mlx5e_suspend()` and
   `_mlx5e_resume()` loop over all SD devices, so the guard applies per
   device. Confirm by reading that a partial SD resume cannot leave one
   device's `res` with a stale pointer the guard misses.
8. **Send to net** (netdev, Tariq Toukan and Saeed Mahameed as mlx5
   maintainers), with a Signed-off-by only.

## Reproducer (written, not run)

`repro_failed_resume.sh` in this directory, with kernel config fragment
`repro_failed_resume.config`. It only
uses mainline code paths. In outline:

1. **Isolate one mlx5e.** Unbind every other mlx5_core PCI device. Turn
   off auxiliary autoprobe, and leave only `mlx5_core.eth` bound on the
   target, so mlx5_ib (which attaches before eth) stays out of it. The
   target must have no VFs.
2. **Arm failslab** for exactly one failure: `probability 100`,
   `times 1`, `ignore-gfp-wait N`, and a stack-trace filter
   (`require-start`/`require-end`) on `mlx5_core_create_tis()`. Every
   firmware command `kzalloc()`s a work entry in `cmd_alloc_ent()`, so
   the first CREATE_TIS of the resume fails with `-ENOMEM`. That is in
   `mlx5e_create_tises()`, after the doorbell array exists.
3. **Run a suspend cycle without sleeping:**
   `echo devices > /sys/power/pm_test; echo mem > /sys/power/state`. This
   runs every driver's suspend and resume callbacks and returns after
   about 5 s.
4. **Check that the fault hit the right place:** dmesg shows
   `FAULT_INJECTION: forcing a failure`, `alloc tises failed, -12` and
   `Device[..] (eth) failed to load`.
5. **Unbind the target from mlx5_core.** Without the fix, KASAN reports
   a slab use-after-free in `mlx5_free_bfreg()` and a double free in
   `mlx5e_destroy_mdev_resources()`. With the fix, the unbind is clean.
6. Restore everything: rebind the devices, autoprobe and `pm_test`.

`MODE=dry` runs step 3 alone, without the fault, to check that the PM
cycle works on the machine and that mlx5 comes back.

What running it takes:

- **A debug kernel:** `CONFIG_FAULT_INJECTION`,
  `CONFIG_FAULT_INJECTION_DEBUG_FS`, `CONFIG_FAILSLAB`,
  `CONFIG_FAULT_INJECTION_STACKTRACE_FILTER`, `CONFIG_KASAN` and
  `CONFIG_PM_DEBUG`. The current test kernel has only `CONFIG_PM_DEBUG`.
  Without failslab, BPF cannot do it either: `should_failslab()` is
  error-injectable only when `CONFIG_FAILSLAB` is built, and no mlx5
  function is. Without KASAN, the unfixed unbind may oops or silently
  corrupt memory, so the script refuses to run unless `FORCE=1`.
- **Two builds of `mlx5_core.ko`** from the same tree and config: one with
  6ccd4989 and one with it reverted. For example, run `git revert
  --no-commit 6ccd4989`, build with `M=drivers/net/ethernet/mellanox/mlx5/core`,
  save the module, then `git revert --abort`.
- **A machine where suspending every device for 5 s is acceptable.**
  `pm_test=devices` covers the disks and the NIC carrying ssh too. The
  Mellanox Cloud test host is a VM with virtio networking and passed-through
  mlx5 PFs, so it should be acceptable there, but expect ssh to stall.
  Alternatively, a nested guest (`/dev/kvm` is available) with an mlx5
  function passed through with vfio keeps the host out of it entirely.
- **Root access to `/proc/kallsyms`,** for the filter range.

Simpler, but not mainline: in our tree, a deterministic check of the
firmware-alive case needs no fault injection. Raise the RX fence with
`F_ETH_SUSPEND` on a bound VF, then unbind the VF. Without the fix, dmesg
shows the DEALLOC_PD and DEALLOC_TRANSPORT_DOMAIN errors. With it, it
shows none. That is the case the out-of-tree `growth_split_probe.sh` hit.
