#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Answers four questions about unbinding a VF's mlx5e around SAVE, on PF
# $PF with VF 0 as the subject and VF 1 as a peer (both on one host):
#
#   gid     With VF 0's mlx5e unbound (GIDs deleted), what happens to RC
#           traffic in each direction on QPs connected before the unbind?
#           And do those QPs work again after mlx5e is rebound and the IP
#           re-added (CRIU's rollback)?
#   fence   Does the RoCE RX fence drop traffic, stay up across an mlx5e
#           unbind and rebind, and lift afterwards?
#   recycle Does RECYCLE_VF keep the VF's admin MAC?
#   detach  With mlx5e suspended through the RX fence's F_ETH_SUSPEND
#           instead of unbound, do the netdev, IP and
#           GIDs survive, and do QPs connected before it work after resume?
#   suspend Can mlx5e unbind while the VF is SUSPEND_VHCA'd? Not in the
#           default PHASES: under SUSPEND(initiator) the netdev unregister
#           blocks on references for minutes and wedges the VF.
#
# VF 1's netdev lives in its own network namespace, so each side reaches
# the other's IP over the wire (in one namespace the peer's address is
# local and the QP's MAC resolution times out).
#
#   sudo tools/testing/criu_rdma/probes/eth_unbind_questions.sh
#   sudo PHASES="gid fence" tools/testing/criu_rdma/probes/eth_unbind_questions.sh
#
# Functional only; no timing. SR-IOV on $PF is reset at the end.

set -uo pipefail

PF="${PF:-0000:08:00.0}"
PHASES="${PHASES:-gid fence recycle}"
NS_B=vfq_peer
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VFMIG_TOOL="${VFMIG_TOOL:-$HERE/../tools/mlx5_vfmig}"
S="${WORKDIR:-$(mktemp -d /var/tmp/eth_unbind_questions.XXXXXX)}"
RC_PAIR="$HERE/rc_pair"
FENCE="$HERE/vfmig_fence"
IP_A=192.168.250.10 IP_B=192.168.250.11
MAC_A=02:00:00:fa:00:10 MAC_B=02:00:00:fa:00:11
SUSPEND_WAIT="${SUSPEND_WAIT:-120}"

PF_DEV="/sys/bus/pci/devices/$PF"
PF_NET="$(ls "$PF_DEV/net" | head -1)"
ETH_DRV=/sys/bus/auxiliary/drivers/mlx5_core.eth

note() { printf '  %s\n' "$*"; }
sect() { printf '\n== %s ==\n' "$*"; kmsg "phase $*"; }
kmsg() { echo "eth_unbind_questions: $*" >/dev/kmsg; }

