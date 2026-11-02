#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Reuse VFs for SAVE/LOAD round after round without SR-IOV teardown.
#
# Each cycle provisions VF 0 and VF 1 once, then runs $ITERS rounds
# back to back. Every round checks that sriov_numvfs and the virtfn
# links are unchanged, and, with TRACE=1, that the vacated VF sees
# exactly one reset and one FLR and that nothing resets or re-inits
# either VF between LOAD and the end of the restored probe.
#
#   1  VF 1 runs normally, is unbound and FLR'd through sysfs, and
#      takes VF 0's image.
#   2  As 1, with RECYCLE_VF instead of the sysfs FLR.
#   3  VF 1 is restored at the start of every round: unbind it,
#      RECYCLE_VF it and LOAD VF 0's image into it again. Its teardown
#      must log no fatal health event and leave no arena pages behind.
#   4  The two VFs alternate: each round saves from the VF restored in
#      the previous round and loads into the other one.
#   5  As 4, with a UUID that hops to the destination every round
#      through SET_VF_UUID's clear-then-stamp handoff, and checks that
#      duplicate stamps and recycling a stamped VF are refused.
#
# In cycles 1, 2, 4 and 5 a restored VF has to be unbound to be reused.
# A fatal health event there is the known restored-VF teardown problem
# (design/teardown_reclaim_hang.md); it is reported as INFO in those
# cycles and fails cycle 3, which exists to check it.
#
# A restored VF passes when the probe takes the restored path, its ib
# device appears and it shows no fatal health event for $SETTLE seconds.
#
# Usage (as root):
#   PF=0000:08:00.0 tools/testing/criu_rdma/save_load/test_vf_recycle.sh
#   CYCLES="2 5" ITERS=20 .../test_vf_recycle.sh
#
# Environment:
#   PF            PF BDF (default 0000:08:00.0)
#   CYCLES        cycles to run, in order (default "1 2 3 4 5")
#   ITERS         rounds per cycle (default 10)
#   TOOL          mlx5_vfmig CLI (default ../tools/mlx5_vfmig)
#   TRACE         1 (default): check resets with ../trace/reset_trace.sh;
#                 0: skip
#   EXPECT_BASE   arena base every VF should reserve (default 0x100000000)
#   SET_DMA       1 (default): switch an identity-domain VF's IOMMU group
#                 to DMA before tracking it; 0: fail instead
#   BIND_TIMEOUT  seconds to wait for a VF probe or unbind (default 90)
#   SETTLE        seconds to watch a restored VF's health (default 10)
#   KEEP          1: leave the last cycle's VFs and the work dir in place
#
# Exits 0 if every cycle passes, non-zero otherwise.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

PF="${PF:-0000:08:00.0}"
CYCLES="${CYCLES:-1 2 3 4 5}"
ITERS="${ITERS:-10}"
TOOL="${TOOL:-$HERE/../tools/mlx5_vfmig}"
RTRACE="$HERE/../trace/reset_trace.sh"
TRACE="${TRACE:-1}"
EXPECT_BASE="${EXPECT_BASE:-0x100000000}"
SET_DMA="${SET_DMA:-1}"
BIND_TIMEOUT="${BIND_TIMEOUT:-90}"
SETTLE="${SETTLE:-10}"
KEEP="${KEEP:-0}"

WORKDIR="$(mktemp -d /tmp/vfmig-recycle-XXXXXX)"
BLOB="$WORKDIR/vf.blob"
PF_DEV="/sys/bus/pci/devices/$PF"
CDEV="/dev/mlx5_vfmig/$PF"
RUN_ID="$$-$(date +%s)"
U1=5eed0001-0000-4000-8000-000000000001
U2=5eed0002-0000-4000-8000-000000000002

# mlx5 health poller reporting a dead or disabled function.
HEALTH_ERRS='Fatal error [0-9]+ detected|firmware internal error|NIC IFC still'
# Allocator complaints that mean replay or the restored probe went wrong.
ALLOC_ERRS='replay_page\(|replay_external\(|replay: wire claims|DRIFT|replay/alloc size mismatch|exhausted at cursor|could not reserve IOVA window|not SET_TRACKED|manifest CRC mismatch'

# Per-cycle state, reset by run_cycle.
CYCLE=0
ROUND=0
# Numbers steps that can repeat within a round, to keep labels unique.
SEQ=0
LAYOUT0=""
declare -a FAILS=() INFOS=()

note() { printf '  %s\n' "$*"; }
sect() { printf '\n== %s ==\n' "$*"; }
die()  { echo "FAIL: $*" >&2; exit 1; }

