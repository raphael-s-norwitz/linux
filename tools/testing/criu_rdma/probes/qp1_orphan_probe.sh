#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Does the source's QP1 left over on a restored VF write where it should
# not? On a restored VF mlx5_ib creates no QP1 of its own (the GSI setup
# is refused, so ib_mad leaves the port out), but firmware brings back
# the source's QP1 in RTS with its receive WQEs still pointing at the
# source's ib_mad buffers. Those were streaming DMA mappings outside the
# vfmig arena, so on the destination their IOVAs are either unmapped
# (an IOMMU fault) or now belong to something else (silent corruption).
#
# Flow, on PF $PF with two VFs in DMA domains:
#   control  VF 1 (the source, full drivers, $IP_A) gets RDMA CM connects
#            from VF 0 (the peer, netns $NS_B, $IP_B). Expect rping to
#            pass and nothing in dmesg.
#   restore  Raise the RX fence with F_ETH_SUSPEND on VF 1, SAVE it,
#            recycle both VFs, LOAD into VF 0 with VF 1's admin MAC, bind
#            it, lift the carried fence, give it $IP_A. Rebind VF 1 as the
#            peer in $NS_B with $IP_B.
#   hazard   Send $COUNT RDMA CM connects (rping -c) from the peer to the
#            restored VF. They go to QPN 1, the orphan. Report rping's
#            result, the restored port's receive counters, and dmesg
#            IOMMU faults, "bogus CQ" completion events (enabled through
#            dynamic debug for the run) and mlx5 errors.
#   teardown Unbind the restored VF and look at dmesg again.
#
#   sudo tools/testing/criu_rdma/probes/qp1_orphan_probe.sh
#   sudo COUNT=10 tools/testing/criu_rdma/probes/qp1_orphan_probe.sh
#
# Functional only; no timing. SR-IOV on $PF is reset at the end.

set -uo pipefail

PF="${PF:-0000:08:00.0}"
COUNT="${COUNT:-3}"
NS_B=qp1o_peer
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VFMIG_TOOL="${VFMIG_TOOL:-$HERE/../tools/mlx5_vfmig}"
FENCE="$HERE/vfmig_fence"
S="${WORKDIR:-$(mktemp -d /var/tmp/qp1_orphan_probe.XXXXXX)}"
BLOB="$S/vf.blob"
IP_A=192.168.251.10 IP_B=192.168.251.11
MAC_S=02:00:00:fb:00:10 MAC_P=02:00:00:fb:00:11
TAG=qp1_orphan_probe

PF_DEV="/sys/bus/pci/devices/$PF"
PF_NET="$(ls "$PF_DEV/net" 2>/dev/null | head -1)"
AUX=/sys/bus/auxiliary
DDC=/proc/dynamic_debug/control
[[ -e "$DDC" ]] || DDC=/sys/kernel/debug/dynamic_debug/control

IOMMU_RE='DMAR|AMD-Vi|IO_PAGE_FAULT|[Ii][Oo][Mm][Mm][Uu].*[Ff]ault|smmu.*fault'
BOGUS_RE='bogus CQ'
ERR_RE='Fatal error|syndrome|CQE error|local protection|WARNING:|BUG:|health compromised|unknown resource'

note() { printf '  %s\n' "$*"; }
sect() { printf '\n== %s ==\n' "$*"; kmsg "phase $*"; }
kmsg() { echo "$TAG: $*" >/dev/kmsg; }

old_autoprobe=""
cleanup() {
	ip netns del "$NS_B" 2>/dev/null
	echo 0 >"$PF_DEV/sriov_numvfs" 2>/dev/null
	echo 1 >"$PF_DEV/sriov_drivers_autoprobe" 2>/dev/null
	[[ -n "$old_autoprobe" ]] && echo "$old_autoprobe" >"$AUX/drivers_autoprobe"
	[[ -w "$DDC" ]] && echo 'module mlx5_core format "bogus CQ" -p' >"$DDC" 2>/dev/null
}
trap cleanup EXIT

[[ "$EUID" -eq 0 ]] || { echo "run as root"; exit 2; }
[[ -n "$PF_NET" ]] || { echo "PF $PF has no netdev (needed for VF admin MACs)"; exit 2; }
for f in "$FENCE" "$VFMIG_TOOL"; do [[ -x "$f" ]] || { echo "missing $f (make -C tools/testing/criu_rdma)"; exit 2; }; done
command -v rping >/dev/null || { echo "rping missing (librdmacm-utils)"; exit 2; }
old_autoprobe="$(cat "$AUX/drivers_autoprobe")"
echo 1 >"$AUX/drivers_autoprobe"
if [[ -w "$DDC" ]] && echo 'module mlx5_core format "bogus CQ" +p' >"$DDC" 2>/dev/null; then
	DYNDBG=on
