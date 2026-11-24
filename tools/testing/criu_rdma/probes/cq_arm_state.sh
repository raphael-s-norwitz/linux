#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Does a CQ's arm state survive SAVE/LOAD?
#
# cq_arm_holder leaves four CQs in known states on the source VF:
#
#   A  armed            expect cqc.st 0x9
#   B  solicited armed  expect 0x6
#   C  armed, fired     expect 0xa
#   D  never armed      whatever the source reads
#
# The script reads each CQ's state from mlx5 debugfs
# (CQs/0x<cqn>/state, a firmware QUERY_CQ per read) on the source before
# criu dump, and on the destination after criu restore, before any
# traffic. It passes if every CQ reads the same on both sides and A, B
# and C read as expected on the source. A mismatch on A or B means a
# restored process waiting on that CQ would not be woken.
#
# Needs the mlx5_core debugfs "state" CQ field and debugfs mounted.
#
#   sudo CRIU_REPO=/path/to/criu tools/testing/criu_rdma/probes/cq_arm_state.sh
#   sudo CRIU_REPO=/path/to/criu DST_VF=0 tools/testing/criu_rdma/probes/cq_arm_state.sh
#
# The first runs VF 0 -> VF 1, the second the same slot.
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
HOLDER="${HOLDER:-$HERE/cq_arm_holder}"

PF="${PF:-0000:08:00.0}"
SRC_VF="${SRC_VF:-0}"
DST_VF="${DST_VF:-1}"
IP="${IP:-192.168.252.10}"
MAC="${MAC:-02:00:00:fc:00:10}"
DBG=/sys/kernel/debug/mlx5

PF_DEV="/sys/bus/pci/devices/$PF"
PF_NET="$(ls "$PF_DEV/net" 2>/dev/null | head -1)"
NUMVFS=$(( (SRC_VF > DST_VF ? SRC_VF : DST_VF) + 1 ))
W="${WORKDIR:-/var/tmp/cq_arm_state.$(date +%Y%m%d-%H%M%S)}"
STATUS="$W/holder.status"
IMG="$W/img"
HOLDER_PID=""
RESTORED_PID=""

note() { printf '  %s\n' "$*"; }
sect() { printf '\n== %s ==\n' "$*"; }
fail() { echo "FAIL: $*"; exit 1; }

cleanup() {
	[[ -n "$HOLDER_PID" ]] && kill -KILL "$HOLDER_PID" 2>/dev/null
	[[ -n "$RESTORED_PID" ]] && kill -KILL "$RESTORED_PID" 2>/dev/null
	echo 0 >"$PF_DEV/sriov_numvfs" 2>/dev/null
	echo 1 >"$PF_DEV/sriov_drivers_autoprobe" 2>/dev/null
}
trap cleanup EXIT

[[ "$EUID" -eq 0 ]] || { echo "run as root"; exit 2; }
for f in "$MLX5_SO" "$RESTORE_TOOL" "$CRIU" "$VFMIG_TOOL" "$HOLDER"; do
	[[ -e "$f" ]] || { echo "missing $f"; exit 2; }
done
[[ -d "$DBG" ]] || { echo "$DBG missing (debugfs not mounted?)"; exit 2; }
[[ -n "$PF_NET" ]] || { echo "PF $PF has no netdev (needed to set the VF MAC)"; exit 2; }
echo 1 >/sys/bus/auxiliary/drivers_autoprobe
mkdir -p "$W/plugins" "$IMG"
ln -sf "$MLX5_SO" "$W/plugins/"

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
holder_status() {	# <signal> <pid>
	local s
	: >"$STATUS"
	kill "-$1" "$2" 2>/dev/null || return 1
	for _ in $(seq 1 100); do
		s="$(cat "$STATUS" 2>/dev/null)"
		[[ -n "$s" ]] && { echo "$s"; return 0; }
		sleep 0.1
	done
	echo "(no answer)"
}

# states <bdf> <file>: write "<name> <cqn> <state>" for each CQ.
states() {
	local name f
	: >"$2"
	for name in A B C D; do
		f="$DBG/$1/CQs/${CQN[$name]}/state"
		echo "$name ${CQN[$name]} $(cat "$f" 2>/dev/null || echo missing)" >>"$2"
	done
}

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
[[ -d "$DBG/$SRC_BDF/CQs" ]] || fail "$DBG/$SRC_BDF/CQs missing"

systemd-run --scope --quiet --unit="cq-arm-holder-$$" \
	bash -c "exec </dev/null >'$W/holder.log' 2>&1; exec '$HOLDER' '$SRC_IB' '$STATUS' '$IDX'" &
disown "$!" 2>/dev/null
for _ in $(seq 1 100); do
	st="$(cat "$STATUS" 2>/dev/null)"
	[[ -n "$st" ]] && break
	sleep 0.1
done
[[ "$st" == READY* ]] || { cat "$W/holder.log"; fail "holder: ${st:-no status}"; }
HOLDER_PID="$(pgrep -fx "$HOLDER $SRC_IB $STATUS $IDX" | head -1)"
[[ -n "$HOLDER_PID" ]] || fail "holder pid not found"
declare -A CQN
for kv in ${st#READY }; do CQN[${kv%%=*}]="${kv#*=}"; done
note "holder pid=$HOLDER_PID: $st"

states "$SRC_BDF" "$W/states.src"
[[ -e "$DBG/$SRC_BDF/CQs/${CQN[A]}/state" ]] ||
	fail "no debugfs state field for CQ ${CQN[A]} (kernel without the CQ state field?)"

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
note "bdf=$DEST_BDF ib=$DEST_IB"

"$CRIU" restore -D "$IMG" -v4 -o restore.log -d --pidfile "$W/restored.pid" --shell-job \
	-L "$W/plugins" >/dev/null 2>&1 || { tail -40 "$IMG/restore.log"; fail "criu restore"; }
RESTORED_PID="$(cat "$W/restored.pid")"
note "criu restore ok (pid=$RESTORED_PID)"
states "$DEST_BDF" "$W/states.dst"
out="$(holder_status USR1 "$RESTORED_PID")"
note "restored holder polls every CQ: $out"
kill -TERM "$RESTORED_PID" 2>/dev/null
RESTORED_PID=""

# ---------------------------------------------------------------- answers
sect "states (workdir $W)"
printf '  %-3s %-8s %-8s %-8s %s\n' CQ cqn source dest expected
rc=0
declare -A WANT=([A]=0x9 [B]=0x6 [C]=0xa [D]=-)
while read -r name cqn s && read -r _ _ d <&3; do
	verdict=ok
	[[ "$s" == "$d" ]] || verdict=CHANGED
	[[ "${WANT[$name]}" == - || "$s" == "${WANT[$name]}" ]] ||
		verdict="$verdict, source unexpected"
	[[ "$verdict" == ok ]] || rc=1
	printf '  %-3s %-8s %-8s %-8s %-8s %s\n' "$name" "$cqn" "$s" "$d" "${WANT[$name]}" \
		"$verdict"
done <"$W/states.src" 3<"$W/states.dst"
[[ "$out" == OK ]] || { note "restored holder check: $out"; rc=1; }
echo
[[ $rc -eq 0 ]] && echo PASS || echo FAIL
exit $rc
