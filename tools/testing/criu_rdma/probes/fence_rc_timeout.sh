#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# What can still complete on a VF while its RoCE RX fence is up, as it is
# on a migration destination between LOAD and RESUME_IBDEV? On PF $PF,
# with VF 0 fenced and VF 1 as a peer (both on one host):
#
#   sender  VF 0 sends to VF 1: the SEND arrives but its ACK is dropped,
#           as for an RC send left unacknowledged at SAVE. Does the
#           sender get only an error completion, or an async QP event
#           too?
#   peer    VF 1 sends to VF 0: the SEND is dropped. Same question for
#           VF 1, and whether VF 0 sees anything.
#   loop    VF 0 sends to itself, between two contexts. Does loopback
#           bypass the fence?
#
# The async events matter because on a destination they would arrive
# before RESTORE_QP registers the QP, and mlx5 drops them.
#
#   sudo tools/testing/criu_rdma/probes/fence_rc_timeout.sh
#
# Functional only; no timing. SR-IOV on $PF is reset at the end.

set -uo pipefail

PF="${PF:-0000:08:00.0}"
NS_B=fence_rc_peer
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VFMIG_TOOL="${VFMIG_TOOL:-$HERE/../tools/mlx5_vfmig}"
RC_PAIR="$HERE/rc_pair"
FENCE="$HERE/vfmig_fence"
IP_A=192.168.251.10 IP_B=192.168.251.11
MAC_A=02:00:00:fb:00:10 MAC_B=02:00:00:fb:00:11

PF_DEV="/sys/bus/pci/devices/$PF"
PF_NET="$(ls "$PF_DEV/net" | head -1)"

note() { printf '  %s\n' "$*"; }
sect() { printf '\n== %s ==\n' "$*"; echo "fence_rc_timeout: phase $*" >/dev/kmsg; }

cleanup() {
	[[ -n "${FT:-}" ]] && "$FENCE" "$PF" lift 0 "$FT" "$FG" 2>/dev/null
	[[ -n "${RC_PID:-}" ]] && kill "$RC_PID" 2>/dev/null
	[[ -n "${LB_PID:-}" ]] && kill "$LB_PID" 2>/dev/null
	ip netns del "$NS_B" 2>/dev/null
	echo 0 >"$PF_DEV/sriov_numvfs" 2>/dev/null
	echo 1 >"$PF_DEV/sriov_drivers_autoprobe" 2>/dev/null
}
trap cleanup EXIT

[[ "$EUID" -eq 0 ]] || { echo "run as root"; exit 2; }
for f in "$RC_PAIR" "$FENCE" "$VFMIG_TOOL"; do [[ -x "$f" ]] || { echo "missing $f"; exit 2; }; done
echo 1 >/sys/bus/auxiliary/drivers_autoprobe

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
gid_index() {	# <ibdev> <ip>
	local IFS=. p="/sys/class/infiniband/$1/ports/1" want g i
	set -- "$1" $2
	want="$(printf '0000:0000:0000:0000:0000:ffff:%02x%02x:%02x%02x' "$2" "$3" "$4" "$5")"
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

# rc|lb <command>: send one command to that rc_pair, print and keep its answer.
LAST=""
ask() {
	local -n co="$1"
	shift
	echo "$*" >&"${co[1]}"
	read -r -t 30 LAST <&"${co[0]}" || LAST="(no answer from rc_pair)"
	note "$LAST"
}
wait_ready() {
	local -n co="$1"
	read -r -t 10 LAST <&"${co[0]}"
	[[ "$LAST" == ready ]] || { echo "rc_pair $1: $LAST"; exit 1; }
}

# ---------------------------------------------------------------- setup
sect setup
echo 0 >"$PF_DEV/sriov_numvfs"
echo 0 >"$PF_DEV/sriov_drivers_autoprobe"
echo 2 >"$PF_DEV/sriov_numvfs"
BDF_A="$(vf_bdf 0)" BDF_B="$(vf_bdf 1)"
for b in "$BDF_A" "$BDF_B"; do
	t="/sys/bus/pci/devices/$b/iommu_group/type"
	case "$(cat "$t")" in DMA|DMA-FQ) ;; *) echo DMA >"$t" ;; esac
done
ip link set "$PF_NET" vf 0 mac "$MAC_A"
ip link set "$PF_NET" vf 1 mac "$MAC_B"
"$VFMIG_TOOL" "$PF" set_tracked 0 1 >/dev/null
"$VFMIG_TOOL" "$PF" enable_migratable 0 >/dev/null
for b in "$BDF_A" "$BDF_B"; do
	echo mlx5_core >"/sys/bus/pci/devices/$b/driver_override"
	echo "$b" >/sys/bus/pci/drivers/mlx5_core/bind