else
	DYNDBG="off (no dynamic debug; bogus CQ events are not visible)"
fi

vf_bdf() { readlink "$PF_DEV/virtfn$1" | xargs basename; }
ibdev_of() { ls "/sys/bus/pci/devices/$1/infiniband" 2>/dev/null | head -1; }
wait_ibdev() {
	local ib
	for _ in $(seq 1 100); do ib="$(ibdev_of "$1")"; [[ -n "$ib" ]] && { echo "$ib"; return 0; }; sleep 0.1; done
	return 1
}
wait_netdev() {
	for _ in $(seq 1 100); do
		ls "/sys/bus/pci/devices/$1/net" 2>/dev/null | head -1 | grep . && return 0
		sleep 0.1
	done
	return 1
}
port_state() { cat "/sys/class/infiniband/$1/ports/1/state" 2>/dev/null || echo none; }
wait_active() {
	for _ in $(seq 1 100); do [[ "$(port_state "$1")" == *ACTIVE* ]] && return 0; sleep 0.1; done
	return 1
}
counter() {	# ibdev name: port counter or hw counter, "-" if absent
	local p="/sys/class/infiniband/$1/ports/1"
	cat "$p/counters/$2" 2>/dev/null || cat "$p/hw_counters/$2" 2>/dev/null || echo -
}
counters() {
	echo "port_rcv_packets=$(counter "$1" port_rcv_packets)" \
	     "port_xmit_packets=$(counter "$1" port_xmit_packets)" \
	     "out_of_buffer=$(counter "$1" out_of_buffer)"
}

bind_vf() {	# bdf: mlx5_core, auxiliary drivers follow through autoprobe
	echo mlx5_core >"/sys/bus/pci/devices/$1/driver_override"
	echo "$1" >/sys/bus/pci/drivers/mlx5_core/bind
}
unbind_vf() {
	[[ -e "/sys/bus/pci/devices/$1/driver" ]] && echo "$1" >"/sys/bus/pci/devices/$1/driver/unbind"
}
recycle() {	# vf_id: unbound -> recycled, tracked, migratable
	unbind_vf "$(vf_bdf "$1")"
	"$VFMIG_TOOL" "$PF" recycle_vf "$1" >/dev/null || { note "recycle_vf $1 FAILED"; return 1; }
	"$VFMIG_TOOL" "$PF" set_tracked "$1" 1 >/dev/null
	"$VFMIG_TOOL" "$PF" enable_migratable "$1" >/dev/null
}

# Bring the peer up on vf_id $1: MAC_P, netns NS_B, IP_B.
peer_up() {
	local bdf ib net
	bdf="$(vf_bdf "$1")"
	ip link set "$PF_NET" vf "$1" mac "$MAC_P"
	bind_vf "$bdf"
	ib="$(wait_ibdev "$bdf")" && net="$(wait_netdev "$bdf")" || { note "peer vf $1 did not come up"; return 1; }
	ip netns del "$NS_B" 2>/dev/null
	ip netns add "$NS_B"
	ip link set "$net" netns "$NS_B"
	ip -n "$NS_B" link set "$net" up && ip -n "$NS_B" addr add "$IP_B/24" dev "$net"
	wait_active "$ib" || note "peer port not ACTIVE: $(port_state "$ib")"
	IB_P="$ib"
	note "peer: vf $1 $bdf $ib $net (netns $NS_B) $IP_B port=$(port_state "$ib")"
}

# Print the dmesg lines logged since "$TAG: phase $1".
phase_dmesg() {
	dmesg | awk -v m="$TAG: phase $1" \
		'{l[NR] = $0} index($0, m) {s = NR} END {for (i = s + 1; s && i <= NR; i++) print l[i]}'
}
report_dmesg() {	# phase
	local d
	d="$(phase_dmesg "$1")"
	note "IOMMU faults:        $(grep -cE "$IOMMU_RE" <<<"$d")"
	note "bogus CQ events:     $(grep -cE "$BOGUS_RE" <<<"$d") (dynamic debug $DYNDBG)"
	note "mlx5 errors/WARNs:   $(grep -cE "$ERR_RE" <<<"$d")"
	grep -E "$IOMMU_RE|$BOGUS_RE|$ERR_RE|vfmig" <<<"$d" | grep -v 'arena reserved' | tail -25 \
		| sed 's/^/    dmesg: /'
}

# One rping connect from the peer netns to IP_A. Against the restored VF
# nothing answers, so rping runs until the CM gives up on its REQ retries.
cm_connect() {	# label
	local rc out
	out="$(timeout 120 ip netns exec "$NS_B" rping -c -a "$IP_A" -C 1 -v 2>&1)"
	rc=$?
	note "$1: rc=$rc $(tr '\n' ' ' <<<"$out" | cut -c1-160)"
	return $rc
}

