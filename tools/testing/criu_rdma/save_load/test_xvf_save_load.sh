#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Cross-VF SAVE/LOAD round-trip on one PF, with no CRIU involved.
#
# SAVE_VHCA_STATE on VF $SRC_VF, recreate the VF set, then
# LOAD_VHCA_STATE onto VF $DST_VF and bind it. The image carries only
# the driver's own kernel buffers (cmd ring, FW pages, EQs, WQ frags,
# doorbells), so this exercises the deterministic-IOVA replay and the
# firmware LOAD, not user verbs objects.
#
# Each stage is reported separately:
#   arena     both VFs reserve the same IOVA window ($EXPECT_BASE)
#   replay    the LOAD stream's HOST_PAGE records replay into $DST_VF
#   fw_load   the probe-time LOAD_VHCA_STATE lands on $DST_VF's vHCA
#   probe     $DST_VF binds to mlx5_core with no allocator drift and
#             brings up its ib device
#   health    no fatal health event on $DST_VF for $SETTLE seconds
#             after the probe
#
# A fatal health event during the final SR-IOV teardown is reported on
# its own "teardown" line, for information only.
#
# SRC_VF == DST_VF runs the same-slot case through the same steps.
#
# Usage (as root):
#   PF=0000:08:00.0 tools/testing/criu_rdma/save_load/test_xvf_save_load.sh
#   PF=0000:08:00.0 SRC_VF=0 DST_VF=0 .../test_xvf_save_load.sh
#
# Environment:
#   PF           PF BDF (default 0000:08:00.0)
#   SRC_VF       VF index the state is saved from (default 0)
#   DST_VF       VF index the state is loaded onto (default 1)
#   TOOL         mlx5_vfmig CLI (default ../tools/mlx5_vfmig)
#   BLOB         image path (default: inside the work dir)
#   EXPECT_BASE  arena base every VF should reserve (default 0x100000000)
#   SET_DMA      1 (default): switch an identity-domain VF's IOMMU group
#                to DMA before tracking it; 0: fail instead
#   BIND_TIMEOUT seconds to wait for a VF probe (default 90)
#   SETTLE       seconds to watch the restored VF's health (default 10;
#                the mlx5 health poll runs every 2 s)
#   KEEP         1: leave the VFs and the work dir in place on exit
#
# Exits 0 on PASS, non-zero on FAIL.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

PF="${PF:-0000:08:00.0}"
SRC_VF="${SRC_VF:-0}"
DST_VF="${DST_VF:-1}"
TOOL="${TOOL:-$HERE/../tools/mlx5_vfmig}"
EXPECT_BASE="${EXPECT_BASE:-0x100000000}"
SET_DMA="${SET_DMA:-1}"
BIND_TIMEOUT="${BIND_TIMEOUT:-90}"
SETTLE="${SETTLE:-10}"
KEEP="${KEEP:-0}"

WORKDIR="$(mktemp -d /tmp/vfmig-xvf-XXXXXX)"
BLOB="${BLOB:-$WORKDIR/vf.blob}"
PF_DEV="/sys/bus/pci/devices/$PF"
CDEV="/dev/mlx5_vfmig/$PF"
NUM_VFS=$(( (SRC_VF > DST_VF ? SRC_VF : DST_VF) + 1 ))
RUN_ID="$$-$(date +%s)"
SRIOV_TOUCHED=0

declare -A RESULT=()
STAGES=(arena replay fw_load probe health)
TEARDOWN_NOTE=""

# mlx5 health poller reporting a dead or disabled function.
HEALTH_ERRS='Fatal error [0-9]+ detected|firmware internal error|NIC IFC still'

die()  { echo "FAIL: $*" >&2; exit 1; }
note() { printf '  %s\n' "$*"; }
sect() { printf '\n== %s ==\n' "$*"; }

summary() {
	local s
	echo
	echo "== Summary (SRC_VF=$SRC_VF -> DST_VF=$DST_VF on $PF) =="
	for s in "${STAGES[@]}"; do
		printf '  %-8s %s\n' "$s" "${RESULT[$s]:-not reached}"
	done
	[[ -n "$TEARDOWN_NOTE" ]] && printf '  %-8s %s\n' teardown "$TEARDOWN_NOTE"
	return 0
}

