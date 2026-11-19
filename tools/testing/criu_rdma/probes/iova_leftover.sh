#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Check that IOVAs a bound driver used in a VF's DMA domain do not get
# in the way of re-tracking the VF, with the same window or another.
#
# Reserving a window above 4 GiB turns off dma-iommu's "below 4 GiB
# first" policy for the VF, so once it is bound the driver's ordinary
# DMA mappings are allocated top-down from just below the window, and
# the ones it frees stay in the domain's IOVA caches. Those caches are
# flushed when an allocation, here the window's, would otherwise fail,
# so only mappings still live after the unbind (a leak) can block it.
#
# On PF $PF, VF $VF unbound in a fresh DMA domain (its IOMMU group
# switched to identity and back), so nothing an earlier bind mapped is
# still in it:
#   0. if PRETRACK=1, track with $FIRST and untrack before any bind,
#      as a restore does after a refused LOAD
#   1. track with $FIRST, bind mlx5_core, bring the netdev up if
#      NETDEV_UP=1, unbind, untrack
#   2. re-track with $FIRST: must succeed
#   3. untrack, track with $SECOND: must succeed
#   4. if 3 failed, to tell a leak in this domain from anything else:
#      switch the VF's IOMMU group to identity and back to $GROUP_TYPE,
#      which gives it a fresh domain, and track with $SECOND again
#
# With GROUP_TYPE=DMA-FQ the driver's unmapped IOVAs also wait in the
# domain's flush queue, up to 1 s under a caching-mode virtual IOMMU,
# where the allocator's own retry does not reach; steps 2 and 3 follow
# the unbind at once, and the probe reports whether SET_TRACKED had to
# wait for the queue to drain.
#
#   sudo tools/testing/criu_rdma/probes/iova_leftover.sh
#   sudo FIRST=36 SECOND=39 tools/testing/criu_rdma/probes/iova_leftover.sh
#   sudo GROUP_TYPE=DMA-FQ tools/testing/criu_rdma/probes/iova_leftover.sh
#
# Environment:
#   PF           PF BDF (default 0000:08:00.0)
#   VF           VF index (default 0)
#   FIRST        iova_bits while bound (default 39)
#   SECOND       iova_bits to re-track with (default 38)
#   NETDEV_UP    1 (default): bring the VF's netdev up while bound, so
#                its RX buffers are DMA-mapped
#   PRETRACK     1 (default): run step 0
#   GROUP_TYPE   the VF's IOMMU group type: DMA (default, strict) or
#                DMA-FQ (lazy, with a flush queue)
#   TOOL         mlx5_vfmig CLI (default ../tools/mlx5_vfmig)
#   BIND_TIMEOUT seconds to wait for the VF probe (default 90)
#
# Functional only; no timing. SR-IOV on $PF is reset at the end.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

PF="${PF:-0000:08:00.0}"
VF="${VF:-0}"
FIRST="${FIRST:-39}"
SECOND="${SECOND:-38}"
NETDEV_UP="${NETDEV_UP:-1}"
PRETRACK="${PRETRACK:-1}"
GROUP_TYPE="${GROUP_TYPE:-DMA}"
TOOL="${TOOL:-$HERE/../tools/mlx5_vfmig}"
BIND_TIMEOUT="${BIND_TIMEOUT:-90}"

PF_DEV="/sys/bus/pci/devices/$PF"
RUN_ID="$$-$(date +%s)"
note() { printf '  %s\n' "$*"; }
sect() { printf '\n== %s ==\n' "$*"; }
die()  { echo "RESULT: FAIL: $*"; exit 1; }

mark() { echo "criu_rdma iova_leftover $RUN_ID: $1" >/dev/kmsg; }
klog_since() { dmesg | sed -n "/criu_rdma iova_leftover $RUN_ID: $1\$/,\$p"; }
vfmig_lines() { klog_since "$1" | grep -E 'vfmig_iova: vf [0-9]+ (IOVA window|could not)' | sed 's/^[^]]*] //'; }

teardown() {
	"$TOOL" "$PF" set_tracked "$VF" 0 >/dev/null 2>&1 || true
	echo 0 >"$PF_DEV/sriov_numvfs" 2>/dev/null || true
	echo 1 >"$PF_DEV/sriov_drivers_autoprobe" 2>/dev/null || true
}

set_type() { echo "$1" >"/sys/bus/pci/devices/$BDF/iommu_group/type"; }

# Track with @1; print the kernel's window lines; return set_tracked's exit.
track() {
	local tag="track-$1-$((++SEQ))" rc=0
	mark "$tag"
	"$TOOL" "$PF" set_tracked "$VF" 1 "$1" >/dev/null 2>&1 || rc=$?
	vfmig_lines "$tag" | sed 's/^/    /'
	return "$rc"
}

untrack() {
	"$TOOL" "$PF" set_tracked "$VF" 0 >/dev/null || die "untrack vf $VF failed"
}

