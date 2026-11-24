#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Is a completion event that firmware raises between LOAD and RESTORE_CQ
# delivered at the CQ's adoption?
#
# cq_early_holder arms two CQs on the source VF. On E, a self-loop RC QP
# retries a send for ever against its own RNR NAKs; Q gets no traffic.
# After criu dump, the destination VF comes up fenced, so the QP's
# loopback traffic is dropped and the send fails after its ACK timeouts,
# writing an error CQE to E and raising E's completion EQE before CRIU
# has restored any CQ. The script waits WAIT_S seconds after LOAD for
# that, then runs criu restore.
#
# First, as a control without a migration, a second holder runs on the
# source while the script raises the fence there with vfmig_fence for
# WAIT_S seconds: its send must fail.
#
# It passes if:
#   - the control's send failed behind the live fence;
#   - mlx5_core_adopt_cq() logs delivering the early event for E (its
#     debug line, enabled here through dynamic debug), and not for Q;
#   - the restored holder finds the failed send on E and nothing on Q;
#   - E reads fired (0xa) and Q armed (0x9) in mlx5 debugfs;
#   - unbinding the destination VF warns of no CQ left unrestored.
#
# When a send has not failed, the holder posts the receive it waits for:
# a send that then completes was still being retried. The restored QP's
# state is also printed.
#
# If the send fails on the source, between the dump's fence and SAVE,
# there is nothing to replay; the script then says so and fails. Raise
# TIMEOUT (the QP's ACK timeout exponent) if that happens.
#
# Needs dynamic debug, the mlx5_core debugfs "state" CQ field and
# debugfs mounted.
#
#   sudo CRIU_REPO=/path/to/criu tools/testing/criu_rdma/probes/cq_early_replay.sh
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
HOLDER="${HOLDER:-$HERE/cq_early_holder}"
FENCE="${FENCE:-$HERE/vfmig_fence}"

PF="${PF:-0000:08:00.0}"
SRC_VF="${SRC_VF:-0}"
DST_VF="${DST_VF:-1}"
IP="${IP:-192.168.252.10}"
MAC="${MAC:-02:00:00:fc:00:10}"
TIMEOUT="${TIMEOUT:-16}"
WAIT_S="${WAIT_S:-5}"
DBG=/sys/kernel/debug/mlx5
DYNDBG=/sys/kernel/debug/dynamic_debug/control
REPLAY_MSG="delivering the completion event CQ"

PF_DEV="/sys/bus/pci/devices/$PF"
PF_NET="$(ls "$PF_DEV/net" 2>/dev/null | head -1)"
NUMVFS=$(( (SRC_VF > DST_VF ? SRC_VF : DST_VF) + 1 ))
W="${WORKDIR:-/var/tmp/cq_early_replay.$(date +%Y%m%d-%H%M%S)}"
STATUS="$W/holder.status"
IMG="$W/img"
MARK="cq_early_replay.$$"
HOLDER_PID=""
RESTORED_PID=""
CONTROL_PID=""
FT=""

note() { printf '  %s\n' "$*"; }
sect() { printf '\n== %s ==\n' "$*"; }
fail() { echo "FAIL: $*"; exit 1; }

cleanup() {
	[[ -n "$FT" ]] && "$FENCE" "$PF" lift "$SRC_VF" "$FT" "$FG" 2>/dev/null
	[[ -n "$CONTROL_PID" ]] && kill -KILL "$CONTROL_PID" 2>/dev/null
	[[ -n "$HOLDER_PID" ]] && kill -KILL "$HOLDER_PID" 2>/dev/null
	[[ -n "$RESTORED_PID" ]] && kill -KILL "$RESTORED_PID" 2>/dev/null
	echo 0 >"$PF_DEV/sriov_numvfs" 2>/dev/null
	echo 1 >"$PF_DEV/sriov_drivers_autoprobe" 2>/dev/null
	echo "func mlx5_core_adopt_cq -p" >"$DYNDBG" 2>/dev/null
}
trap cleanup EXIT

[[ "$EUID" -eq 0 ]] || { echo "run as root"; exit 2; }
for f in "$MLX5_SO" "$RESTORE_TOOL" "$CRIU" "$VFMIG_TOOL" "$HOLDER" "$FENCE"; do
	[[ -e "$f" ]] || { echo "missing $f"; exit 2; }
