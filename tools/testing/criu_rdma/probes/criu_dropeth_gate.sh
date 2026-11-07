#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Can "unbind mlx5e before SAVE" live entirely inside CRIU?
#
# Same flow as criu/test/rdma/run_vfmig_qp_cr.sh, with the harness playing
# both the orchestrator (VF admin MAC, IP on the destination) and the step
# CRIU would own (unbind the source's mlx5_core.eth before the dump):
#
#   source:  set VF admin MAC, bind, ip addr add $IP, wait for the RoCEv2
#            IPv4 GID, start qp_gid_holder on that GID index (self-loop RC
#            QP, one zero-length SEND to itself), [dropeth: unbind ETH and
#            re-read the QP state], criu dump (SAVE).
#   dest:    reprovision, set the same admin MAC, prerestore tool (LOAD +
#            bind), ip addr add $IP, check port ACTIVE and that the GID sits
#            at the same index with the same value, criu restore, SIGUSR1:
#            QP must still be RTS and pass another self-SEND.
#
# MODES="baseline dropeth" (default) runs both back to back; baseline keeps
# the source netdev bound, so a failure in both modes is not the unbind.
# With MLX5_VFMIG_RX_FENCE_F_ETH_SUSPEND in the kernel (vfmig_fence ... eth
# stands in for the plugin passing it; the plugin's own fence stacks on top):
#   suspendeth  suspend the source's mlx5e instead of unbinding it
#   leaverun    suspend it, criu dump --leave-running, lift both fences
#               (resuming mlx5e), and check that the still-running source QP
#               passes traffic (no restore)
#
#   sudo CRIU_REPO=/path/to/criu tools/testing/criu_rdma/probes/criu_dropeth_gate.sh                 # VF0 -> VF1
#   sudo CRIU_REPO=/path/to/criu DST_VF=0 tools/testing/criu_rdma/probes/criu_dropeth_gate.sh        # same slot
#
# Functional only; no timing.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CRIU_REPO="${CRIU_REPO:?set CRIU_REPO to the CRIU tree}"
PLUGIN_DIR="$CRIU_REPO/plugins/rdma/mlx5_sriov_vfmig"
MLX5_SO="${MLX5_SO:-$PLUGIN_DIR/rdma_mlx5_vfmig_plugin.so}"
RESTORE_TOOL="${RESTORE_TOOL:-$PLUGIN_DIR/mlx5_vfmig_restore_vf}"
CRIU="${CRIU:-$CRIU_REPO/criu/criu}"
VFMIG_TOOL="${VFMIG_TOOL:-$HERE/../tools/mlx5_vfmig}"
HOLDER="${HOLDER:-$CRIU_REPO/test/rdma/qp_gid_holder}"
FENCE_TOOL="${FENCE_TOOL:-$HERE/vfmig_fence}"

PF="${PF:-0000:08:00.0}"
SRC_VF="${SRC_VF:-0}"
DST_VF="${DST_VF:-1}"
MODES="${MODES:-baseline dropeth}"
IP="${IP:-192.168.250.10}"
MAC="${MAC:-02:00:00:fa:00:10}"
# Destination admin MAC; "none" leaves it unset. Differs from MAC only to
# see whether LOAD carries the source's MAC.
DST_MAC="${DST_MAC:-$MAC}"

PF_DEV="/sys/bus/pci/devices/$PF"
PF_NET="$(ls "$PF_DEV/net" 2>/dev/null | head -1)"
NUMVFS=$(( (SRC_VF > DST_VF ? SRC_VF : DST_VF) + 1 ))
WORKROOT="${WORKROOT:-/var/tmp/dropeth_gate.$(date +%Y%m%d-%H%M%S)}"
HOLDER_PID=""
RESTORED_PID=""
declare -A RESULT DETAIL

note() { printf '  %s\n' "$*"; }
sect() { printf '\n== %s ==\n' "$*"; }
kmsg() { echo "dropeth_gate: $*" >/dev/kmsg 2>/dev/null || true; }

cleanup() {
	[[ -n "$HOLDER_PID" ]] && kill -KILL "$HOLDER_PID" 2>/dev/null
	[[ -n "$RESTORED_PID" ]] && kill -KILL "$RESTORED_PID" 2>/dev/null
	echo 0 >"$PF_DEV/sriov_numvfs" 2>/dev/null
	echo 1 >"$PF_DEV/sriov_drivers_autoprobe" 2>/dev/null
}
trap cleanup EXIT