cleanup() {
	local line
	if [[ "$KEEP" == "1" ]]; then
		summary
		echo "  [keep] VFs left provisioned; work dir $WORKDIR"
		return
	fi
	if [[ "$SRIOV_TOUCHED" == "1" ]]; then
		mark teardown 2>/dev/null || true
		echo 0 >"$PF_DEV/sriov_numvfs" 2>/dev/null || true
		echo 1 >"$PF_DEV/sriov_drivers_autoprobe" 2>/dev/null || true
		line="$(klog_since teardown | grep -E -m1 "$HEALTH_ERRS" || true)"
		if [[ -n "$line" ]]; then
			TEARDOWN_NOTE="INFO: fatal health event during SR-IOV disable: ${line#*] }"
		else
			TEARDOWN_NOTE="clean"
		fi
	fi
	summary
	rm -rf "$WORKDIR"
}
trap cleanup EXIT

fail_stage() {
	RESULT[$1]="FAIL: $2"
	die "$1: $2"
}

pass_stage() {
	RESULT[$1]="PASS${2:+ ($2)}"
	note "$1: PASS${2:+ ($2)}"
}

# Kernel log since the named marker. Markers go through /dev/kmsg so the
# harness never has to clear the ring buffer.
mark() { echo "criu_rdma xvf $RUN_ID: $1" >/dev/kmsg; }
klog_since() {
	dmesg | sed -n "/criu_rdma xvf $RUN_ID: $1\$/,\$p"
}

vf_bdf() { basename "$(readlink "$PF_DEV/virtfn$1")"; }

wait_for() {
	local i
	for i in $(seq 1 100); do
		[[ -e "$1" ]] && return 0
		sleep 0.1
	done
	return 1
}

vhca_of() { "$TOOL" "$PF" get_vhca_id "$1" | awk '{print $NF}'; }

iommu_group_of() { basename "$(readlink "/sys/bus/pci/devices/$1/iommu_group")"; }

# The arena needs a managed DMA domain; a VF that inherits an identity
# default domain (e.g. iommu=pt) is refused by SET_TRACKED.
ensure_dma_domain() {
	local bdf="$1" type_file type
	type_file="/sys/bus/pci/devices/$bdf/iommu_group/type"
	[[ -e "$type_file" ]] || die "$bdf has no IOMMU group (IOMMU disabled?)"
	type="$(cat "$type_file")"
	case "$type" in
	DMA|DMA-FQ)
		return 0 ;;
	identity)
		[[ "$SET_DMA" == "1" ]] \
			|| die "$bdf IOMMU group is identity (passthrough); set SET_DMA=1 or boot without iommu=pt"
		echo DMA >"$type_file" \
			|| die "could not switch $bdf IOMMU group to DMA"
		note "$bdf: IOMMU group switched identity -> $(cat "$type_file")"
		;;
	*)
		die "$bdf IOMMU group type '$type' is not a managed DMA domain" ;;
	esac
}

# Bind with a bounded wait: a wedged probe holds the device lock and never
# returns, so the bind write runs in the background.
bind_vf() {
	local bdf="$1" started bg
	echo mlx5_core >"/sys/bus/pci/devices/$bdf/driver_override"
	( echo "$bdf" >/sys/bus/pci/drivers/mlx5_core/bind ) >/dev/null 2>&1 &
	bg=$!
	started=$(date +%s)
	while :; do
		if [[ -e "/sys/bus/pci/devices/$bdf/driver" ]]; then
			wait "$bg" 2>/dev/null || true
			return 0
		fi
		if ! kill -0 "$bg" 2>/dev/null; then
			sleep 0.2
			[[ -e "/sys/bus/pci/devices/$bdf/driver" ]] && return 0
			return 1
		fi
		if (( $(date +%s) - started >= BIND_TIMEOUT )); then
			echo "ERROR: $bdf probe still pending after ${BIND_TIMEOUT}s;" \
			     "it is likely wedged and a reboot may be needed" >&2
			return 2
		fi
		sleep 0.5
	done
}