# A hard failure leaves the VFs in an unknown state: stop the cycle.
hard() { FAILS+=("round $ROUND: $*"); echo "  FAIL round $ROUND: $*"; exit 1; }
soft() { FAILS+=("round $ROUND: $*"); echo "  FAIL round $ROUND: $*"; }
info() { INFOS+=("round $ROUND: $*"); echo "  INFO round $ROUND: $*"; }

# Kernel log and trace markers. Labels are unique per cycle and round.
mark() {
	echo "criu_rdma recycle $RUN_ID: c$CYCLE $1" >/dev/kmsg
	[[ "$TRACE" == 1 ]] && "$RTRACE" mark "c$CYCLE-$1"
	return 0
}
klog_since() {
	dmesg | sed -n "/criu_rdma recycle $RUN_ID: c$CYCLE $1\$/,\$p"
}

# reset_trace.sh report over [from, to); prints the window only on FAIL.
trace_check() {
	local from="$1" to="$2" what="$3" out
	shift 3
	[[ "$TRACE" == 1 ]] || return 0
	if ! out="$("$RTRACE" report --pf "$PF" --vf "$BDF0" --vf "$BDF1" \
		    --from "c$CYCLE-$from" --to "c$CYCLE-$to" "$@")"; then
		echo "$out" | sed 's/^/    /'
		soft "trace: $what"
	fi
}

vf_bdf() { basename "$(readlink "$PF_DEV/virtfn$1")"; }
bdf_of() { if [[ "$1" == 0 ]]; then echo "$BDF0"; else echo "$BDF1"; fi; }

layout() {
	local l
	cat "$PF_DEV/sriov_numvfs"
	for l in "$PF_DEV"/virtfn*; do echo "$(basename "$l") $(readlink "$l")"; done
}

check_layout() {
	[[ "$(layout)" == "$LAYOUT0" ]] ||
		hard "sriov_numvfs or virtfn links changed: $(layout | tr '\n' ' ')"
}

vhca_of() { "$TOOL" "$PF" get_vhca_id "$1" | awk '{print $NF}'; }
query() { "$TOOL" "$PF" query_vf "$1"; }

# Run a CLI step that must exit with @want (0 or 1).
expect_rc() {
	local want="$1" rc=0 out
	shift
	out="$("$TOOL" "$PF" "$@" 2>&1)" || rc=$?
	(( rc == want )) || soft "'$*' exited $rc, want $want: $out"
}

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

# SET_TRACKED and ENABLE_MIGRATABLE on an unbound, untracked VF.
bootstrap_vf() {
	local vf="$1" tag="r$ROUND.$((++SEQ))-track$1" line
	ensure_dma_domain "$(bdf_of "$vf")"
	mark "$tag"
	"$TOOL" "$PF" set_tracked "$vf" 1 >/dev/null || hard "SET_TRACKED vf $vf failed"
	line="$(klog_since "$tag" | grep -m1 "vfmig_iova: vf $vf arena reserved" || true)"
	[[ "$line" == *"kernel slots [$EXPECT_BASE,"* ]] ||
		hard "vf $vf arena not reserved at $EXPECT_BASE: ${line:-nothing logged}"
	"$TOOL" "$PF" enable_migratable "$vf" >/dev/null ||
		hard "ENABLE_MIGRATABLE vf $vf failed"
}

# Bind or unbind with a bounded wait: a wedged probe or teardown holds
# the device lock and never returns.
wait_bg() {
	local bg="$1" started=$SECONDS
	while kill -0 "$bg" 2>/dev/null; do
		(( SECONDS - started < BIND_TIMEOUT )) || return 2
		sleep 0.5
	done
	wait "$bg" 2>/dev/null || true
}

bind_vf() {
	local bdf="$1" bg
	echo mlx5_core >"/sys/bus/pci/devices/$bdf/driver_override"
	( echo "$bdf" >/sys/bus/pci/drivers/mlx5_core/bind ) >/dev/null 2>&1 &
	bg=$!
	wait_bg "$bg" || hard "$bdf probe still pending after ${BIND_TIMEOUT}s (wedged; reboot may be needed)"
	[[ -e "/sys/bus/pci/devices/$bdf/driver" ]] || hard "$bdf did not bind"
}

