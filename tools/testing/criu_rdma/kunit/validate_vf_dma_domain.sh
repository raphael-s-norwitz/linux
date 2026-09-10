#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# validate_vf_dma_domain.sh - prove a VF can be moved from IOMMU passthrough
# (identity default domain, e.g. under iommu=pt) to a managed DMA-IOMMU domain
# while it is driver-unbound, and that the DMA-IOVA arena path is then live on
# it. This is the provisioning precondition for vfmig arena-backed tracking:
# an orchestrator performs the same "echo DMA > .../type" step when it creates
# a VF destined to be migrated.
#
# What it does, all reversible:
#   1. create one VF with driver autoprobe disabled (so it stays unbound),
#   2. resolve its IOMMU group and record the original domain type,
#   3. echo DMA into the group's type and confirm the switch,
#   4. (optional) load arena_kunit against the VF: with the VF now on the
#      managed path the 5 arena_device subtests must RUN (not skip) and pass,
#      which is direct proof use_dma_iommu() is true and reserve/link/probe/
#      flush all work on real hardware,
#   5. restore the original domain type and tear the VF down.
#
# Usage:
#   sudo ./validate_vf_dma_domain.sh
#   sudo PF=0000:08:00.0 VF_INDEX=0 RUN_KUNIT=1 ./validate_vf_dma_domain.sh
#   sudo KEEP=1 ./validate_vf_dma_domain.sh      # leave VF up on DMA domain
#
# Env:
#   PF          PF BDF (default 0000:08:00.0)
#   VF_INDEX    virtfn index to use (default 0 => first VF)
#   RUN_KUNIT   1 to drive arena_kunit against the VF (default 1)
#   ARENA_KO    path to arena_kunit.ko (default: next to this script)
#   KEEP        1 to keep the VF + DMA domain after the run (default 0)

set -u

PF=${PF:-0000:08:00.0}
VF_INDEX=${VF_INDEX:-0}
RUN_KUNIT=${RUN_KUNIT:-1}
KEEP=${KEEP:-0}
SELF_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ARENA_KO=${ARENA_KO:-$SELF_DIR/arena_kunit.ko}

PF_PATH=/sys/bus/pci/devices/$PF
fail() { echo "FAIL: $*" >&2; exit 1; }
info() { echo "== $*"; }

[ "$(id -u)" -eq 0 ] || fail "must run as root (sysfs writes + insmod)"
[ -d "$PF_PATH" ] || fail "PF $PF not found"
[ -e "$PF_PATH/sriov_numvfs" ] || fail "$PF is not SR-IOV capable"

# --- state we must restore ------------------------------------------------
ORIG_NUMVFS=$(cat "$PF_PATH/sriov_numvfs")
ORIG_AUTOPROBE=$(cat "$PF_PATH/sriov_drivers_autoprobe")
VF_BDF=""
GRP=""
ORIG_TYPE=""

cleanup() {
	set +e
	if [ "$KEEP" = "1" ]; then
		info "KEEP=1: leaving VF $VF_BDF on group $GRP type=$(cat /sys/kernel/iommu_groups/$GRP/type 2>/dev/null)"
		return
	fi
	if [ -n "$GRP" ] && [ -n "$ORIG_TYPE" ]; then
		# Only restorable while unbound; VF was never bound in this run.
		echo "$ORIG_TYPE" > "/sys/kernel/iommu_groups/$GRP/type" 2>/dev/null \
			&& info "restored group $GRP type -> $ORIG_TYPE"
	fi
	# Return SR-IOV to how we found it.
	if [ "$(cat "$PF_PATH/sriov_numvfs")" != "$ORIG_NUMVFS" ]; then
		echo 0 > "$PF_PATH/sriov_numvfs" 2>/dev/null
		[ "$ORIG_NUMVFS" != "0" ] && echo "$ORIG_NUMVFS" > "$PF_PATH/sriov_numvfs" 2>/dev/null
		info "restored sriov_numvfs -> $ORIG_NUMVFS"
	fi
	echo "$ORIG_AUTOPROBE" > "$PF_PATH/sriov_drivers_autoprobe" 2>/dev/null
}
trap cleanup EXIT

