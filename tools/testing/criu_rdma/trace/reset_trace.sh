#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Record PCI resets and mlx5 HCA lifecycle commands, and fail if any hit
# a watched VF other than the ones allowed.
#
#   reset_trace.sh arm
#   reset_trace.sh mark <label>
#   reset_trace.sh report --pf <pf-bdf> --vf <bdf> [--vf <bdf> ...]
#                         [--allow <call>@<bdf>=<n> ...]
#                         [--from <label>] [--to <label>]
#   reset_trace.sh disarm
#
# arm installs kprobes on pcie_flr, pcie_reset_flr, pci_reset_function,
# pci_reset_function_locked and __pci_reset_function_locked, on the mlx5
# ENABLE_HCA, DISABLE_HCA, INIT_HCA and TEARDOWN_HCA paths, and on
# mlx5_cmd_do() filtered to SAVE_VHCA_STATE and LOAD_VHCA_STATE, which
# report prints as window markers.
# Events go to a private tracefs instance, so other tracing is untouched.
#
# report prints every event and exits 1 if a reset or HCA command hit a
# watched VF without a matching --allow, or if an --allow count is not
# met exactly. A PF-issued ENABLE_HCA/DISABLE_HCA is charged to the VF
# its func_id names. --from/--to limit the check to events between two
# marks; markers from SAVE/LOAD are printed but never fail the report.
#
# <call> is one of: flr (pcie_flr, the FLR itself), reset (one reset
# operation; the pci_reset_* and pcie_reset_flr entry points it nests
# through count once), enable_hca, disable_hca, init_hca, teardown_hca. Example, recycle of VF 0000:08:00.2 with VF .3 watched:
#   reset_trace.sh report --pf 0000:08:00.0 --vf 0000:08:00.2 \
#       --vf 0000:08:00.3 --allow flr@0000:08:00.2=1 \
#       --allow reset@0000:08:00.2=1 --allow teardown_hca@0000:08:00.2=1 \
#       --allow disable_hca@0000:08:00.2=1
#
# TRACE_FILE=<file> makes report read a saved copy of the instance's
# trace instead of the live one (no root needed).
#
# Struct offsets come from gdb on the vmlinux and mlx5_core.ko of the
# kernel tree (KTREE, default: this script's tree), which must match the
# running kernel. Needs root.

set -u

KTREE="${KTREE:-$(cd "$(dirname "$0")/../../../.." && pwd)}"
TRACEFS="${TRACEFS:-/sys/kernel/tracing}"
TRACE_FILE="${TRACE_FILE:-}"
INST="$TRACEFS/instances/criu_rdma_reset"
GROUP=criu_reset

die() { echo "reset_trace: $*" >&2; exit 2; }

# offsetof(@type, @member) from @obj's DWARF. Prints nothing on failure.
offset() {
	local obj="$1" type="$2" member="$3"

	gdb -batch -ex "print (long)&((($type *)0)->$member)" "$obj" 2>/dev/null |
		sed -n 's/^\$1 = \([0-9][0-9]*\)$/\1/p'
}

traceable() {
	grep -qE "^$1( |$)" "$TRACEFS/available_filter_functions"
}

add_probe() {
	local name="$1" sym="$2" args="$3" filter="${4:-}" fn="${2#*:}"

	if ! traceable "$fn"; then
		echo "reset_trace: $fn not traceable, skipping" >&2
		return
	fi
	if ! echo "p:$GROUP/$name $sym $args" >>"$TRACEFS/kprobe_events"; then
		do_disarm quiet
		die "could not add probe $name on $sym ($args)"
	fi
	if [[ -n "$filter" ]] &&
	   ! echo "$filter" >"$INST/events/$GROUP/$name/filter"; then
		do_disarm quiet
		die "could not set filter on $name"
	fi
	if ! echo 1 >"$INST/events/$GROUP/$name/enable"; then
		do_disarm quiet
		die "could not enable $name"
	fi
}

