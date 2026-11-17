#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Cycle SET_TRACKED on and off across VFs and window sizes, and check
# every reservation lands exactly where its iova_bits says:
# [2^(bits-1), 2^bits), e.g. 39 -> [256 GiB, 512 GiB),
# 38 -> [128 GiB, 256 GiB), 34 -> [8 GiB, 16 GiB).
#
# Flow, on PF $PF with $NUMVFS unbound VFs in DMA domains:
#   edges    iova_bits out of range is refused (EINVAL); re-tracking with
#            the same window is a no-op, 0 and 39 being the same window;
#            re-tracking with a different window is refused (EBUSY).
#   rounds   $ITERS rounds. Each round tracks every VF, VF j with the
#            ((round + j) mod n)th entry of $BITS, so VFs hold different
#            windows at once and each VF sees every size over the run.
#            For each VF: the logged window, the kernel slot and USER_PAGE
#            bounds, and QUERY_VF's tracked bit. Then untracks every VF
#            and checks the release and tracked=0.
#   report   Per iova_bits: reservations checked, the window, and the
#            IOMMU aperture end and bus DMA limit the kernel logged.
#
#   sudo tools/testing/criu_rdma/probes/iova_track_cycle.sh
#   sudo ITERS=200 NUMVFS=8 tools/testing/criu_rdma/probes/iova_track_cycle.sh
#   sudo BITS="39 38" tools/testing/criu_rdma/probes/iova_track_cycle.sh
#
# Environment:
#   PF        PF BDF (default 0000:08:00.0)
#   NUMVFS    VFs to create (default 4)
#   ITERS     rounds (default 50)
#   BITS      iova_bits values to rotate through (default
#             "0 39 38 37 36 35 34"; 0 is the kernel default, 39)
#   TOOL      mlx5_vfmig CLI (default ../tools/mlx5_vfmig)
#   SET_DMA   1 (default): switch identity-domain VFs' IOMMU groups to
#             DMA; 0: fail instead
#   KEEP      1: leave the VFs in place on exit
#
# Functional only; no timing. SR-IOV on $PF is reset at the end.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

PF="${PF:-0000:08:00.0}"
NUMVFS="${NUMVFS:-4}"
ITERS="${ITERS:-50}"
BITS="${BITS:-0 39 38 37 36 35 34}"
TOOL="${TOOL:-$HERE/../tools/mlx5_vfmig}"
SET_DMA="${SET_DMA:-1}"
KEEP="${KEEP:-0}"

PF_DEV="/sys/bus/pci/devices/$PF"
RUN_ID="$$-$(date +%s)"
WORKDIR="$(mktemp -d /var/tmp/iova_track_cycle.XXXXXX)"
STATS="$WORKDIR/stats"
BAD='could not reserve IOVA window|is out of reach|IOVA window reserved at|cannot take a [0-9]+-bit DMA mask|DMAR: .*fault|AMD-Vi: .*fault|arm-smmu.*fault|WARNING: CPU|BUG: '

read -r -a BITS_A <<<"$BITS"
NFAIL=0
NCHECK=0
SEQ=0

note() { printf '  %s\n' "$*"; }
sect() { printf '\n== %s ==\n' "$*"; }
die()  { echo "FAIL: $*" >&2; exit 1; }
fail() { NFAIL=$((NFAIL + 1)); echo "  FAIL: $*"; }

mark() { echo "criu_rdma iova_cycle $RUN_ID: $1" >/dev/kmsg; }
klog_since() { dmesg | sed -n "/criu_rdma iova_cycle $RUN_ID: $1\$/,\$p"; }

vf_bdf() { basename "$(readlink "$PF_DEV/virtfn$1")"; }
eff_bits() { echo $(($1 ? $1 : 39)); }
hex() { printf '0x%x' "$1"; }

ensure_dma_domain() {
	local bdf="$1" type_file type
	type_file="/sys/bus/pci/devices/$bdf/iommu_group/type"
	[[ -e "$type_file" ]] || die "$bdf has no IOMMU group (IOMMU disabled?)"
	type="$(cat "$type_file")"
	case "$type" in
	DMA|DMA-FQ) ;;
	identity)
		[[ "$SET_DMA" == 1 ]] ||
			die "$bdf IOMMU group is identity; set SET_DMA=1 or boot without iommu=pt"
		echo DMA >"$type_file" || die "could not switch $bdf IOMMU group to DMA"
		;;
	*) die "$bdf IOMMU group type '$type' is not a managed DMA domain" ;;
	esac
}