done
IB_A="$(wait_ibdev "$BDF_A")" IB_B="$(wait_ibdev "$BDF_B")"
NET_A="$(wait_netdev "$BDF_A")" NET_B="$(wait_netdev "$BDF_B")"
[[ -n "$IB_A" && -n "$IB_B" && -n "$NET_A" && -n "$NET_B" ]] ||
	{ echo "VFs did not come up"; exit 1; }
ip link set "$NET_A" up && ip addr add "$IP_A/24" dev "$NET_A"
ip netns del "$NS_B" 2>/dev/null
ip netns add "$NS_B"
ip link set "$NET_B" netns "$NS_B"
ip -n "$NS_B" link set "$NET_B" up && ip -n "$NS_B" addr add "$IP_B/24" dev "$NET_B"
wait_active "$IB_A" && wait_active "$IB_B" || { echo "ports not ACTIVE"; exit 1; }
GIDX_A="$(gid_index "$IB_A" "$IP_A")" GIDX_B="$(gid_index "$IB_B" "$IP_B")"
note "A: vf 0 $BDF_A $IB_A gid=$GIDX_A; B: vf 1 $BDF_B $IB_B (netns $NS_B) gid=$GIDX_B"
ping -c 2 -W 2 -I "$NET_A" "$IP_B" >/dev/null && note "ping A -> B ok" || note "ping A -> B FAILED"

coproc RC { exec "$RC_PAIR" "$IB_A" "$GIDX_A" "$IB_B" "$GIDX_B" 2>&1; }
RC_PID=$RC_PID
wait_ready RC
coproc LB { exec "$RC_PAIR" "$IB_A" "$GIDX_A" "$IB_A" "$GIDX_A" 2>&1; }
LB_PID=$LB_PID
wait_ready LB

# Connecting is local (no CM), so every pair is connected before the fence.
for n in 0 1 2 3; do ask RC connect $n; done
for n in 0 1 2; do ask LB connect $n; done

sect baseline
ask RC send 0 AB
BASE_RC="$LAST"
ask RC send 0 BA
ask LB send 0 AB
BASE_LB="$LAST"
ask RC async A
ask RC async B
ask LB async A

sect fence
read -r FT FG < <("$FENCE" "$PF" raise 0) || { echo "fence raise failed"; FT=""; exit 1; }
note "fence raised on vf 0: table=$FT group=$FG"

sect sender
note "-- A sends to B, B's ACK is dropped at A"
ask RC send 1 AB
SENDER="$LAST"
ask RC async A
SENDER_ASYNC_A="$LAST"
ask RC async B
SENDER_ASYNC_B="$LAST"

sect peer
note "-- B sends to A, the SEND is dropped at A"
ask RC send 2 BA
PEER="$LAST"
ask RC async B
PEER_ASYNC_B="$LAST"
ask RC async A
PEER_ASYNC_A="$LAST"

sect loop
note "-- A sends to itself through a second context"
ask LB send 1 AB
LOOP="$LAST"
ask LB async A
LOOP_ASYNC_A="$LAST"
ask LB async B

sect lift
if "$FENCE" "$PF" lift 0 "$FT" "$FG"; then
	FT=""
	note "fence lifted"
else
	note "fence lift FAILED"
fi
ask RC send 3 AB
ask RC send 3 BA
ask LB send 2 AB

echo quit >&"${RC[1]}"
echo quit >&"${LB[1]}"
wait "$RC_PID" "$LB_PID" 2>/dev/null
RC_PID="" LB_PID=""

echo
echo "== answers"
[[ "$BASE_RC" == *"sender=ok receiver=ok"* ]] ||
	echo "  baseline A->B failed, results below are not meaningful: $BASE_RC"
echo "  sender: $SENDER"
echo "          A: $SENDER_ASYNC_A"
echo "          B: $SENDER_ASYNC_B"
echo "  peer:   $PEER"
echo "          B: $PEER_ASYNC_B"
echo "          A: $PEER_ASYNC_A"
if [[ "$BASE_LB" != *"sender=ok receiver=ok"* ]]; then
	echo "  loop:   baseline loopback failed, no answer: $BASE_LB"
elif [[ "$LOOP" == *"sender=ok receiver=ok"* ]]; then
	echo "  loop:   loopback BYPASSES the fence: $LOOP"
else
	echo "  loop:   loopback is fenced: $LOOP"
fi
echo "          $LOOP_ASYNC_A"
echo
echo "done; SR-IOV on $PF is reset on exit"