done
[[ -d "$DBG" ]] || { echo "$DBG missing (debugfs not mounted?)"; exit 2; }
[[ -w "$DYNDBG" ]] || { echo "$DYNDBG missing (no dynamic debug?)"; exit 2; }
[[ -n "$PF_NET" ]] || { echo "PF $PF has no netdev (needed to set the VF MAC)"; exit 2; }
echo "func mlx5_core_adopt_cq +p" >"$DYNDBG" ||
	{ echo "mlx5_core_adopt_cq not in dynamic debug (mlx5_core not loaded?)"; exit 2; }
echo 1 >/sys/bus/auxiliary/drivers_autoprobe
mkdir -p "$W/plugins" "$IMG"
ln -sf "$MLX5_SO" "$W/plugins/"
echo "$MARK" >/dev/kmsg

ensure_dma_domain() {
	local t="/sys/bus/pci/devices/$1/iommu_group/type"
	case "$(cat "$t")" in DMA|DMA-FQ) ;; *) echo DMA >"$t" ;; esac
}
vf_bdf() { readlink "$PF_DEV/virtfn$1" | xargs basename; }
wait_ibdev() {
	local ib
	for _ in $(seq 1 100); do
		for ib in /sys/class/infiniband/*; do
			[[ "$(basename "$(readlink -f "$ib/device")")" == "$1" ]] &&
				{ basename "$ib"; return 0; }
		done
		sleep 0.1
	done
	return 1
}
wait_netdev() {
	for _ in $(seq 1 100); do
		ls "/sys/bus/pci/devices/$1/net" 2>/dev/null | head -1 | grep . && return 0
		sleep 0.1
	done
	return 1
}
wait_active() {
	for _ in $(seq 1 100); do
		[[ "$(cat "/sys/class/infiniband/$1/ports/1/state" 2>/dev/null)" == *ACTIVE* ]] &&
			return 0
		sleep 0.1
	done
	return 1
}
gid_index() {	# <ibdev>
	local IFS=. p="/sys/class/infiniband/$1/ports/1" want g i
	set -- $IP
	want="$(printf '0000:0000:0000:0000:0000:ffff:%02x%02x:%02x%02x' "$1" "$2" "$3" "$4")"
	for _ in $(seq 1 100); do
		for g in "$p"/gids/*; do
			i="${g##*/}"
			[[ "$(cat "$g" 2>/dev/null)" == "$want" &&
			   "$(cat "$p/gid_attrs/types/$i" 2>/dev/null)" == "RoCE v2" ]] &&
				{ echo "$i"; return 0; }
		done
		sleep 0.1
	done
	return 1
}
holder_status() {	# <status file> <signal> <pid>; SIGUSR2 polls for up to 10 s
	local s
	: >"$1"
	kill "-$2" "$3" 2>/dev/null || return 1
	for _ in $(seq 1 150); do
		s="$(cat "$1" 2>/dev/null)"
		[[ -n "$s" ]] && { echo "$s"; return 0; }
		sleep 0.1
	done
	echo "(no answer)"
}
# start_holder <status file> <log>: run a holder on the source and print
# "<pid> <READY line>".
start_holder() {
	local st pid
	systemd-run --scope --quiet --unit="cq-early-holder-$$-${1##*/}" \
		bash -c "exec </dev/null >'$2' 2>&1; exec '$HOLDER' '$SRC_IB' '$1' '$IDX' '$TIMEOUT'" &
	disown "$!" 2>/dev/null
	for _ in $(seq 1 100); do
		st="$(cat "$1" 2>/dev/null)"
		[[ -n "$st" ]] && break
		sleep 0.1
	done
	[[ "$st" == READY* ]] || { cat "$2" >&2; echo "holder: ${st:-no status}" >&2; return 1; }
	pid="$(pgrep -fx "$HOLDER $SRC_IB $1 $IDX $TIMEOUT" | head -1)"
	[[ -n "$pid" ]] || { echo "holder pid not found" >&2; return 1; }
	echo "$pid $st"
}
# Kernel log since this run started.
klog() { dmesg | sed -n "/$MARK/,\$p"; }