[[ "$EUID" -eq 0 ]] || { echo "run as root"; exit 2; }
for f in "$MLX5_SO" "$RESTORE_TOOL" "$CRIU" "$VFMIG_TOOL" "$HOLDER" "$FENCE_TOOL"; do
	[[ -e "$f" ]] || { echo "missing $f"; exit 2; }
done
[[ -n "$PF_NET" ]] || { echo "PF $PF has no netdev (needed to set the VF MAC)"; exit 2; }
echo 1 >/sys/bus/auxiliary/drivers_autoprobe
mkdir -p "$WORKROOT"

ensure_dma_domain() {
	local t="/sys/bus/pci/devices/$1/iommu_group/type"
	case "$(cat "$t")" in DMA|DMA-FQ) ;; *) echo DMA >"$t" ;; esac
}

vf_bdf() { readlink "$PF_DEV/virtfn$1" | xargs basename; }

wait_ibdev() {
	local bdf="$1" ib
	for _ in $(seq 1 100); do
		for ib in /sys/class/infiniband/*; do
			[[ "$(basename "$(readlink -f "$ib/device")")" == "$bdf" ]] \
				&& { basename "$ib"; return 0; }
		done
		sleep 0.1
	done
	return 1
}

wait_netdev() {
	local bdf="$1"
	for _ in $(seq 1 100); do
		ls "/sys/bus/pci/devices/$bdf/net" 2>/dev/null | head -1 | grep . && return 0
		sleep 0.1
	done
	return 1
}

eth_auxdev() { basename "$(ls -d /sys/bus/pci/devices/$1/mlx5_core.eth.* 2>/dev/null | head -1)"; }

port_state() { cat "/sys/class/infiniband/$1/ports/1/state" 2>/dev/null || echo "?"; }

wait_active() {
	for _ in $(seq 1 100); do
		[[ "$(port_state "$1")" == *ACTIVE* ]] && return 0
		sleep 0.1
	done
	return 1
}

ipv4_gid() {
	local IFS=.
	set -- $IP
	printf '0000:0000:0000:0000:0000:ffff:%02x%02x:%02x%02x' "$1" "$2" "$3" "$4"
}

# Index of the RoCE v2 GID carrying $IP, waiting for ib_core to add it.
find_gid_index() {
	local ib="$1" want p i
	want="$(ipv4_gid)"
	p="/sys/class/infiniband/$ib/ports/1"
	for _ in $(seq 1 100); do
		for g in "$p"/gids/*; do
			i="$(basename "$g")"
			[[ "$(cat "$g" 2>/dev/null)" == "$want" ]] || continue
			[[ "$(cat "$p/gid_attrs/types/$i" 2>/dev/null)" == "RoCE v2" ]] || continue
			echo "$i"; return 0
		done
		sleep 0.1
	done
	return 1
}

gid_table() {
	local p="/sys/class/infiniband/$1/ports/1" g i v
	for g in "$p"/gids/*; do
		i="$(basename "$g")"
		v="$(cat "$g" 2>/dev/null)" || continue
		[[ "$v" == 0000:0000:0000:0000:0000:0000:0000:0000 ]] && continue
		printf '%s %s %s %s\n' "$i" "$v" "$(cat "$p/gid_attrs/types/$i" 2>/dev/null)" \
			"$(cat "$p/gid_attrs/ndevs/$i" 2>/dev/null)"
	done
}

configure_ip() {
	ip link set "$1" up && ip addr add "$IP/24" dev "$1"
}

holder_status() {
	local s
	: >"$STATUS"
	kill "-$1" "$2" 2>/dev/null || return 1
	for _ in $(seq 1 150); do
		s="$(cat "$STATUS" 2>/dev/null)"
		[[ -n "$s" ]] && { echo "$s"; return 0; }
		sleep 0.1
	done
	echo "(no answer)"
}

# run_mode runs in a subshell; results come back through files.
fail() {
	echo "FAIL: $*"
	echo "$*" >"$WORKROOT/$MODE/detail"
	exit 1
}

run_mode() {
	trap '[[ -n "$HOLDER_PID" ]] && kill -KILL "$HOLDER_PID"; [[ -n "$RESTORED_PID" ]] && kill -KILL "$RESTORED_PID"' EXIT
	local W="$WORKROOT/$MODE" SRC_BDF DEST_BDF SRC_IB DEST_IB NET IDX DIDX EAUX st out
	mkdir -p "$W/plugins" "$W/img"
	ln -sf "$MLX5_SO" "$W/plugins/"
	STATUS="$W/holder.status"
	local IMG="$W/img" UUID
	UUID="$(cat /proc/sys/kernel/random/uuid)"
	kmsg "mode=$MODE begin"

	sect "[$MODE] source VF $SRC_VF"
	echo 0 >"$PF_DEV/sriov_numvfs"
	echo 0 >"$PF_DEV/sriov_drivers_autoprobe"
	echo "$NUMVFS" >"$PF_DEV/sriov_numvfs"
	SRC_BDF="$(vf_bdf "$SRC_VF")"
	ensure_dma_domain "$SRC_BDF"
	ip link set "$PF_NET" vf "$SRC_VF" mac "$MAC" || fail "set source VF MAC"
	"$VFMIG_TOOL" "$PF" set_tracked "$SRC_VF" 1 >/dev/null || fail set_tracked
	"$VFMIG_TOOL" "$PF" enable_migratable "$SRC_VF" >/dev/null || fail enable_migratable
	"$VFMIG_TOOL" "$PF" set_vf_uuid "$SRC_VF" "$UUID" >/dev/null || fail set_vf_uuid
	echo mlx5_core >"/sys/bus/pci/devices/$SRC_BDF/driver_override"
	echo "$SRC_BDF" >/sys/bus/pci/drivers/mlx5_core/bind
	SRC_IB="$(wait_ibdev "$SRC_BDF")" || fail "no source ibdev"
	NET="$(wait_netdev "$SRC_BDF")" || fail "no source netdev"
	configure_ip "$NET" || fail "ip on source $NET"
	wait_active "$SRC_IB" || fail "source port not ACTIVE: $(port_state "$SRC_IB")"
	IDX="$(find_gid_index "$SRC_IB")" || fail "no RoCEv2 GID for $IP on source"
	note "bdf=$SRC_BDF ib=$SRC_IB net=$NET mac=$(cat /sys/class/net/$NET/address) gid_index=$IDX"
	gid_table "$SRC_IB" >"$W/gids.src"
	sed 's/^/    /' "$W/gids.src"

	systemd-run --scope --quiet --unit="dropeth-holder-$$-$MODE" \
		bash -c "exec </dev/null >'$W/holder.log' 2>&1; exec '$HOLDER' '$SRC_IB' '$STATUS' '$IDX'" &
	disown "$!" 2>/dev/null
	for _ in $(seq 1 100); do
		st="$(cat "$STATUS" 2>/dev/null)"
		[[ -n "$st" ]] && break
		sleep 0.1
	done
	[[ "$st" == READY ]] || { cat "$W/holder.log"; fail "holder: ${st:-no status}"; }
	HOLDER_PID="$(pgrep -fx "$HOLDER $SRC_IB $STATUS $IDX" | head -1)"
	[[ -n "$HOLDER_PID" ]] || fail "holder pid not found"
	note "holder pid=$HOLDER_PID: $(cat "$W/holder.log") (self-SEND ok)"
	note "QP before SAVE: $(holder_status USR2 "$HOLDER_PID") (RTS=3)"

	if [[ "$MODE" == dropeth ]]; then
		EAUX="$(eth_auxdev "$SRC_BDF")"
		[[ -n "$EAUX" ]] || fail "no mlx5_core.eth aux device under $SRC_BDF"
		echo "$EAUX" >/sys/bus/auxiliary/drivers/mlx5_core.eth/unbind \
			|| fail "unbind $EAUX"
		sleep 1
		note "unbound $EAUX; netdev present: $(ls /sys/bus/pci/devices/$SRC_BDF/net 2>/dev/null || echo none)"
		note "port: $(port_state "$SRC_IB"); GIDs left: $(gid_table "$SRC_IB" | wc -l)"
		st="$(holder_status USR2 "$HOLDER_PID")"
		note "QP after unbind: $st"
		echo "$st" >"$W/qp_after_unbind"
	fi

	# Stand-in for the plugin raising its fence with F_ETH_SUSPEND: raise our
	# own here; the plugin's plain fence stacks on top of it during the dump.
	local gfence="" leave=()
	if [[ "$MODE" == suspendeth || "$MODE" == leaverun ]]; then
		gfence="$("$FENCE_TOOL" "$PF" raise "$SRC_VF" eth 2>"$W/fence.err")" \
			|| fail "raise fence with eth: $(cat "$W/fence.err")"
		grep -q '^mlx5e suspended' "$W/fence.err" || fail "fence raise: $(cat "$W/fence.err")"
		echo "$gfence" >"$W/gate_fence"
		note "gate fence up with mlx5e suspended (table/group $gfence); netdev $(ls /sys/bus/pci/devices/$SRC_BDF/net 2>/dev/null || echo gone), port $(port_state "$SRC_IB"), GIDs $(gid_table "$SRC_IB" | wc -l)"
		st="$(holder_status USR2 "$HOLDER_PID")"
		note "QP after suspend: $st"
		echo "$st" >"$W/qp_after_unbind"
	fi
	[[ "$MODE" == leaverun ]] && leave=(--leave-running)

	kmsg "mode=$MODE dump"
	"$CRIU" dump -t "$HOLDER_PID" -D "$IMG" -v4 -o dump.log --shell-job -L "$W/plugins" "${leave[@]}" \
		>/dev/null 2>&1 || { tail -40 "$IMG/dump.log"; fail "criu dump"; }
	grep -qaE 'fini-DUMP drain: .*records_written=[1-9][0-9]* failed=0' "$IMG/dump.log" \
		|| fail "SAVE drain wrote no record"
	note "criu dump ok, image dir $(du -sb "$IMG" | cut -f1) bytes"

	if [[ "$MODE" == leaverun ]]; then
		# The plugin leaves both fences up after --leave-running; lift
		# ours with eth (resumes mlx5e first), then the plugin's, as the
		# plugin would have to.
		"$FENCE_TOOL" "$PF" lift "$SRC_VF" $gfence eth || fail "lift gate fence with eth ($gfence)"
		wait_active "$SRC_IB" || fail "source port not ACTIVE after resume: $(port_state "$SRC_IB")"
		local fh
		fh="$(grep -aoE "fenced RoCE RX pf=$PF vf_id=$SRC_VF .*table 0x[0-9a-f]+ group 0x[0-9a-f]+" "$IMG/dump.log" \
			| tail -1 | sed -E 's/.*table (0x[0-9a-f]+) group (0x[0-9a-f]+)/\1 \2/')"
		[[ -n "$fh" ]] || fail "no fence handles for vf $SRC_VF in dump.log"
		"$FENCE_TOOL" "$PF" lift "$SRC_VF" $fh || fail "lift plugin fence ($fh)"
		note "lifted gate fence (mlx5e resumed) and plugin fence (table/group $fh)"
		note "mlx5e resumed; IP GID index $(find_gid_index "$SRC_IB") (was $IDX)"
		kill -0 "$HOLDER_PID" 2>/dev/null || fail "source holder did not keep running"
		out="$(holder_status USR1 "$HOLDER_PID")"
		note "source QP check after --leave-running: $out"
		kill -TERM "$HOLDER_PID" 2>/dev/null
		HOLDER_PID=""
		kmsg "mode=$MODE end"
		[[ "$out" == OK ]] || fail "source QP after --leave-running: $out"
		exit 0
	fi
	HOLDER_PID=""

	sect "[$MODE] destination VF $DST_VF"
	echo 0 >"$PF_DEV/sriov_numvfs"
	sleep 0.5
	echo 0 >"$PF_DEV/sriov_drivers_autoprobe"
	echo "$NUMVFS" >"$PF_DEV/sriov_numvfs"
	DEST_BDF="$(vf_bdf "$DST_VF")"
	ensure_dma_domain "$DEST_BDF"
	if [[ "$DST_MAC" != none ]]; then
		ip link set "$PF_NET" vf "$DST_VF" mac "$DST_MAC" || fail "set dest VF MAC"
	fi
	note "dest admin MAC set to $DST_MAC (source $MAC)"
	"$VFMIG_TOOL" "$PF" set_tracked "$DST_VF" 1 >/dev/null || fail "dest set_tracked"
	"$VFMIG_TOOL" "$PF" set_vf_uuid "$DST_VF" "$UUID" >/dev/null || fail "dest set_vf_uuid"
	kmsg "mode=$MODE prerestore"
	"$RESTORE_TOOL" -D "$IMG" -p "$MLX5_SO" >"$W/fw_restore.log" 2>&1 \
		|| { tail -20 "$W/fw_restore.log"; fail "prerestore tool"; }
	DEST_IB="$(wait_ibdev "$DEST_BDF")" || fail "no dest ibdev"
	NET="$(wait_netdev "$DEST_BDF")" || fail "no dest netdev"
	configure_ip "$NET" || fail "ip on dest $NET"
	wait_active "$DEST_IB" || fail "dest port not ACTIVE: $(port_state "$DEST_IB")"
	DIDX="$(find_gid_index "$DEST_IB")" || fail "no RoCEv2 GID for $IP on dest"
	gid_table "$DEST_IB" >"$W/gids.dst"
	note "bdf=$DEST_BDF ib=$DEST_IB net=$NET mac=$(cat /sys/class/net/$NET/address) gid_index=$DIDX"
	sed 's/^/    /' "$W/gids.dst"
	# ndev names differ between the two VFs; compare index, value and type.
	if diff <(awk '{print $1,$2,$3,$4}' "$W/gids.src") \
		<(awk '{print $1,$2,$3,$4}' "$W/gids.dst") >/dev/null; then
		note "GID table: same indexes, values and types as the source"
	else
		note "GID table differs from the source:"
		diff "$W/gids.src" "$W/gids.dst" | sed 's/^/    /'
	fi
	[[ "$DIDX" == "$IDX" ]] || fail "IP GID at index $DIDX on dest, $IDX on source"

	kmsg "mode=$MODE restore"
	"$CRIU" restore -D "$IMG" -v4 -o restore.log -d --pidfile "$W/restored.pid" --shell-job \
		-L "$W/plugins" >/dev/null 2>&1 || { tail -40 "$IMG/restore.log"; fail "criu restore"; }
	RESTORED_PID="$(cat "$W/restored.pid")"
	note "criu restore ok (pid=$RESTORED_PID)"
	out="$(holder_status USR1 "$RESTORED_PID")"
	note "restored QP check: $out"
	if [[ -n "$gfence" ]]; then
		# The restore lifted only the plugin's fence; ours came along in
		# the image as a table that is no longer the root.
		"$FENCE_TOOL" "$PF" lift "$DST_VF" $gfence || note "lift carried gate fence on dest failed"
	fi
	kill -TERM "$RESTORED_PID" 2>/dev/null
	RESTORED_PID=""
	kmsg "mode=$MODE end"
	[[ "$out" == OK ]] || fail "restored QP: $out"
	exit 0
}

dmesg_since() {
	dmesg | awk -v m="dropeth_gate: mode=$1 begin" \
		'{l[NR] = $0} index($0, m) {s = NR} END {for (i = s; s && i <= NR; i++) print l[i]}' \
		| grep -E 'vfmig|syndrome|Fatal|error|WARN|mlx5.*fail' | grep -v dropeth_gate | tail -40
}

for MODE in $MODES; do
	mkdir -p "$WORKROOT/$MODE"
	if (run_mode); then RESULT[$MODE]=PASS; else RESULT[$MODE]=FAIL; fi
	DETAIL[$MODE]="$(cat "$WORKROOT/$MODE/detail" 2>/dev/null)"
	DETAIL[$MODE.qp_after_unbind]="$(cat "$WORKROOT/$MODE/qp_after_unbind" 2>/dev/null)"
	sect "[$MODE] kernel messages"
	dmesg_since "$MODE" >"$WORKROOT/$MODE/dmesg.txt"
	sed 's/^/  /' "$WORKROOT/$MODE/dmesg.txt"
done

sect "Summary (SRC_VF=$SRC_VF DST_VF=$DST_VF, workdir $WORKROOT)"
rc=0
for MODE in $MODES; do
	printf '  %-9s %s %s\n' "$MODE" "${RESULT[$MODE]}" "${DETAIL[$MODE]:-}"
	[[ -n "${DETAIL[$MODE.qp_after_unbind]:-}" ]] \
		&& printf '  %-9s QP after ETH unbind: %s\n' "" "${DETAIL[$MODE.qp_after_unbind]}"
	[[ "${RESULT[$MODE]}" == PASS ]] || rc=1
done
exit $rc