# ---------------------------------------------------------------- setup
sect setup
echo 0 >"$PF_DEV/sriov_numvfs"
echo 0 >"$PF_DEV/sriov_drivers_autoprobe"
echo 2 >"$PF_DEV/sriov_numvfs"
BDF_0="$(vf_bdf 0)" BDF_1="$(vf_bdf 1)"
for b in "$BDF_0" "$BDF_1"; do
	t="/sys/bus/pci/devices/$b/iommu_group/type"
	case "$(cat "$t")" in DMA|DMA-FQ) ;; *) echo DMA >"$t" ;; esac
done
for v in 0 1; do
	"$VFMIG_TOOL" "$PF" set_tracked "$v" 1 >/dev/null
	"$VFMIG_TOOL" "$PF" enable_migratable "$v" >/dev/null
done
ip link set "$PF_NET" vf 1 mac "$MAC_S"
bind_vf "$BDF_1"
IB_S="$(wait_ibdev "$BDF_1")" NET_S="$(wait_netdev "$BDF_1")" || { echo "source vf 1 did not come up"; exit 1; }
ip link set "$NET_S" up && ip addr add "$IP_A/24" dev "$NET_S"
wait_active "$IB_S" || note "source port not ACTIVE: $(port_state "$IB_S")"
note "source: vf 1 $BDF_1 $IB_S $NET_S $IP_A port=$(port_state "$IB_S")"
peer_up 0 || exit 1

# ---------------------------------------------------------------- control
sect control
pass=0
for i in $(seq 1 "$COUNT"); do
	timeout 30 rping -s -a "$IP_A" -C 1 -v >>"$S/rping_server.log" 2>&1 &
	sleep 1
	cm_connect "control connect $i" && pass=$((pass + 1))
	wait $!
done
note "control: $pass of $COUNT rping runs passed (expect all)"
report_dmesg control

# ---------------------------------------------------------------- restore
sect restore
read -r FT FG < <("$FENCE" "$PF" raise 1 eth 2>"$S/fence.err") || { echo "fence raise FAILED: $(cat "$S/fence.err")"; exit 1; }
note "fence up on vf 1 with mlx5e suspended: table $FT group $FG"
"$VFMIG_TOOL" "$PF" save_vhca_state 1 "$BLOB" >/dev/null || { echo "SAVE vf 1 FAILED"; exit 1; }
note "saved vf 1: $(dmesg | grep 'vfmig: saved vf 1 ' | tail -1 | sed 's/.*vfmig: //')"
recycle 1 || exit 1
ip link set "$PF_NET" vf 1 mac 00:00:00:00:00:00
ip netns del "$NS_B" 2>/dev/null
recycle 0 || exit 1
ip link set "$PF_NET" vf 0 mac "$MAC_S"
"$VFMIG_TOOL" "$PF" load_vhca_state 0 "$BLOB" >/dev/null || { echo "LOAD vf 0 FAILED"; exit 1; }
bind_vf "$BDF_0"
IB_R="$(wait_ibdev "$BDF_0")" NET_R="$(wait_netdev "$BDF_0")" || { echo "restored vf 0 did not come up"; exit 1; }
"$FENCE" "$PF" lift 0 "$FT" "$FG" && note "carried fence lifted on vf 0" || note "fence lift on vf 0 FAILED"
ip link set "$NET_R" up && ip addr add "$IP_A/24" dev "$NET_R"
wait_active "$IB_R" || note "restored port not ACTIVE: $(port_state "$IB_R")"
note "restored: vf 0 $BDF_0 $IB_R $NET_R $IP_A port=$(port_state "$IB_R")" \
     "driver QPs=$(ls /sys/kernel/debug/mlx5/$BDF_0/QPs 2>/dev/null | wc -l)" \
     "CQs=$(ls /sys/kernel/debug/mlx5/$BDF_0/CQs 2>/dev/null | wc -l)"
peer_up 1 || exit 1
ip netns exec "$NS_B" ping -c 2 -W 2 "$IP_A" >/dev/null && note "ping peer -> restored ok" || note "ping peer -> restored FAILED"
report_dmesg restore

# ---------------------------------------------------------------- hazard
sect hazard
note "restored before: $(counters "$IB_R")"
for i in $(seq 1 "$COUNT"); do cm_connect "hazard connect $i"; done
sleep 2
note "restored after:  $(counters "$IB_R")"
ip netns exec "$NS_B" ping -c 2 -W 2 "$IP_A" >/dev/null && note "ping peer -> restored still ok" || note "ping peer -> restored FAILED after the connects"
report_dmesg hazard

# ---------------------------------------------------------------- teardown
sect teardown
unbind_vf "$BDF_0"
sleep 2
report_dmesg teardown

echo
echo "done; work dir $S; SR-IOV on $PF is reset on exit"