do_arm() {
	local release pdev_name mdev_dev dev_name pdev mdev

	[[ -d "$TRACEFS" ]] || die "no tracefs at $TRACEFS"
	release=$(cat "$KTREE/include/config/kernel.release" 2>/dev/null)
	[[ "$release" == "$(uname -r)" ]] ||
		die "$KTREE is $release, running $(uname -r); set KTREE"
	grep -q '^mlx5_core ' /proc/modules || die "mlx5_core not loaded"

	pdev_name=$(offset "$KTREE/vmlinux" "struct pci_dev" dev.kobj.name)
	dev_name=$(offset "$KTREE/vmlinux" "struct device" kobj.name)
	mdev_dev=$(offset "$KTREE/drivers/net/ethernet/mellanox/mlx5/core/mlx5_core.ko" \
		   "struct mlx5_core_dev" device)
	[[ -n "$pdev_name" && -n "$dev_name" && -n "$mdev_dev" ]] ||
		die "could not read struct offsets from $KTREE (gdb + DWARF needed)"

	do_disarm quiet
	mkdir "$INST" || die "could not create $INST"
	echo 16384 >"$INST/buffer_size_kb"

	# :string reads the string at the fetched address, so each char *
	# member needs one more dereference than the pointer chain itself.
	pdev="bdf=+0(+$pdev_name(\$arg1)):string"
	mdev="dev=+0(+$dev_name(+$mdev_dev(\$arg1))):string"

	add_probe flr pcie_flr "$pdev"
	# probe=1 only asks whether FLR is supported; report skips it.
	add_probe reset_flr pcie_reset_flr "$pdev probe=\$arg2:u8"
	add_probe reset_function pci_reset_function "$pdev"
	add_probe reset_function_locked pci_reset_function_locked "$pdev"
	add_probe reset_function_locked_inner __pci_reset_function_locked "$pdev"
	add_probe enable_hca mlx5_core:mlx5_core_enable_hca "$mdev func_id=\$arg2:u16"
	add_probe disable_hca mlx5_core:mlx5_core_disable_hca "$mdev func_id=\$arg2:u16"
	add_probe init_hca mlx5_core:mlx5_cmd_init_hca "$mdev"
	add_probe teardown_hca mlx5_core:mlx5_cmd_teardown_hca "$mdev"
	add_probe force_teardown_hca mlx5_core:mlx5_cmd_force_teardown_hca "$mdev"
	add_probe fast_teardown_hca mlx5_core:mlx5_cmd_fast_teardown_hca "$mdev"
	# SAVE_VHCA_STATE (0x118) and LOAD_VHCA_STATE (0x119) as window
	# markers. The inbox is big-endian, so on x86 the u16 reads come out
	# byte-swapped: opcode 0x118 reads as 0x1801, and vhca_id sits at
	# byte 10. report swaps them back.
	add_probe vhca_cmd mlx5_core:mlx5_cmd_do \
		"$mdev opcode=+0(\$arg2):u16 vhca=+10(\$arg2):u16" \
		"opcode == 0x1801 || opcode == 0x1901"

	echo > "$INST/trace"
	echo 1 >"$INST/tracing_on"
	echo "reset_trace: armed ($INST)"
}

