#!/usr/bin/env bash
#
# Reproduce the mlx5e double destroy of mdev resources on stock mlx5:
# system suspend, a resume that fails inside mlx5e_create_mdev_resources()
# after the bfregs array is allocated, then removal of the device.
#
#   PM suspend  -> mlx5_suspend -> mlx5e_suspend: detach netdev (sets
#                  MLX5E_STATE_DESTROYING), destroy mdev resources (memset)
#   PM resume   -> mlx5_resume -> mlx5e_resume -> mlx5e_create_mdev_resources:
#                  bfregs allocated, CREATE_TIS fails (injected), error path
#                  kfree()s bfregs but leaves res->bfregs and num_bfregs set
#   unbind      -> _mlx5e_remove -> unregister_netdev -> _mlx5e_suspend:
#                  netdev not present + DESTROYING -> destroy mdev resources
#                  again: mlx5_free_bfreg() on freed memory, kfree() twice
#
# Without the fix, a KASAN kernel reports a use-after-free in
# mlx5_free_bfreg() and a double free in mlx5e_destroy_mdev_resources().
# With it, the unbind is clean.
#
# No suspend actually happens: /sys/power/pm_test=devices runs every
# driver's suspend and resume callbacks and returns after about 5 s. All
# devices of this machine go through them, including its disks and the NIC
# carrying ssh, so the machine is unresponsive for those seconds. Prefer a
# VM.
#
# The fault is one failed kmalloc, the command work entry of the first
# CREATE_TIS after arming, selected with failslab's stack-trace filter on
# mlx5_core_create_tis(). Needs CONFIG_FAULT_INJECTION,
# CONFIG_FAULT_INJECTION_DEBUG_FS, CONFIG_FAILSLAB,
# CONFIG_FAULT_INJECTION_STACKTRACE_FILTER; CONFIG_KASAN to see the bug
# reliably (without it, the second destroy may oops or corrupt memory
# silently, so the script refuses unless FORCE=1).
#
# Kernel config: repro_failed_resume.config next to this script.
#
#   sudo MODE=dry ./repro_failed_resume.sh     # PM cycle only, no fault
#   sudo ./repro_failed_resume.sh              # inject, then unbind
#
# TARGET is the mlx5 PCI function to test (default 0000:08:00.1). For the
# run, every other mlx5_core PCI device is unbound, so only TARGET's mlx5e
# runs into the fault, and auxiliary autoprobe is turned off with only
# mlx5_core.eth bound on TARGET, so mlx5_ib and fwctl stay out of it. All of
# it is restored on exit. TARGET must have no VFs.
#
# Functional only; no timing.

set -uo pipefail

TARGET="${TARGET:-0000:08:00.1}"
MODE="${MODE:-inject}"
FORCE="${FORCE:-0}"
FS=/sys/kernel/debug/failslab
PCI=/sys/bus/pci/drivers/mlx5_core
AUX=/sys/bus/auxiliary
MARK="repro_failed_resume: $$"

note() { printf '  %s\n' "$*"; }
die() { echo "FAIL: $*"; exit 1; }

[[ "$EUID" -eq 0 ]] || { echo "run as root"; exit 2; }
[[ "$MODE" == dry || "$MODE" == inject ]] || { echo "MODE is dry or inject"; exit 2; }
[[ -e "$PCI/$TARGET" ]] || { echo "$TARGET is not bound to mlx5_core"; exit 2; }
[[ "$(cat /sys/bus/pci/devices/$TARGET/sriov_numvfs 2>/dev/null || echo 0)" == 0 ]] \
	|| { echo "$TARGET has VFs; set sriov_numvfs=0 first"; exit 2; }
grep -qw devices /sys/power/pm_test 2>/dev/null || { echo "no /sys/power/pm_test (CONFIG_PM_DEBUG)"; exit 2; }
grep -qw mem /sys/power/state || { echo "no 'mem' in /sys/power/state"; exit 2; }
mountpoint -q /sys/kernel/debug || mount -t debugfs none /sys/kernel/debug

kasan=0
dmesg | grep -q "KernelAddressSanitizer initialized" && kasan=1
if [[ "$MODE" == inject ]]; then
	[[ -d "$FS" ]] || { echo "no $FS (CONFIG_FAILSLAB, CONFIG_FAULT_INJECTION_DEBUG_FS)"; exit 2; }
	[[ -e "$FS/require-start" ]] || { echo "no $FS/require-start (CONFIG_FAULT_INJECTION_STACKTRACE_FILTER)"; exit 2; }
	[[ $kasan == 1 || $FORCE == 1 ]] || { echo "no KASAN; the unpatched unbind may oops or corrupt memory silently (FORCE=1 to run anyway)"; exit 2; }
fi

# --- set up: only TARGET's mlx5e in play --------------------------------
others=()
for d in "$PCI"/0000:*; do
	d="$(basename "$d")"
	[[ "$d" == "$TARGET" ]] || others+=("$d")
done
old_autoprobe="$(cat $AUX/drivers_autoprobe)"
old_pm_test="$(sed -n 's/.*\[\(.*\)\].*/\1/p' /sys/power/pm_test)"