# --- 1. create one unbound VF ---------------------------------------------
[ "$ORIG_NUMVFS" = "0" ] || fail "PF already has $ORIG_NUMVFS VFs; run on a PF with sriov_numvfs=0"
info "disabling driver autoprobe so the new VF stays unbound"
echo 0 > "$PF_PATH/sriov_drivers_autoprobe" || fail "cannot write sriov_drivers_autoprobe"
info "creating $((VF_INDEX + 1)) VF(s) on $PF"
echo $((VF_INDEX + 1)) > "$PF_PATH/sriov_numvfs" || fail "cannot create VFs"
udevadm settle 2>/dev/null

VF_LINK=$PF_PATH/virtfn$VF_INDEX
[ -e "$VF_LINK" ] || fail "virtfn$VF_INDEX did not appear"
VF_BDF=$(basename "$(readlink -f "$VF_LINK")")
info "VF is $VF_BDF"

# --- 2. resolve group + confirm unbound -----------------------------------
VF_SYS=/sys/bus/pci/devices/$VF_BDF
if [ -L "$VF_SYS/driver" ]; then
	DRV=$(basename "$(readlink -f "$VF_SYS/driver")")
	info "VF unexpectedly bound to $DRV; unbinding"
	echo "$VF_BDF" > "/sys/bus/pci/drivers/$DRV/unbind" || fail "unbind failed"
fi
GRP=$(basename "$(readlink -f "$VF_LINK/iommu_group")")
[ -n "$GRP" ] || fail "VF has no iommu_group (is the IOMMU on?)"
ORIG_TYPE=$(cat "/sys/kernel/iommu_groups/$GRP/type")
info "VF $VF_BDF is in iommu_group $GRP, type=$ORIG_TYPE"

# --- 3. flip the group to a managed DMA domain ----------------------------
if [ "$ORIG_TYPE" = "DMA" ] || [ "$ORIG_TYPE" = "DMA-FQ" ]; then
	info "group already on a managed DMA domain; nothing to switch"
else
	info "switching group $GRP: $ORIG_TYPE -> DMA"
	echo DMA > "/sys/kernel/iommu_groups/$GRP/type" \
		|| fail "could not set group $GRP type=DMA (VF bound? multi-device group?)"
fi
NEW_TYPE=$(cat "/sys/kernel/iommu_groups/$GRP/type")
[ "$NEW_TYPE" = "DMA" ] || [ "$NEW_TYPE" = "DMA-FQ" ] \
	|| fail "group type is '$NEW_TYPE' after switch, expected DMA"
info "PASS: VF $VF_BDF is now on managed domain type=$NEW_TYPE"

# --- 4. prove the arena path is live on the VF ----------------------------
if [ "$RUN_KUNIT" = "1" ]; then
	[ -f "$ARENA_KO" ] || fail "arena_kunit.ko not found at $ARENA_KO (run make first)"
	info "driving arena_kunit against $VF_BDF"
	dmesg -C
	if ! insmod "$ARENA_KO" bdf="$VF_BDF"; then
		fail "insmod arena_kunit failed"
	fi
	RES=$(cat "/sys/kernel/debug/kunit/arena_device/results" 2>/dev/null)
	rmmod arena_kunit 2>/dev/null
	echo "$RES"
	# Any SKIP means the VF is still not on the DMA-IOMMU path -> switch bug.
	if echo "$RES" | grep -q "SKIP"; then
		fail "arena_device subtests SKIPPED -> VF not on DMA-IOMMU path after switch"
	fi
	echo "$RES" | grep -q "^# Totals: .*fail:0" \
		|| fail "arena_device reported failures (see above / dmesg)"
	info "PASS: arena_device ran and passed on $VF_BDF"
fi

info "RESULT: PASS (VF DMA-domain provisioning validated)"