ibdev_of() {
	local ib
	for ib in /sys/class/infiniband/*; do
		[[ -e "$ib/device" ]] || continue
		[[ "$(basename "$(readlink "$ib/device")")" == "$1" ]] \
			&& { basename "$ib"; return 0; }
	done
	return 1
}

# Reset the VF set and leave every VF unbound.
provision_vfs() {
	SRIOV_TOUCHED=1
	echo 0 >"$PF_DEV/sriov_numvfs"
	echo 0 >"$PF_DEV/sriov_drivers_autoprobe"
	echo "$NUM_VFS" >"$PF_DEV/sriov_numvfs"
	wait_for "$PF_DEV/virtfn$((NUM_VFS - 1))" \
		|| die "virtfn$((NUM_VFS - 1)) did not appear"
}

# SET_TRACKED reserves the arena; check the window it logged.
track_vf() {
	local vf="$1" tag="track-$1-$2" line
	ensure_dma_domain "$(vf_bdf "$vf")"
	mark "$tag"
	"$TOOL" "$PF" set_tracked "$vf" 1 >/dev/null \
		|| fail_stage arena "SET_TRACKED vf $vf failed (see dmesg)"
	line="$(klog_since "$tag" | grep -m1 "vfmig_iova: vf $vf arena reserved" || true)"
	[[ -n "$line" ]] || fail_stage arena "no arena reservation logged for vf $vf"
	[[ "$line" == *"kernel slots [$EXPECT_BASE,"* ]] \
		|| fail_stage arena "vf $vf arena not at $EXPECT_BASE: ${line#*vfmig_iova: }"
}

# Allocator complaints that mean replay or the restored probe went wrong.
ALLOC_ERRS='replay_page\(|replay_external\(|replay: wire claims|DRIFT|replay/alloc size mismatch|exhausted at cursor|could not reserve IOVA window|not SET_TRACKED|manifest CRC mismatch'

# ---------------------------------------------------------------------------
sect "Preconditions"
[[ "$EUID" -eq 0 ]] || die "must run as root"
[[ -x "$TOOL" ]] || die "missing $TOOL -- make -C $HERE/.."
[[ -d "$PF_DEV" ]] || die "PF $PF not present"
[[ -e "$CDEV" ]] || die "missing $CDEV (mlx5_core without vfmig?)"
total="$(cat "$PF_DEV/sriov_totalvfs")"
(( total >= NUM_VFS )) || die "PF $PF supports $total VFs, need $NUM_VFS"
note "PF=$PF SRC_VF=$SRC_VF DST_VF=$DST_VF NUM_VFS=$NUM_VFS kernel=$(uname -r)"

# ---------------------------------------------------------------------------
sect "Source: provision, track and bind VF $SRC_VF"
provision_vfs
SRC_BDF="$(vf_bdf "$SRC_VF")"
track_vf "$SRC_VF" src
"$TOOL" "$PF" enable_migratable "$SRC_VF" >/dev/null
mark bind-src
rc=0; bind_vf "$SRC_BDF" || rc=$?
(( rc == 0 )) || die "source VF $SRC_BDF did not bind (rc=$rc); see dmesg"
SRC_VHCA="$(vhca_of "$SRC_VF")"
note "source VF $SRC_VF: bdf=$SRC_BDF vhca_id=$SRC_VHCA"
sleep 1

sect "SAVE from VF $SRC_VF"
mark save
"$TOOL" "$PF" save_vhca_state "$SRC_VF" "$BLOB"
SAVE_BYTES="$(stat -c %s "$BLOB")"
(( SAVE_BYTES > 16 )) || die "image suspiciously small: $SAVE_BYTES bytes"
klog_since save | grep -q "vfmig: saved vf $SRC_VF (vhca_id" \
	|| die "kernel did not log a completed SAVE for vf $SRC_VF"
note "saved $SAVE_BYTES bytes"

# ---------------------------------------------------------------------------
sect "Destination: recreate VFs, track VF $DST_VF unbound"
provision_vfs
DST_BDF="$(vf_bdf "$DST_VF")"
SRC_GROUP="$(iommu_group_of "$(vf_bdf "$SRC_VF")")"
DST_GROUP="$(iommu_group_of "$DST_BDF")"
note "IOMMU groups: vf $SRC_VF -> $SRC_GROUP, vf $DST_VF -> $DST_GROUP"
track_vf "$DST_VF" dst
if [[ "$SRC_VF" != "$DST_VF" ]]; then
	[[ "$SRC_GROUP" != "$DST_GROUP" ]] \
		|| fail_stage arena "vf $SRC_VF and vf $DST_VF share IOMMU group $SRC_GROUP; identical windows cannot coexist"
	# Hold the same window on the source slot as well, so both VFs
	# reserve [EXPECT_BASE, ...) at once.
	track_vf "$SRC_VF" dst-peer
	pass_stage arena "vf $SRC_VF and vf $DST_VF both at $EXPECT_BASE"
else
	pass_stage arena "vf $DST_VF at $EXPECT_BASE"
fi
"$TOOL" "$PF" enable_migratable "$DST_VF" >/dev/null
[[ ! -e "/sys/bus/pci/devices/$DST_BDF/driver" ]] \
	|| die "destination VF $DST_BDF is bound before LOAD"
DST_VHCA="$(vhca_of "$DST_VF")"
note "destination VF $DST_VF: bdf=$DST_BDF vhca_id=$DST_VHCA (source was $SRC_VHCA)"

# ---------------------------------------------------------------------------
sect "LOAD onto VF $DST_VF (HOST_PAGE replay)"
mark load
if ! "$TOOL" "$PF" load_vhca_state "$DST_VF" "$BLOB"; then
	klog_since load | grep -E "vfmig" | tail -20 >&2 || true
	fail_stage replay "LOAD_VHCA_STATE stream rejected"
fi
LOAD_LOG="$(klog_since load)"
grep -q "LOAD session opened for vf $DST_VF (vhca_id $DST_VHCA)" <<<"$LOAD_LOG" \
	|| fail_stage replay "LOAD session not opened on vf $DST_VF / vhca $DST_VHCA"
if grep -E "$ALLOC_ERRS" <<<"$LOAD_LOG" >&2; then
	fail_stage replay "allocator rejected the replay (above)"
fi
grep -q "staged [0-9]* bytes of LOAD state for vf $DST_VF (vhca_id $DST_VHCA)" <<<"$LOAD_LOG" \
	|| fail_stage replay "LOAD state not staged for vf $DST_VF"
pass_stage replay "staged on vf $DST_VF"

# ---------------------------------------------------------------------------
sect "Bind VF $DST_VF (probe applies LOAD_VHCA_STATE)"
mark bind-dst
rc=0; bind_vf "$DST_BDF" || rc=$?
BIND_LOG="$(klog_since bind-dst)"
if grep -q "LOAD_VHCA_STATE vf $DST_VF .* failed" <<<"$BIND_LOG"; then
	grep "LOAD_VHCA_STATE vf $DST_VF" <<<"$BIND_LOG" >&2
	fail_stage fw_load "firmware rejected LOAD_VHCA_STATE"
fi
if grep -q "applied [0-9]* bytes of LOAD state to vf $DST_VF (vhca_id $DST_VHCA)" <<<"$BIND_LOG"; then
	pass_stage fw_load "applied to vhca $DST_VHCA"
else
	fail_stage fw_load "no LOAD_VHCA_STATE applied to vf $DST_VF / vhca $DST_VHCA (bind rc=$rc)"
fi

(( rc == 0 )) || fail_stage probe "VF $DST_BDF did not bind (rc=$rc); see dmesg"
if grep -E "$ALLOC_ERRS" <<<"$BIND_LOG" >&2; then
	fail_stage probe "allocator drift during the restored probe (above)"
fi
DST_IBDEV=""
for _ in $(seq 1 100); do
	DST_IBDEV="$(ibdev_of "$DST_BDF" || true)"
	[[ -n "$DST_IBDEV" ]] && break
	sleep 0.1
done
[[ -n "$DST_IBDEV" ]] || fail_stage probe "no ib device appeared for $DST_BDF"
DST_NETDEV="$(ls "/sys/bus/pci/devices/$DST_BDF/net" 2>/dev/null | head -1 || true)"
pass_stage probe "ibdev=$DST_IBDEV netdev=${DST_NETDEV:-none}"

# ---------------------------------------------------------------------------
sect "Watch VF $DST_VF health for ${SETTLE}s"
sleep "$SETTLE"
[[ -e "/sys/bus/pci/devices/$DST_BDF/driver" ]] \
	|| fail_stage health "VF $DST_BDF lost its driver while settling"
line="$(klog_since bind-dst | grep -E -m1 "$HEALTH_ERRS" || true)"
[[ -z "$line" ]] || fail_stage health "${line#*] }"
pass_stage health "no fatal health event in ${SETTLE}s"

echo
echo "PASS"