cleanup() {
	[[ -n "${RC_PID:-}" ]] && kill "$RC_PID" 2>/dev/null
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
			[[ "$(basename "$(readlink -f "$ib/device")")" == "$1" ]] && { basename "$ib"; return 0; }
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
port_state() { cat "/sys/class/infiniband/$1/ports/1/state" 2>/dev/null; }
wait_active() {
	for _ in $(seq 1 100); do [[ "$(port_state "$1")" == *ACTIVE* ]] && return 0; sleep 0.1; done
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
			   "$(cat "$p/gid_attrs/types/$i" 2>/dev/null)" == "RoCE v2" ]] && { echo "$i"; return 0; }
		done
		sleep 0.1
	done
	return 1
}
gid_count() {
	local n=0 g
	for g in /sys/class/infiniband/$1/ports/1/gids/*; do
		[[ "$(cat "$g" 2>/dev/null)" != 0000:0000:0000:0000:0000:0000:0000:0000 ]] && n=$((n + 1))
	done
	echo "$n"
}
eth_aux() { basename "$(ls -d /sys/bus/pci/devices/$1/mlx5_core.eth.* | head -1)"; }

# Bring VF 0's netdev back after an unbind: rebind mlx5e, IP, port ACTIVE.
rebind_eth_a() {
	echo "$AUX_A" >"$ETH_DRV/bind" || { note "rebind $AUX_A failed"; return 1; }
	NET_A="$(wait_netdev "$BDF_A")" || { note "no netdev after rebind"; return 1; }
	ip link set "$NET_A" up && ip addr add "$IP_A/24" dev "$NET_A"
	wait_active "$IB_A" || note "port not ACTIVE after rebind: $(port_state "$IB_A")"
	local idx
	idx="$(gid_index "$IB_A" "$IP_A")"
	note "rebound $AUX_A: net=$NET_A port=$(port_state "$IB_A") IP GID index $idx (was $GIDX_A)"
	GIDX_A="$idx"
	[[ -n "${RC_PID:-}" ]] && rc gid A "$idx"
}
unbind_eth_a() {
	echo "$AUX_A" >"$ETH_DRV/unbind"
	note "unbound $AUX_A: netdev=$(ls /sys/bus/pci/devices/$BDF_A/net 2>/dev/null || echo none) port=$(port_state "$IB_A") gids=$(gid_count "$IB_A")"
}

rc() {
	local line
	echo "$*" >&"${RC[1]}"
	read -r -t 30 line <&"${RC[0]}" || line="(no answer from rc_pair)"
	note "$line"
}

phase_dmesg() {
	dmesg | awk -v m="eth_unbind_questions: phase $1" \
		'{l[NR] = $0} index($0, m) {s = NR} END {for (i = s + 1; s && i <= NR; i++) print l[i]}' \
		| grep -E '08:00\.[23]|vfmig|timeout|Fatal|WARNING' | grep -v 'arena reserved' | tail -30 \
		| sed 's/^/    dmesg: /'
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
[[ -n "$IB_A" && -n "$IB_B" && -n "$NET_A" && -n "$NET_B" ]] || { echo "VFs did not come up"; exit 1; }
ip link set "$NET_A" up && ip addr add "$IP_A/24" dev "$NET_A"
ip netns del "$NS_B" 2>/dev/null
ip netns add "$NS_B"
ip link set "$NET_B" netns "$NS_B"
ip -n "$NS_B" link set "$NET_B" up && ip -n "$NS_B" addr add "$IP_B/24" dev "$NET_B"
wait_active "$IB_A" && wait_active "$IB_B" || { echo "ports not ACTIVE"; exit 1; }
GIDX_A="$(gid_index "$IB_A" "$IP_A")" GIDX_B="$(gid_index "$IB_B" "$IP_B")"
AUX_A="$(eth_aux "$BDF_A")"
note "A: vf 0 $BDF_A $IB_A $NET_A mac=$(cat /sys/class/net/$NET_A/address) gid=$GIDX_A aux=$AUX_A"
note "B: vf 1 $BDF_B $IB_B $NET_B (netns $NS_B) mac=$(ip -n "$NS_B" -br link show "$NET_B" | awk '{print $3}') gid=$GIDX_B"
ping -c 2 -W 2 -I "$NET_A" "$IP_B" >/dev/null && note "ping A -> B ok" || note "ping A -> B FAILED"

coproc RC { exec "$RC_PAIR" "$IB_A" "$GIDX_A" "$IB_B" "$GIDX_B" 2>&1; }
RC_PID=$RC_PID
read -r -t 10 line <&"${RC[0]}"
[[ "$line" == ready ]] || { echo "rc_pair: $line"; exit 1; }

# ---------------------------------------------------------------- gid
if [[ " $PHASES " == *" gid "* ]]; then
	sect gid
	for n in 0 1 2 3; do rc connect $n; done
	note "-- baseline, mlx5e bound on A"
	rc send 0 AB
	rc send 0 BA
	unbind_eth_a
	note "-- A's mlx5e unbound: A sends (its source GID is deleted)"
	rc send 1 AB
	note "-- A's mlx5e unbound: B sends to A"
	rc send 2 BA
	rc state 3
	rebind_eth_a
	note "-- after rebind + IP, on pair 3 (connected before the unbind, idle during it)"
	rc send 3 AB
	rc send 3 BA
	phase_dmesg gid
fi

# ---------------------------------------------------------------- fence
if [[ " $PHASES " == *" fence "* ]]; then
	sect fence
	# Pairs connected before an unbind keep a stale GID index afterwards,
	# so 5 and 6 are connected only after the rebind.
	rc connect 4
	read -r FT FG < <("$FENCE" "$PF" raise 0) || { note "raise failed"; FT=""; }
	note "fence raised on vf 0: table=$FT group=$FG"
	note "-- fence up, mlx5e bound: B sends to A (expect dropped)"
	rc send 4 BA
	unbind_eth_a
	rebind_eth_a
	rc connect 5
	rc connect 6
	note "-- fence still up after mlx5e unbind + rebind, fresh pair: B sends to A (dropped = fence survived)"
	rc send 5 BA
	if [[ -n "$FT" ]] && "$FENCE" "$PF" lift 0 "$FT" "$FG"; then
		note "fence lifted with its handles"
	else
		note "fence lift FAILED"
	fi
	note "-- fence lifted: both directions"
	rc send 6 BA
	rc send 6 AB
	phase_dmesg fence
fi

# ---------------------------------------------------------------- detach
# Suspend A's mlx5e through the RX fence's F_ETH_SUSPEND instead of
# unbinding it: the netdev should stay registered, keeping its IP and
# GIDs, so QPs connected before it work again after the lift resumes it.
if [[ " $PHASES " == *" detach "* ]]; then
	sect detach
	ifx="$(cat /sys/class/net/$NET_A/ifindex)"
	for n in 10 11 12; do rc connect $n; done
	note "-- baseline"
	rc send 10 AB
	rc send 10 BA
	if ! read -r FT FG < <("$FENCE" "$PF" raise 0 eth 2>"$S/detach.err"); then
		note "fence raise with eth FAILED: $(cat "$S/detach.err")"
	else
		note "fence up (table $FT group $FG): $(cat "$S/detach.err")"
		note "netdev $NET_A: $( [[ -e /sys/class/net/$NET_A ]] && echo "present, ifindex $(cat /sys/class/net/$NET_A/ifindex) (was $ifx), $(ip -br -4 addr show dev "$NET_A" | awk '{print $3}')" || echo gone)"
		note "port=$(port_state "$IB_A") gids=$(gid_count "$IB_A") IP GID index $(gid_index "$IB_A" "$IP_A") (was $GIDX_A)"
		note "-- suspended: B sends to A"
		rc send 11 BA
		"$FENCE" "$PF" lift 0 "$FT" "$FG" eth && note "fence lifted, mlx5e resumed" \
			|| note "lift with eth FAILED"
		wait_active "$IB_A" || note "port not ACTIVE after resume: $(port_state "$IB_A")"
		note "port=$(port_state "$IB_A") gids=$(gid_count "$IB_A") IP GID index $(gid_index "$IB_A" "$IP_A") (was $GIDX_A)"
		note "-- after resume, pair 12 (connected before the suspend, idle during it)"
		rc send 12 AB
		rc send 12 BA
		note "-- after resume, pair 10 (used before the suspend)"
		rc send 10 AB
		rc send 10 BA
	fi
	phase_dmesg detach
fi

echo quit >&"${RC[1]}"
wait "$RC_PID" 2>/dev/null
RC_PID=""

# ---------------------------------------------------------------- recycle
if [[ " $PHASES " == *" recycle "* ]]; then
	sect recycle
	echo "$BDF_A" >/sys/bus/pci/drivers/mlx5_core/unbind
	if "$VFMIG_TOOL" "$PF" recycle_vf 0; then
		"$VFMIG_TOOL" "$PF" set_tracked 0 1 >/dev/null
		"$VFMIG_TOOL" "$PF" enable_migratable 0 >/dev/null
		echo "$BDF_A" >/sys/bus/pci/drivers/mlx5_core/bind
		IB_A="$(wait_ibdev "$BDF_A")"
		NET_A="$(wait_netdev "$BDF_A")"
		note "after RECYCLE_VF: netdev mac=$(cat /sys/class/net/$NET_A/address 2>/dev/null) (admin MAC was $MAC_A)"
	else
		note "recycle_vf failed"
	fi
	phase_dmesg recycle
fi

# ---------------------------------------------------------------- suspend
if [[ " $PHASES " == *" suspend "* ]]; then
	for dir in initiator both; do
		sect "suspend $dir"
		AUX_A="$(eth_aux "$BDF_A")"
		[[ -e "/sys/bus/auxiliary/devices/$AUX_A/driver" ]] || rebind_eth_a
		if ! "$VFMIG_TOOL" "$PF" suspend_vhca 0 "$dir"; then
			note "suspend_vhca $dir failed (is vf 0 migratable and bound?)"
			continue
		fi
		done_f="$S/.unbind_done.$$"
		rm -f "$done_f"
		( echo "$AUX_A" >"$ETH_DRV/unbind"; echo "rc=$?" >"$done_f" ) &
		for _ in $(seq 1 "$SUSPEND_WAIT"); do [[ -e "$done_f" ]] && break; sleep 1; done
		if [[ -e "$done_f" ]]; then
			note "unbind completed while suspended ($(cat "$done_f"))"
		else
			note "unbind did not complete within ${SUSPEND_WAIT}s while suspended; resuming"
		fi
		"$VFMIG_TOOL" "$PF" resume_vhca 0 both || note "resume_vhca failed"
		for _ in $(seq 1 "$SUSPEND_WAIT"); do [[ -e "$done_f" ]] && break; sleep 1; done
		[[ -e "$done_f" ]] && note "unbind finished: $(cat "$done_f")" || note "unbind still blocked after resume"
		note "netdev=$(ls /sys/bus/pci/devices/$BDF_A/net 2>/dev/null || echo none) port=$(port_state "$IB_A")"
		phase_dmesg "suspend $dir"
		rm -f "$done_f"
	done
fi

echo
echo "done; SR-IOV on $PF is reset on exit"