cleanup() {
	if [[ -d "$FS" ]]; then
		echo 0 >"$FS/probability"
		echo 0 >"$FS/require-start" 2>/dev/null
		echo 0 >"$FS/require-end" 2>/dev/null
	fi
	echo "$old_pm_test" >/sys/power/pm_test
	echo "$old_autoprobe" >$AUX/drivers_autoprobe
	# A rebound TARGET probes fresh aux devices; a TARGET still bound has
	# the ones we unbound, or that the resume re-added, left unbound.
	if [[ -e "$PCI/$TARGET" ]]; then
		for a in /sys/bus/pci/devices/$TARGET/mlx5_core.*; do
			[[ -e "$a" && ! -e "$a/driver" ]] || continue
			echo "$(basename "$a")" >"$AUX/drivers_probe" 2>/dev/null
		done
	else
		echo "$TARGET" >"$PCI/bind" 2>/dev/null
	fi
	for d in "${others[@]}"; do
		[[ -e "$PCI/$d" ]] || echo "$d" >"$PCI/bind" 2>/dev/null
	done
}
trap cleanup EXIT

echo "== setup (TARGET=$TARGET, MODE=$MODE, KASAN=$kasan)"
for d in "${others[@]}"; do
	echo "$d" >"$PCI/unbind" && note "unbound $d from mlx5_core for the run"
done
echo 0 >$AUX/drivers_autoprobe
for a in /sys/bus/pci/devices/$TARGET/mlx5_core.*; do
	a="$(basename "$a")"
	[[ "$a" == mlx5_core.eth.* ]] && continue
	[[ -e "$AUX/devices/$a/driver" ]] || continue
	echo "$a" >"$AUX/devices/$a/driver/unbind" && note "unbound $a"
done
eth="$(basename "$(ls -d /sys/bus/pci/devices/$TARGET/mlx5_core.eth.* 2>/dev/null | head -1)")"
[[ -n "$eth" && -e "$AUX/devices/$eth/driver" ]] || die "mlx5e is not bound on $TARGET"
net="$(ls /sys/bus/pci/devices/$TARGET/net 2>/dev/null | head -1)"
note "mlx5e on $TARGET: $eth, netdev ${net:-none}"

# --- arm ----------------------------------------------------------------
if [[ "$MODE" == inject ]]; then
	read -r start < <(awk '$3 == "mlx5_core_create_tis" {print $1; exit}' /proc/kallsyms)
	[[ -n "$start" && "$start" != 0000000000000000 ]] \
		|| die "no address for mlx5_core_create_tis in /proc/kallsyms (kptr_restrict?)"
	# End: the next symbol above it.
	end="$(awk -v s="$start" '{print $1}' /proc/kallsyms | sort -u \
		| awk -v s="$start" '$1 > s {print; exit}')"
	[[ -n "$end" ]] || die "no symbol after mlx5_core_create_tis"
	echo 0 >"$FS/probability"
	echo 1 >"$FS/interval"
	echo 0 >"$FS/space"
	echo 1 >"$FS/times"
	echo N >"$FS/ignore-gfp-wait"
	echo N >"$FS/task-filter"
	echo 2 >"$FS/verbose"
	echo 32 >"$FS/stacktrace-depth"
	echo "0x$start" >"$FS/require-start"
	echo "0x$end" >"$FS/require-end"
	echo 100 >"$FS/probability"
	note "failslab armed: one failure with mlx5_core_create_tis [0x$start, 0x$end) on the stack"
fi

# --- PM cycle -----------------------------------------------------------
echo "$MARK suspend" >/dev/kmsg
echo devices >/sys/power/pm_test
echo "== suspend/resume of all devices (pm_test=devices, ~5 s)"
echo mem >/sys/power/state
rc=$?
echo "$old_pm_test" >/sys/power/pm_test
[[ "$MODE" == inject ]] && echo 0 >"$FS/probability"
echo "$MARK resumed" >/dev/kmsg
note "write to /sys/power/state returned $rc"

since() { dmesg | awk -v m="$MARK $1" '{l[NR] = $0} index($0, m) {s = NR} END {for (i = s; s && i <= NR; i++) print l[i]}'; }
since suspend | grep -E "FAULT_INJECTION|alloc tises|failed to load|mlx5_resume|pci_pm_resume|returns -|mlx5_core $TARGET" \
	| grep -v "$MARK" | sed 's/^/    /' | tail -30

if [[ "$MODE" == dry ]]; then
	since suspend | grep -q "failed to load" && die "mlx5 failed to resume without a fault"
	[[ -e "$AUX/devices/$eth/driver" ]] || die "mlx5e no longer bound"
	note "netdev after resume: $(ls /sys/bus/pci/devices/$TARGET/net 2>/dev/null || echo none)"
	echo "PASS (dry): suspend/resume callbacks ran and mlx5 came back"
	exit 0
fi

since suspend | grep -q "FAULT_INJECTION: forcing a failure" || die "the fault was not injected"
since suspend | grep -q "alloc tises failed" \
	|| die "the fault did not hit mlx5e_create_tises (see the stack above)"
note "mlx5e resume failed in mlx5e_create_tises, as intended"

# --- the second destroy -------------------------------------------------
echo "$MARK unbind" >/dev/kmsg
echo "== unbind $TARGET from mlx5_core (mlx5e remove destroys the mdev resources again)"
echo "$TARGET" >"$PCI/unbind"
sleep 1
echo "$MARK done" >/dev/kmsg
bad="$(since unbind | grep -E "KASAN|double-free|use-after-free|BUG:|Oops|general protection|refcount_t|list_del corruption|DEALLOC_(PD|TRANSPORT_DOMAIN)" | grep -v "$MARK")"
if [[ -n "$bad" ]]; then
	echo "$bad" | sed 's/^/    /' | head -40
	echo "BUG REPRODUCED: the unbind after a failed resume destroyed the mdev resources twice"
	exit 1
fi
echo "PASS: unbind after the failed resume was clean"