# ---------------------------------------------------------------- source
sect "source VF $SRC_VF"
echo 0 >"$PF_DEV/sriov_numvfs"
echo 0 >"$PF_DEV/sriov_drivers_autoprobe"
echo "$NUMVFS" >"$PF_DEV/sriov_numvfs"
SRC_BDF="$(vf_bdf "$SRC_VF")"
ensure_dma_domain "$SRC_BDF"
UUID="$(cat /proc/sys/kernel/random/uuid)"
ip link set "$PF_NET" vf "$SRC_VF" mac "$MAC" || fail "set source VF MAC"
"$VFMIG_TOOL" "$PF" set_tracked "$SRC_VF" 1 >/dev/null || fail set_tracked
"$VFMIG_TOOL" "$PF" enable_migratable "$SRC_VF" >/dev/null || fail enable_migratable
"$VFMIG_TOOL" "$PF" set_vf_uuid "$SRC_VF" "$UUID" >/dev/null || fail set_vf_uuid
echo mlx5_core >"/sys/bus/pci/devices/$SRC_BDF/driver_override"
echo "$SRC_BDF" >/sys/bus/pci/drivers/mlx5_core/bind
SRC_IB="$(wait_ibdev "$SRC_BDF")" || fail "no source ibdev"
NET="$(wait_netdev "$SRC_BDF")" || fail "no source netdev"
ip link set "$NET" up && ip addr add "$IP/24" dev "$NET" || fail "ip on source $NET"
wait_active "$SRC_IB" || fail "source port not ACTIVE"
IDX="$(gid_index "$SRC_IB")" || fail "no RoCEv2 GID for $IP on source"
note "bdf=$SRC_BDF ib=$SRC_IB net=$NET gid_index=$IDX"

# Control, without a migration: does the fence fail the send at all?
r="$(start_holder "$W/control.status" "$W/control.log")" || fail "control holder"
CONTROL_PID="${r%% *}"
note "control holder pid=$CONTROL_PID: ${r#* }"
read -r FT FG < <("$FENCE" "$PF" raise "$SRC_VF") && [[ -n "$FT" ]] ||
	{ FT=""; fail "fence raise"; }
sleep "$WAIT_S"
control="$(holder_status "$W/control.status" USR1 "$CONTROL_PID")"
"$FENCE" "$PF" lift "$SRC_VF" "$FT" "$FG" >/dev/null && FT="" || fail "fence lift"
[[ "$control" == OK* ]] ||
	control="$control; $(holder_status "$W/control.status" USR2 "$CONTROL_PID")"
kill -TERM "$CONTROL_PID" 2>/dev/null
CONTROL_PID=""
note "control, ${WAIT_S}s behind a live fence: $control"