bind_vf() {
	local started bg
	echo mlx5_core >"/sys/bus/pci/devices/$BDF/driver_override"
	( echo "$BDF" >/sys/bus/pci/drivers/mlx5_core/bind ) >/dev/null 2>&1 &
	bg=$!
	started=$(date +%s)
	while [[ ! -e "/sys/bus/pci/devices/$BDF/driver" ]]; do
		kill -0 "$bg" 2>/dev/null || break
		(( $(date +%s) - started < BIND_TIMEOUT )) ||
			die "$BDF probe still pending after ${BIND_TIMEOUT}s; a reboot may be needed"
		sleep 0.5
	done
	wait "$bg" 2>/dev/null || true
	[[ -e "/sys/bus/pci/devices/$BDF/driver" ]]
}

# ---------------------------------------------------------------------------
sect "Preconditions"
[[ "$EUID" -eq 0 ]] || die "must run as root"
[[ -x "$TOOL" ]] || die "missing $TOOL -- make -C $HERE/.."
[[ -d "$PF_DEV" ]] || die "PF $PF not present"
[[ -e "/dev/mlx5_vfmig/$PF" ]] || die "no /dev/mlx5_vfmig/$PF"
for b in "$FIRST" "$SECOND"; do
	(( b >= 34 && b <= 39 )) || die "iova_bits $b outside 34..39"
done
(( FIRST != SECOND )) || die "FIRST and SECOND must differ"
[[ "$GROUP_TYPE" == DMA || "$GROUP_TYPE" == DMA-FQ ]] || die "GROUP_TYPE must be DMA or DMA-FQ"
SYSFS="/sys/class/mlx5_vfmig/mlx5_vfmig!$PF/iova_bits"
note "kernel $(uname -r), PF $PF, vf $VF, $FIRST then $SECOND bits"
note "NETDEV_UP=$NETDEV_UP, PRETRACK=$PRETRACK, GROUP_TYPE=$GROUP_TYPE"
[[ -r "$SYSFS" ]] && note "PF iova_bits: $(cat "$SYSFS")"
SEQ=0
trap teardown EXIT

sect "Provision"
echo 0 >"$PF_DEV/sriov_numvfs"
echo 0 >"$PF_DEV/sriov_drivers_autoprobe"
echo $((VF + 1)) >"$PF_DEV/sriov_numvfs" || die "could not create $((VF + 1)) VFs"
BDF="$(basename "$(readlink "$PF_DEV/virtfn$VF")")"
[[ "$(cat "/sys/bus/pci/devices/$BDF/iommu_group/type")" == identity ]] ||
	set_type identity || die "could not switch $BDF IOMMU group to identity"
set_type "$GROUP_TYPE" || die "could not switch $BDF IOMMU group to $GROUP_TYPE"
note "vf $VF $BDF, fresh IOMMU domain of type $(cat "/sys/bus/pci/devices/$BDF/iommu_group/type")"

if [[ "$PRETRACK" == 1 ]]; then
	sect "0. Track with $FIRST and untrack, before any bind"
	track "$FIRST" || die "track vf $VF with $FIRST failed"
	untrack
	note "ok"
fi

sect "1. Track with $FIRST, bind, unbind, untrack"
track "$FIRST" || die "track vf $VF with $FIRST failed"
"$TOOL" "$PF" enable_migratable "$VF" >/dev/null || die "enable_migratable vf $VF failed"
bind_vf || die "vf $VF did not bind; see dmesg"
NETDEV="$(ls "/sys/bus/pci/devices/$BDF/net" 2>/dev/null | head -1 || true)"
if [[ "$NETDEV_UP" == 1 && -n "$NETDEV" ]]; then
	ip link set dev "$NETDEV" up || die "could not bring $NETDEV up"
	note "$NETDEV up"
fi
dmesg | grep "$BDF: Using [0-9]*-bit DMA addresses" | tail -1 | sed 's/^[^]]*] /  /'
echo "$BDF" >/sys/bus/pci/drivers/mlx5_core/unbind || die "unbind $BDF failed"
echo >"/sys/bus/pci/devices/$BDF/driver_override"
untrack
note "bound, unbound and untracked"

sect "2. Re-track with $FIRST (same window)"
track "$FIRST" || die "re-track with the same window $FIRST failed"
note "ok"
untrack

sect "3. Track with $SECOND"
mark step3
if track "$SECOND"; then
	note "ok"
	untrack
	if [[ "$GROUP_TYPE" == DMA-FQ ]]; then
		log="$(klog_since step3)"
		[[ "$log" == *"retrying once the DMA-FQ domain's flush queue"* ]] &&
			waited=yes || waited=no
		note "waited for the flush queue: $waited"
	fi
	echo
	echo "RESULT: PASS"
	exit 0
fi
note "refused (above)"

sect "4. Fresh domain (identity -> $GROUP_TYPE), track with $SECOND"
set_type identity || die "could not switch $BDF IOMMU group to identity"
set_type "$GROUP_TYPE" || die "could not switch $BDF IOMMU group back to $GROUP_TYPE"
note "IOMMU group type $(cat "/sys/bus/pci/devices/$BDF/iommu_group/type")"
if track "$SECOND"; then
	untrack
	die "IOVAs left mapped after the unbind took the $SECOND-bit window" \
		"(a fresh domain clears them)"
fi
die "the $SECOND-bit window is refused even on a fresh domain"