tracked_of() {
	"$TOOL" "$PF" query_vf "$1" | sed -n 's/.* tracked=\([01]\) .*/\1/p'
}

# Run set_tracked; print "rc<TAB>output".
st() {
	local out rc=0
	out="$("$TOOL" "$PF" set_tracked "$@" 2>&1)" || rc=$?
	printf '%s\t%s\n' "$rc" "$out"
}

# Fail if anything in $BAD was logged since mark @1.
check_clean() {
	local line
	line="$(klog_since "$1" | grep -E -m1 "$BAD" || true)"
	[[ -z "$line" ]] || fail "$2: kernel logged: ${line#*] }"
}

# Track @vf with @bits and check every bound the kernel logged.
track_check() {
	local vf="$1" bits="$2" tag res rc out b base end log win slots
	b="$(eff_bits "$bits")"
	base=$((1 << (b - 1)))
	end=$((1 << b))
	tag="s$((++SEQ))-track-vf$vf-b$bits"
	mark "$tag"
	res="$(st "$vf" 1 "$bits")"
	rc="${res%%$'\t'*}"
	out="${res#*$'\t'}"
	NCHECK=$((NCHECK + 1))
	if [[ "$rc" != 0 ]]; then
		fail "vf $vf bits $bits: set_tracked exited $rc: $out"
		check_clean "$tag" "vf $vf bits $bits"
		return 1
	fi
	log="$(klog_since "$tag")"
	win="$(grep -m1 -E "vfmig_iova: vf $vf IOVA window \[" <<<"$log" || true)"
	slots="$(grep -m1 -E "vfmig_iova: vf $vf arena reserved" <<<"$log" || true)"
	[[ -n "$win" ]] || { fail "vf $vf bits $bits: no window line logged"; return 1; }
	[[ "$win" == *"IOVA window [$(hex $base), $(hex $end)) reserved under a $b-bit DMA mask"* ]] ||
		fail "vf $vf bits $bits: want [$(hex $base), $(hex $end)) under $b bits, got: ${win#*vfmig_iova: }"
	[[ "$slots" == *"kernel slots [$(hex $base),"* && "$slots" == *"USER_PAGE ["*", $(hex $end))"* ]] ||
		fail "vf $vf bits $bits: slot bounds not in [$(hex $base), $(hex $end)): ${slots#*vfmig_iova: }"
	[[ "$(tracked_of "$vf")" == 1 ]] || fail "vf $vf bits $bits: QUERY_VF tracked != 1"
	check_clean "$tag" "vf $vf bits $bits"
	sed -n 's/.*(IOMMU aperture ends at \(0x[0-9a-f]*\), bus DMA limit \(0x[0-9a-f]*\)).*/\1 \2/p' \
		<<<"$win" | sed "s/^/$b $(hex $base) $(hex $end) /" >>"$STATS"
}

untrack_check() {
	local vf="$1" tag res rc log
	tag="s$((++SEQ))-untrack-vf$vf"
	mark "$tag"
	res="$(st "$vf" 0)"
	rc="${res%%$'\t'*}"
	[[ "$rc" == 0 ]] || { fail "vf $vf: untrack exited $rc: ${res#*$'\t'}"; return 1; }
	log="$(klog_since "$tag")"
	[[ "$log" == *"vfmig_iova: vf $vf arena released"* ]] ||
		fail "vf $vf: no 'arena released' logged on untrack"
	[[ "$(tracked_of "$vf")" == 0 ]] || fail "vf $vf: QUERY_VF tracked != 0 after untrack"
	check_clean "$tag" "vf $vf untrack"
}

# set_tracked must exit non-zero with @want (an strerror fragment).
expect_err() {
	local want="$1" res rc out
	shift
	res="$(st "$@")"
	rc="${res%%$'\t'*}"
	out="${res#*$'\t'}"
	NCHECK=$((NCHECK + 1))
	if [[ "$rc" == 0 || "$out" != *"$want"* ]]; then
		fail "set_tracked $*: want '$want', got exit $rc: $out"
	else
		note "set_tracked $*: $want (ok)"
	fi
}

expect_ok() {
	local res
	res="$(st "$@")"
	NCHECK=$((NCHECK + 1))
	[[ "${res%%$'\t'*}" == 0 ]] || fail "set_tracked $*: exited ${res%%$'\t'*}: ${res#*$'\t'}"
}

edges() {
	sect "Edges ($1)"
	expect_err "Invalid argument" 0 1 33
	expect_err "Invalid argument" 0 1 40
	expect_err "Invalid argument" 0 1 64
	track_check 0 39 || return
	expect_ok 0 1 39
	expect_ok 0 1 0
	expect_err "Device or resource busy" 0 1 38
	expect_err "Device or resource busy" 0 1 34
	[[ "$(tracked_of 0)" == 1 ]] || fail "vf 0 lost tracking after refused re-tracks"
	untrack_check 0
	expect_ok 0 0 38
	track_check 0 34 && untrack_check 0
}

teardown() {
	[[ "$KEEP" == 1 ]] && return 0
	echo 0 >"$PF_DEV/sriov_numvfs" 2>/dev/null || true
	echo 1 >"$PF_DEV/sriov_drivers_autoprobe" 2>/dev/null || true
}

# ---------------------------------------------------------------------------
sect "Preconditions"
[[ "$EUID" -eq 0 ]] || die "must run as root"
[[ -x "$TOOL" ]] || die "missing $TOOL -- make -C $HERE/.."
[[ "$("$TOOL" 2>&1 || true)" == *iova_bits* ]] || die "$TOOL has no iova_bits -- make -C $HERE/.."
[[ -d "$PF_DEV" ]] || die "PF $PF not present"
[[ -e "/dev/mlx5_vfmig/$PF" ]] || die "no /dev/mlx5_vfmig/$PF"
for b in "${BITS_A[@]}"; do
	(( b == 0 || (b >= 34 && b <= 39) )) || die "BITS entry $b outside 0 or 34..39"
done
note "kernel $(uname -r), PF $PF, $NUMVFS VFs, $ITERS rounds, bits: $BITS"
note "cmdline: $(cat /proc/cmdline)"
note "PCI host-bridge windows at or above 8 GiB (inside a window -> -ENOSPC there):"
while IFS= read -r l; do
	start="${l%%-*}"
	start="${start// /}"
	(( 16#$start >= 0x200000000 )) && echo "    $l"
done < <(grep 'PCI Bus' /proc/iomem)
trap teardown EXIT

sect "Provision"
echo 0 >"$PF_DEV/sriov_numvfs"
echo 0 >"$PF_DEV/sriov_drivers_autoprobe"
echo "$NUMVFS" >"$PF_DEV/sriov_numvfs" || die "could not create $NUMVFS VFs"
for ((j = 0; j < NUMVFS; j++)); do
	bdf="$(vf_bdf "$j")"
	ensure_dma_domain "$bdf"
	note "vf $j $bdf group $(basename "$(readlink "/sys/bus/pci/devices/$bdf/iommu_group")") $(cat "/sys/bus/pci/devices/$bdf/iommu_group/type")"
	[[ "$(tracked_of "$j")" == 0 ]] || die "vf $j already tracked after provisioning"
done

edges start

sect "Rounds"
n=${#BITS_A[@]}
for ((r = 1; r <= ITERS; r++)); do
	f0=$NFAIL
	for ((j = 0; j < NUMVFS; j++)); do
		track_check "$j" "${BITS_A[$(((r + j) % n))]}"
	done
	for ((j = 0; j < NUMVFS; j++)); do
		untrack_check "$j"
	done
	(( NFAIL == f0 )) && s=ok || s="$((NFAIL - f0)) failures"
	(( r % 10 == 0 || NFAIL != f0 || r == ITERS )) && note "round $r/$ITERS: $s"
done

edges end

sect "Report"
printf '  %-5s %-6s %-30s %-20s %s\n' bits count window "aperture end" "bus DMA limit"
sort "$STATS" | uniq -c | while read -r cnt b base end ap bus; do
	printf '  %-5s %-6s %-30s %-20s %s\n' "$b" "$cnt" "[$base, $end)" "$ap" "$bus"
done
note "workdir: $WORKDIR"
if (( NFAIL )); then
	echo "RESULT: FAIL ($NFAIL failures over $NCHECK checked calls)"
	exit 1
fi
echo "RESULT: PASS ($NCHECK checked calls)"