do_disarm() {
	if [[ -d "$INST" ]]; then
		echo 0 >"$INST/tracing_on"
		for f in "$INST"/events/$GROUP/*/enable; do
			[[ -e "$f" ]] && echo 0 >"$f"
		done
		rmdir "$INST" 2>/dev/null ||
			die "could not remove $INST"
	fi
	grep -o "^p:$GROUP/[^ ]*" "$TRACEFS/kprobe_events" 2>/dev/null |
	while read -r p; do
		echo "-:${p#p:}" >>"$TRACEFS/kprobe_events"
	done
	[[ "${1:-}" == quiet ]] || echo "reset_trace: disarmed"
}

do_mark() {
	[[ -d "$INST" ]] || die "not armed"
	echo "criu_rdma mark $1" >"$INST/trace_marker"
}

# Map an mlx5 HCA event to the BDF it acts on. A PF command with a
# non-zero func_id targets VF func_id - 1; its routing ID is computed
# from the PF's SR-IOV offset and stride, which stay readable after
# sriov_numvfs=0 removes the virtfn links.
target_bdf() {
	local dev="$1" func_id="$2" pf="$3" off stride dom bus df rid

	if [[ "$dev" == "$pf" && -n "$func_id" && "$func_id" != 0 ]]; then
		off=$(cat "/sys/bus/pci/devices/$pf/sriov_offset" 2>/dev/null)
		stride=$(cat "/sys/bus/pci/devices/$pf/sriov_stride" 2>/dev/null)
		if [[ -n "$off" && -n "$stride" &&
		      "$pf" =~ ^([0-9a-f]{4}):([0-9a-f]{2}):([0-9a-f]{2})\.([0-7])$ ]]; then
			dom=${BASH_REMATCH[1]}
			bus=$((16#${BASH_REMATCH[2]}))
			df=$(( (16#${BASH_REMATCH[3]} << 3) | BASH_REMATCH[4] ))
			rid=$(( (bus << 8 | df) + off + stride * (func_id - 1) ))
			printf '%s:%02x:%02x.%x\n' "$dom" $((rid >> 8)) \
				$(( (rid >> 3) & 0x1f )) $((rid & 7))
			return
		fi
	fi
	echo "$dev"
}

swap16() { echo $(( ($1 >> 8 | $1 << 8) & 0xffff )); }

do_report() {
	local pf="" from="" to="" last="" in_window line call bdf dev fid key a n
	local -a vfs=() allows=()
	local -A watched=() allowed=() seen=()
	local fail=0

	while (( $# )); do
		case "$1" in
		--pf) pf="$2"; shift 2 ;;
		--vf) vfs+=("$2"); shift 2 ;;
		--allow) allows+=("$2"); shift 2 ;;
		--from) from="$2"; shift 2 ;;
		--to) to="$2"; shift 2 ;;
		*) die "unknown report option $1" ;;
		esac
	done
	[[ -n "$pf" && ${#vfs[@]} -gt 0 ]] || die "report needs --pf and --vf"
	[[ -n "$TRACE_FILE" || -d "$INST" ]] || die "not armed"

	for a in "${vfs[@]}"; do watched[$a]=1; done
	for a in "${allows[@]}"; do
		[[ "$a" =~ ^([a-z_]+)@([0-9a-f:.]+)=([0-9]+)$ ]] ||
			die "bad --allow $a (want call@bdf=n)"
		allowed["${BASH_REMATCH[1]}@${BASH_REMATCH[2]}"]=${BASH_REMATCH[3]}
	done

	[[ -z "$from" ]] && in_window=1 || in_window=0
	while IFS= read -r line; do
		[[ "$line" == \#* || -z "$line" ]] && continue
		if [[ "$line" =~ criu_rdma\ mark\ (.*)$ ]]; then
			echo "MARK  ${BASH_REMATCH[1]}"
			[[ "${BASH_REMATCH[1]}" == "$from" ]] && in_window=1
			[[ -n "$to" && "${BASH_REMATCH[1]}" == "$to" ]] && in_window=0
			continue
		fi
		[[ "$line" =~ \ ([a-z_]+):\ \( ]] || continue
		call="${BASH_REMATCH[1]}"
		bdf=""; dev=""; fid=""
		[[ "$line" =~ bdf=\"([^\"]*)\" ]] && bdf="${BASH_REMATCH[1]}"
		[[ "$line" =~ dev=\"([^\"]*)\" ]] && dev="${BASH_REMATCH[1]}"
		[[ "$line" =~ func_id=([0-9]+) ]] && fid="${BASH_REMATCH[1]}"
		if [[ "$call" == vhca_cmd ]]; then
			[[ "$line" =~ opcode=(0x[0-9a-f]+|[0-9]+) ]] || continue
			case $(swap16 $((BASH_REMATCH[1]))) in
			$((0x118))) a=SAVE_VHCA_STATE ;;
			$((0x119))) a=LOAD_VHCA_STATE ;;
			*) continue ;;
			esac
			n=""
			[[ "$line" =~ vhca=(0x[0-9a-f]+|[0-9]+) ]] &&
				n=$(printf '0x%04x' "$(swap16 $((BASH_REMATCH[1])))")
			echo "WIN   $a vhca_id $n via $dev"
			continue
		fi
		[[ -z "$bdf" ]] && bdf=$(target_bdf "$dev" "$fid" "$pf")
		if [[ "$call" == reset_flr && "$line" =~ probe=([0-9]+) &&
		      "${BASH_REMATCH[1]}" != 0 ]]; then
			echo "      reset_flr $bdf (capability probe)"
			continue
		fi
		if [[ ! "$bdf" =~ ^[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]$ ]]; then
			echo "HIT   $call <unreadable device>  <-- cannot attribute"
			fail=1
			continue
		fi

		case "$call" in
		flr) ;;
		reset_flr|reset_function|reset_function_locked|reset_function_locked_inner)
			call=reset ;;
		force_teardown_hca|fast_teardown_hca) call=teardown_hca ;;
		esac

		# One reset nests several entry points; count it once.
		if [[ "$call" == reset && "$last" == "reset@$bdf" ]]; then
			echo "      $call $bdf (nested)"
			continue
		fi
		last="$call@$bdf"

		if (( ! in_window )) || [[ -z "${watched[$bdf]:-}" ]]; then
			echo "      $call $bdf (not checked)"
			continue
		fi
		key="$call@$bdf"
		seen[$key]=$(( ${seen[$key]:-0} + 1 ))
		if [[ -z "${allowed[$key]:-}" ]]; then
			echo "HIT   $call $bdf  <-- not allowed"
			fail=1
		else
			echo "HIT   $call $bdf (allowed)"
		fi
	done <"${TRACE_FILE:-$INST/trace}"

	for key in "${!allowed[@]}"; do
		n=${seen[$key]:-0}
		a=${allowed[$key]}
		if (( n != a )); then
			echo "COUNT $key: saw $n, expected $a  <-- mismatch"
			fail=1
		fi
	done
	for key in "${!seen[@]}"; do
		[[ -n "${allowed[$key]:-}" ]] && continue
		echo "COUNT $key: saw ${seen[$key]}, expected 0"
	done

	if (( fail )); then
		echo "reset_trace: FAIL"
		return 1
	fi
	echo "reset_trace: PASS"
}

[[ $EUID -eq 0 || ( "${1:-}" == report && -n "$TRACE_FILE" ) ]] ||
	die "needs root"
case "${1:-}" in
arm) do_arm ;;
disarm) do_disarm ;;
mark) [[ -n "${2:-}" ]] || die "mark needs a label"; do_mark "$2" ;;
report) shift; do_report "$@" ;;
*) sed -n '4,13p' "$0"; exit 2 ;;
esac