r="$(start_holder "$STATUS" "$W/holder.log")" || fail "holder"
HOLDER_PID="${r%% *}"
st="${r#* }"
declare -A ID
for kv in ${st#READY }; do ID[${kv%%=*}]="${kv#*=}"; done
note "holder pid=$HOLDER_PID: $st"
note "source states: E $(cat "$DBG/$SRC_BDF/CQs/${ID[E]}/state" 2>/dev/null || echo ?)" \
	"Q $(cat "$DBG/$SRC_BDF/CQs/${ID[Q]}/state" 2>/dev/null || echo ?)"

"$CRIU" dump -t "$HOLDER_PID" -D "$IMG" -v4 -o dump.log --shell-job -L "$W/plugins" \
	>/dev/null 2>&1 || { tail -40 "$IMG/dump.log"; fail "criu dump"; }
HOLDER_PID=""
note "criu dump ok"

# ---------------------------------------------------------------- destination
sect "destination VF $DST_VF"
echo 0 >"$PF_DEV/sriov_numvfs"
sleep 0.5
echo 0 >"$PF_DEV/sriov_drivers_autoprobe"
echo "$NUMVFS" >"$PF_DEV/sriov_numvfs"
DEST_BDF="$(vf_bdf "$DST_VF")"
ensure_dma_domain "$DEST_BDF"
ip link set "$PF_NET" vf "$DST_VF" mac "$MAC" || fail "set dest VF MAC"
"$VFMIG_TOOL" "$PF" set_tracked "$DST_VF" 1 >/dev/null || fail "dest set_tracked"
"$VFMIG_TOOL" "$PF" set_vf_uuid "$DST_VF" "$UUID" >/dev/null || fail "dest set_vf_uuid"
"$RESTORE_TOOL" -D "$IMG" -p "$MLX5_SO" >"$W/fw_restore.log" 2>&1 ||
	{ tail -20 "$W/fw_restore.log"; fail "prerestore tool"; }
DEST_IB="$(wait_ibdev "$DEST_BDF")" || fail "no dest ibdev"
note "bdf=$DEST_BDF ib=$DEST_IB; waiting ${WAIT_S}s for the send to fail behind the fence"
sleep "$WAIT_S"

"$CRIU" restore -D "$IMG" -v4 -o restore.log -d --pidfile "$W/restored.pid" --shell-job \
	-L "$W/plugins" >/dev/null 2>&1 || { tail -40 "$IMG/restore.log"; fail "criu restore"; }
RESTORED_PID="$(cat "$W/restored.pid")"
note "criu restore ok (pid=$RESTORED_PID)"
E_ST="$(cat "$DBG/$DEST_BDF/CQs/${ID[E]}/state" 2>/dev/null || echo missing)"
Q_ST="$(cat "$DBG/$DEST_BDF/CQs/${ID[Q]}/state" 2>/dev/null || echo missing)"
QP_ST="$(cat "$DBG/$DEST_BDF/QPs/${ID[QP]}/state" 2>/dev/null || echo missing)"
out="$(holder_status "$STATUS" USR1 "$RESTORED_PID")"
kick=""
[[ "$out" == OK* ]] || kick="$(holder_status "$STATUS" USR2 "$RESTORED_PID")"
kill -TERM "$RESTORED_PID" 2>/dev/null
RESTORED_PID=""
klog >"$W/klog.restore"
echo 0 >"$PF_DEV/sriov_numvfs"
klog >"$W/klog"

# ---------------------------------------------------------------- answers
sect "answers (workdir $W)"
rc=0
verdict() {	# <ok> <what>
	if [[ "$1" == 1 ]]; then note "ok    $2"; else note "WRONG $2"; rc=1; fi
}
verdict "$([[ "$control" == OK* ]] && echo 1)" "control: live fence fails the send: $control"
note "info  QP ${ID[QP]} state after restore: $QP_ST"
[[ -n "$kick" ]] && note "info  restored holder, receive posted: $kick"
grep "$DEST_BDF" "$W/klog.restore" |
	grep -E "before its adoption lost|Async event for bogus CQ|CQ error on CQN" |
	sed 's/^/        lost before adoption: /'
grep -q "$REPLAY_MSG ${ID[E]} " "$W/klog.restore" && e_replayed=1 || e_replayed=0
grep -q "$REPLAY_MSG ${ID[Q]} " "$W/klog.restore" && q_replayed=1 || q_replayed=0
verdict "$e_replayed" "E ${ID[E]}: early event delivered at adoption"
verdict "$((!q_replayed))" "Q ${ID[Q]}: no event delivered"
verdict "$([[ "$out" == OK* ]] && echo 1)" "restored holder: $out"
verdict "$([[ "$E_ST" == 0xa ]] && echo 1)" "E state after restore: $E_ST (want 0xa, fired)"
verdict "$([[ "$Q_ST" == 0x9 ]] && echo 1)" "Q state after restore: $Q_ST (want 0x9, armed)"
left="$(grep "$DEST_BDF" "$W/klog" | grep "never restored")"
verdict "$([[ -z "$left" ]] && echo 1)" "VF unbind: no CQ left unrestored"
[[ -n "$left" ]] && printf '        %s\n' "$left"
if [[ "$e_replayed" == 0 && "$out" == OK* ]]; then
	note "E's send failed but no early event was delivered: it probably failed on the"
	note "source before SAVE. Rerun with a larger TIMEOUT (now $TIMEOUT)."
fi
echo
[[ $rc -eq 0 ]] && echo PASS || echo FAIL
exit $rc