ibdev_of() {
	local ib i
	for i in $(seq 1 100); do
		for ib in /sys/class/infiniband/*; do
			[[ -e "$ib/device" ]] || continue
			[[ "$(basename "$(readlink "$ib/device")")" == "$1" ]] &&
				{ basename "$ib"; return 0; }
		done
		sleep 0.1
	done
	return 1
}

# Bind @vf as an ordinary VF.
bind_live() {
	local vf="$1" bdf
	bdf="$(bdf_of "$vf")"
	bind_vf "$bdf"
	ibdev_of "$bdf" >/dev/null || hard "no ib device for live VF $bdf"
}

# Unbind @vf. @restored=1: it came up from a LOAD; its teardown health
# is checked as cycle 3's subject and reported as INFO elsewhere.
unbind_vf() {
	local vf="$1" restored="$2" bdf bg t0 line tag="r$ROUND.$((++SEQ))-unbind$1" kind=live
	bdf="$(bdf_of "$vf")"
	[[ "$restored" == 1 ]] && kind=restored
	mark "$tag"
	t0=$SECONDS
	( echo "$bdf" >"/sys/bus/pci/devices/$bdf/driver/unbind" ) >/dev/null 2>&1 &
	bg=$!
	wait_bg "$bg" || hard "unbind of $bdf still blocked after ${BIND_TIMEOUT}s"
	[[ ! -e "/sys/bus/pci/devices/$bdf/driver" ]] || hard "$bdf still bound after unbind"
	line="$(klog_since "$tag" | grep -E -m1 "$HEALTH_ERRS" || true)"
	[[ -z "$line" ]] && return 0
	line="unbind of $kind VF $vf ($((SECONDS - t0))s): ${line#*] }"
	if [[ "$restored" == 1 && "$CYCLE" != 3 ]]; then
		info "$line"
	else
		soft "$line"
	fi
}

# Reset an unbound @vf with @how (flr: sysfs reset; recycle: RECYCLE_VF
# followed by the bootstrap) and check the trace saw one FLR of it.
# @restored is passed on to the arena-page check.
vacate_vf() {
	local vf="$1" how="$2" restored="$3" bdf line pages q s="r$ROUND.$((++SEQ))"
	bdf="$(bdf_of "$vf")"
	mark "$s-reset$vf"
	if [[ "$how" == flr ]]; then
		echo 1 >"/sys/bus/pci/devices/$bdf/reset" || hard "sysfs FLR of $bdf failed"
	else
		"$TOOL" "$PF" recycle_vf "$vf" >/dev/null || hard "RECYCLE_VF vf $vf failed"
		line="$(klog_since "$s-reset$vf" | grep -m1 "vfmig: vf $vf recycled" || true)"
		[[ "$line" =~ ,\ ([0-9]+)\ arena\ pages\ dropped ]] ||
			hard "no recycle line logged for vf $vf"
		pages="${BASH_REMATCH[1]}"
		if (( pages )); then
			line="recycle of VF $vf dropped $pages arena pages left by its teardown"
			if [[ "$restored" == 1 && "$CYCLE" != 3 ]]; then info "$line"; else soft "$line"; fi
		fi
		q="$(query "$vf")"
		[[ "$q" == *"restored=0 tracked=0 "*"vf_uuid=<unset>"* ]] ||
			hard "vf $vf after recycle: $q"
	fi
	mark "$s-boot$vf"
	[[ "$how" == recycle ]] && bootstrap_vf "$vf"
	trace_check "$s-reset$vf" "$s-boot$vf" "reset of VF $vf" \
		--allow "flr@$bdf=1" --allow "reset@$bdf=1"
}

# SAVE @src (bound), LOAD into @dst (unbound, bootstrapped), bind @dst
# and check it came up restored.
restore() {
	local src="$1" dst="$2" bdf vhca log line
	bdf="$(bdf_of "$dst")"
	vhca="$(vhca_of "$dst")"

	mark "r$ROUND-save"
	"$TOOL" "$PF" save_vhca_state "$src" "$BLOB" >/dev/null || hard "SAVE from vf $src failed"
	grep -q "vfmig: saved vf $src (vhca_id" <<<"$(klog_since "r$ROUND-save")" ||
		hard "no completed SAVE logged for vf $src"

	mark "r$ROUND-load"
	"$TOOL" "$PF" load_vhca_state "$dst" "$BLOB" >/dev/null || hard "LOAD into vf $dst rejected"
	log="$(klog_since "r$ROUND-load")"
	if grep -E "$ALLOC_ERRS" <<<"$log" >&2; then hard "replay into vf $dst rejected (above)"; fi
	grep -q "staged [0-9]* bytes of LOAD state for vf $dst (vhca_id $vhca)" <<<"$log" ||
		hard "LOAD not staged for vf $dst"

	mark "r$ROUND-bind"
	bind_vf "$bdf"
	log="$(klog_since "r$ROUND-bind")"
	grep -q "applied [0-9]* bytes of LOAD state to vf $dst (vhca_id $vhca)" <<<"$log" ||
		hard "LOAD_VHCA_STATE not applied to vf $dst"
	grep -q "VF (vhca_id $vhca) restored;" <<<"$log" ||
		hard "vf $dst did not take the restored probe path"
	if grep -E "$ALLOC_ERRS" <<<"$log" >&2; then hard "allocator drift in restored probe (above)"; fi
	ibdev_of "$bdf" >/dev/null || hard "no ib device for restored VF $bdf"

	sleep "$SETTLE"
	[[ -e "/sys/bus/pci/devices/$bdf/driver" ]] || hard "restored VF $bdf lost its driver"
	line="$(klog_since "r$ROUND-bind" | grep -E -m1 "$HEALTH_ERRS" || true)"
	[[ -z "$line" ]] || soft "restored VF $dst health: ${line#*] }"
	mark "r$ROUND-settled"
	trace_check "r$ROUND-load" "r$ROUND-settled" "LOAD to settled"
}

# Cycle 5: refusals on the unbound destination, before it is recycled.
uuid_refusals() {
	local dst="$1" holder="$2"
	mark "r$ROUND-refuse"
	expect_rc 1 set_vf_uuid "$dst" "$U1"
	expect_rc 0 set_vf_uuid "$dst" "$U2"
	expect_rc 1 recycle_vf "$dst"
	expect_rc 1 clear_vf_uuid "$dst" "$U1"
	expect_rc 0 clear_vf_uuid "$dst" "$U2"
	[[ "$(query "$holder")" == *"vf_uuid=$U1"* ]] || soft "vf $holder lost $U1"
	mark "r$ROUND-refused"
	trace_check "r$ROUND-refuse" "r$ROUND-refused" "UUID refusals"
}

# Cycle 5: move U1 from @src to @dst, the restored destination.
uuid_handoff() {
	local src="$1" dst="$2"
	expect_rc 1 set_vf_uuid "$dst" "$U1"
	expect_rc 1 clear_vf_uuid "$src" "$U2"
	expect_rc 0 clear_vf_uuid "$src" "$U1"
	expect_rc 0 set_vf_uuid "$dst" "$U1"
	[[ "$(query "$dst")" == *"vf_uuid=$U1"* ]] || soft "U1 did not land on vf $dst"
	[[ "$(query "$src")" == *"vf_uuid=<unset>"* ]] || soft "vf $src still holds a UUID"
}

provision() {
	echo 0 >"$PF_DEV/sriov_numvfs"
	echo 0 >"$PF_DEV/sriov_drivers_autoprobe"
	echo 2 >"$PF_DEV/sriov_numvfs"
	BDF0="$(vf_bdf 0)"
	BDF1="$(vf_bdf 1)"
	bootstrap_vf 0
	bootstrap_vf 1
	LAYOUT0="$(layout)"
}

cycle_body() {
	local x=0 y=1 t

	case "$CYCLE" in
	1|2)
		bind_live 0
		bind_live 1
		for ROUND in $(seq 1 "$ITERS"); do
			unbind_vf 1 0
			vacate_vf 1 "$([[ $CYCLE == 1 ]] && echo flr || echo recycle)" 0
			restore 0 1
			# Back to a live VF for the next round.
			unbind_vf 1 1
			vacate_vf 1 recycle 1
			bind_live 1
			check_layout
			note "round $ROUND done"
		done
		;;
	3)
		bind_live 0
		restore 0 1
		for ROUND in $(seq 1 "$ITERS"); do
			unbind_vf 1 1
			vacate_vf 1 recycle 1
			restore 0 1
			check_layout
			note "round $ROUND done"
		done
		;;
	4|5)
		bind_live 0
		bind_live 1
		[[ "$CYCLE" == 5 ]] && expect_rc 0 set_vf_uuid 0 "$U1"
		for ROUND in $(seq 1 "$ITERS"); do
			unbind_vf "$y" "$(( ROUND > 2 ))"
			[[ "$CYCLE" == 5 ]] && uuid_refusals "$y" "$x"
			vacate_vf "$y" recycle "$(( ROUND > 2 ))"
			restore "$x" "$y"
			[[ "$CYCLE" == 5 ]] && uuid_handoff "$x" "$y"
			check_layout
			note "round $ROUND done ($x -> $y)"
			t=$x; x=$y; y=$t
		done
		;;
	*) die "unknown cycle $CYCLE" ;;
	esac
}

teardown() {
	local line
	if [[ "$TRACE" == 1 ]]; then "$RTRACE" disarm >/dev/null 2>&1 || true; fi
	[[ "$KEEP" == 1 ]] && return 0
	echo "criu_rdma recycle $RUN_ID: c$CYCLE teardown" >/dev/kmsg
	echo 0 >"$PF_DEV/sriov_numvfs" 2>/dev/null || true
	echo 1 >"$PF_DEV/sriov_drivers_autoprobe" 2>/dev/null || true
	line="$(klog_since teardown | grep -E -m1 "$HEALTH_ERRS" || true)"
	[[ -z "$line" ]] || echo "  INFO: fatal health event during SR-IOV disable: ${line#*] }"
	return 0
}

# Run one cycle in a subshell so a hard failure ends only that cycle.
# Its findings go to $WORKDIR/cycle<N>.{fail,info,round,done}. The
# subshell must not be tested (|| or if), or bash ignores set -e in it.
run_cycle() {
	CYCLE="$1"
	sect "Cycle $CYCLE ($ITERS rounds)"
	set +e
	(
		set -e
		ROUND=0
		finish() {
			local st=$?
			(( st == 0 || ${#FAILS[@]} )) || FAILS+=("round $ROUND: aborted (exit $st, see output)")
			printf '%s\n' "${FAILS[@]+"${FAILS[@]}"}" >"$WORKDIR/cycle$CYCLE.fail"
			printf '%s\n' "${INFOS[@]+"${INFOS[@]}"}" >"$WORKDIR/cycle$CYCLE.info"
			echo "$ROUND" >"$WORKDIR/cycle$CYCLE.round"
			teardown
		}
		trap finish EXIT
		if [[ "$TRACE" == 1 ]]; then
			"$RTRACE" disarm >/dev/null 2>&1 || true
			"$RTRACE" arm >/dev/null || die "reset_trace.sh arm failed (TRACE=0 to skip)"
		fi
		provision
		cycle_body
		touch "$WORKDIR/cycle$CYCLE.done"
	)
	set -e
}

# ---------------------------------------------------------------------------
sect "Preconditions"
[[ "$EUID" -eq 0 ]] || die "must run as root"
[[ -x "$TOOL" ]] || die "missing $TOOL -- make -C $HERE/.."
[[ "$("$TOOL" 2>&1 || true)" == *recycle_vf* ]] || die "$TOOL has no recycle_vf -- make -C $HERE/.."
[[ -d "$PF_DEV" ]] || die "PF $PF not present"
[[ -e "$CDEV" ]] || die "missing $CDEV (mlx5_core without vfmig?)"
(( $(cat "$PF_DEV/sriov_totalvfs") >= 2 )) || die "PF $PF supports fewer than 2 VFs"
note "PF=$PF CYCLES=\"$CYCLES\" ITERS=$ITERS TRACE=$TRACE kernel=$(uname -r)"

for c in $CYCLES; do
	run_cycle "$c"
done

sect "Summary"
rc=0
for c in $CYCLES; do
	fails="$(grep -c . "$WORKDIR/cycle$c.fail" 2>/dev/null || true)"
	round="$(cat "$WORKDIR/cycle$c.round" 2>/dev/null || echo 0)"
	if [[ ! -e "$WORKDIR/cycle$c.done" ]]; then
		printf '  cycle %s  FAIL (stopped in round %s of %s)\n' "$c" "$round" "$ITERS"
		rc=1
	elif [[ "${fails:-0}" != 0 ]]; then
		printf '  cycle %s  FAIL (%s findings over %s rounds)\n' "$c" "$fails" "$ITERS"
		rc=1
	else
		printf '  cycle %s  PASS (%s rounds)\n' "$c" "$ITERS"
	fi
	sed 's/^/      /' "$WORKDIR/cycle$c.fail" 2>/dev/null | grep -v '^ *$' | head -10 || true
	sort "$WORKDIR/cycle$c.info" 2>/dev/null | grep . | sed 's/^/      INFO /' | head -5 || true
	n="$(grep -c . "$WORKDIR/cycle$c.info" 2>/dev/null || true)"
	(( ${n:-0} <= 5 )) || printf '      INFO ... %s in total\n' "$n"
done
[[ "$KEEP" == 1 ]] && echo "  [keep] work dir $WORKDIR" || rm -rf "$WORKDIR"
exit $rc
