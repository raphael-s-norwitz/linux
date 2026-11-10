#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Find whose objects grow the saved image across restored generations.
# For each mode, run a chain normal -> gen 1 -> ... -> gen GENS with only
# some of the VF's auxiliary drivers bound, and print the image size and
# its growth per generation. Auxiliary autoprobe is turned off for the
# run so nothing binds behind our back, and restored on exit.
#   none   mlx5_core only
#   eth    + mlx5_core.eth
#   rdma   + mlx5_ib.rdma
#   full     + mlx5_core.eth, mlx5_ib.rdma, mlx5_fwctl (the usual setup)
#   dropeth  as full, but unbind the source's mlx5_core.eth before each SAVE
#   suspendeth  as full, but raise the RX fence with F_ETH_SUSPEND before
#            each SAVE (mlx5e suspended, netdev stays registered), and lift
#            the carried fence on the destination after it binds
#   linkdown as full, but "ip link set down" the source's netdev before each
#            SAVE (userspace only; mlx5e closes its channels but keeps
#            TIRs, RQT, flow tables, TIS, PD and TD)
# With mlx5_ib bound and probes/q_counter_room built ("make probes"), each
# generation also prints qc_room, the queue counters the VF has left.
# Run: sudo tools/testing/criu_rdma/probes/growth_split_probe.sh
#      sudo MODES="full dropeth" GENS=4 tools/testing/criu_rdma/probes/growth_split_probe.sh
PF=${PF:-0000:08:00.0}
MODES=${MODES:-"none eth rdma full dropeth"}
GENS=${GENS:-3}
S=/sys/bus/pci/devices/$PF
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
K="$(cd "$HERE/../../../.." && pwd)"
T=$K/tools/testing/criu_rdma/tools/mlx5_vfmig
FENCE=$HERE/vfmig_fence
OUT=${OUT:-$PWD/growth_split_probe}
BLOB=$OUT.blob
DBG=/sys/kernel/debug/mlx5
AUX=/sys/bus/auxiliary
exec > >(tee "$OUT.log") 2>&1

make -C $K/tools/testing/criu_rdma >/dev/null || exit 1
mountpoint -q /sys/kernel/debug || mount -t debugfs none /sys/kernel/debug

old_autoprobe=$(cat $AUX/drivers_autoprobe)
trap 'echo $old_autoprobe > $AUX/drivers_autoprobe' EXIT
echo 0 > $AUX/drivers_autoprobe

bdf_of() { basename "$(readlink $S/virtfn$1)"; }

aux_drivers() {	# mode -> "kind:driver ..."
	case $1 in
	none) ;;
	eth)  echo "eth:mlx5_core.eth" ;;
	rdma) echo "rdma:mlx5_ib.rdma" ;;
	full|dropeth|suspendeth|linkdown) echo "eth:mlx5_core.eth rdma:mlx5_ib.rdma fwctl:mlx5_fwctl.mlx5_fwctl" ;;
	esac
}

unbind_eth() {	# vf_id
	local dev
	dev=$(basename "$(ls -d /sys/bus/pci/devices/$(bdf_of $1)/mlx5_core.eth.* 2>/dev/null | head -1)")
	[ -e $AUX/devices/$dev/driver ] && echo "$dev" > $AUX/devices/$dev/driver/unbind
}

port_state() {	# vf_id: bring the netdev up, then report RoCE port 1
	local bdf=$(bdf_of $1) ib net i st
	ib=$(ls /sys/bus/pci/devices/$bdf/infiniband 2>/dev/null | head -1)
	net=$(ls /sys/bus/pci/devices/$bdf/net 2>/dev/null | head -1)
	[ -n "$ib" ] || { echo "port=no-ib"; return; }
	[ -n "$net" ] && ip link set "$net" up 2>/dev/null
	for i in $(seq 1 20); do
		st=$(cat /sys/class/infiniband/$ib/ports/1/state 2>/dev/null)
		[[ "$st" == *ACTIVE* ]] && break
		sleep 0.5
	done
	echo "port=${st#*: } gids=$(cat /sys/class/infiniband/$ib/ports/1/gids/* 2>/dev/null | grep -vc '^0000:0000:0000:0000:0000:0000:0000:0000$')"
}

# Bare `wait` would also wait on the tee above and never return.
bind_vf() {	# bdf mode
	local bdf=$1 bg i kd kind drv dev
	echo mlx5_core > /sys/bus/pci/devices/$bdf/driver_override
	( echo "$bdf" > /sys/bus/pci/drivers/mlx5_core/bind ) 2>/dev/null &
	bg=$!
	for i in $(seq 1 180); do
		kill -0 $bg 2>/dev/null || break
		sleep 0.5
	done
	kill -0 $bg 2>/dev/null && { echo "   mlx5_core bind of $bdf hung"; return 1; }
	wait $bg
	[ -e /sys/bus/pci/devices/$bdf/driver ] || return 1
	for kd in $(aux_drivers $2); do
		kind=${kd%%:*} drv=${kd#*:}
		dev=$(basename "$(ls -d /sys/bus/pci/devices/$bdf/mlx5_core.$kind.* 2>/dev/null | head -1)")
		[ -n "$dev" ] || { echo "   no mlx5_core.$kind device on $bdf"; return 1; }
		echo "$dev" > $AUX/drivers/$drv/bind || { echo "   bind $dev to $drv FAILED"; return 1; }
	done
	sleep 2
}

unbind_vf() {
	local bdf=$1
	[ -e /sys/bus/pci/devices/$bdf/driver ] && echo "$bdf" > /sys/bus/pci/devices/$bdf/driver/unbind
}

fresh() {	# vf_id: unbound -> recycled + bootstrapped
	unbind_vf "$(bdf_of $1)"
	$T $PF recycle_vf $1 >/dev/null || echo "   RECYCLE vf $1 FAILED"
	$T $PF set_tracked $1 1 >/dev/null
	$T $PF enable_migratable $1 >/dev/null
}

objs() {	# vf_id
	local d=$DBG/$(bdf_of $1)
	echo "CQs=$(ls $d/CQs 2>/dev/null | wc -l) QPs=$(ls $d/QPs 2>/dev/null | wc -l)" \
	     "ib=$([ -e /sys/bus/pci/devices/$(bdf_of $1)/infiniband ] && echo y || echo n)" \
	     "net=$(ls /sys/bus/pci/devices/$(bdf_of $1)/net 2>/dev/null | tr '\n' ' ')"
}

q_room() {	# vf_id: queue counters the VF can still allocate
	local ib=$(ls /sys/bus/pci/devices/$(bdf_of $1)/infiniband 2>/dev/null | head -1)
	[ -n "$ib" ] && [ -x $HERE/q_counter_room ] || return
	echo "qc_room=$($HERE/q_counter_room $ib 2>/dev/null | sed -n 's/.*room for \([0-9]*\) .*/\1/p')"
}

save() {	# vf_id mode -> prints size
	[ "$2" = dropeth ] && unbind_eth $1
	if [ "$2" = suspendeth ]; then
		$FENCE $PF raise $1 eth > $OUT.fence 2>/dev/null || return 1
	fi
	[ "$2" = linkdown ] && ip link set dev "$(ls /sys/bus/pci/devices/$(bdf_of $1)/net | head -1)" down
	$T $PF save_vhca_state $1 $BLOB >/dev/null || return 1
	dmesg | grep "vfmig: saved vf $1 " | tail -1 | sed 's/.*state: //; s/ .*//'
}

declare -A summary
echo "criu_rdma growth_split_probe start" > /dev/kmsg
for mode in $MODES; do
	echo 0 > $S/sriov_numvfs
	echo "criu_rdma growth_split_probe mode $mode" > /dev/kmsg
	echo "== mode $mode: $(aux_drivers $mode | sed 's/[a-z]*://g')"
	echo 0 > $S/sriov_drivers_autoprobe
	echo 2 > $S/sriov_numvfs
	for v in 0 1; do
		tf=/sys/bus/pci/devices/$(bdf_of $v)/iommu_group/type
		[ "$(cat $tf)" = identity ] && echo DMA > $tf
		$T $PF set_tracked $v 1 >/dev/null
		$T $PF enable_migratable $v >/dev/null
	done

	cur=1 prev= deltas=
	bind_vf $(bdf_of $cur) $mode || { echo "   VF$cur bind FAILED"; continue; }
	for g in $(seq 0 $GENS); do
		o="$(objs $cur)"
		case $mode in full|dropeth|suspendeth|linkdown) o="$o $(port_state $cur)" ;; esac
		o="$o $(q_room $cur)"
		sz=$(save $cur $mode) || { echo "   SAVE vf $cur FAILED (gen $g)"; break; }
		d=
		[ -n "$prev" ] && { d=$((sz - prev)); deltas="$deltas $d"; d=" (+$d)"; }
		printf '   gen %d vf %d: %s bytes%s  %s\n' $g $cur "$sz" "$d" "$o"
		prev=$sz
		[ $g -eq $GENS ] && break
		dst=$((1 - cur))
		[ $g -gt 0 ] && fresh $dst
		$T $PF load_vhca_state $dst $BLOB >/dev/null || { echo "   LOAD vf $dst FAILED"; break; }
		bind_vf $(bdf_of $dst) $mode || { echo "   VF$dst bind FAILED (gen $((g + 1)))"; break; }
		# The restored VF carries the source's fence; take it down as a
		# restore would.
		if [ $mode = suspendeth ]; then
			$FENCE $PF lift $dst $(cat $OUT.fence) || { echo "   fence lift on vf $dst FAILED"; break; }
		fi
		cur=$dst
	done
	summary[$mode]="${deltas:- n/a}"
done

echo 0 > $S/sriov_numvfs
echo "criu_rdma growth_split_probe end" > /dev/kmsg
echo "== growth per generation (bytes), DESTROY_EQ failures, fatal health events"
this_run() {	# dmesg from this run's start marker on
	dmesg | awk '{l[NR] = $0} /criu_rdma growth_split_probe start/ {s = NR}
		END {for (i = s; s && i <= NR; i++) print l[i]}'
}
for mode in $MODES; do
	m=$(this_run | sed -n "/criu_rdma growth_split_probe mode $mode\$/,/criu_rdma growth_split_probe \(mode\|end\)/p")
	printf '   %-8s%-28s destroy_eq_fail=%s fatal=%s\n' $mode "${summary[$mode]}" \
		"$(grep -c 'DESTROY_EQ(0x302).*failed' <<< "$m")" \
		"$(grep -c 'Fatal error' <<< "$m")"
done
echo "== dmesg (restores / errors since start)"
this_run | grep -E 'restored;|DRIFT|Fatal error|failed|creating a new|does not match' | tail -40
echo "== SR-IOV was reset at the end, so the last mode's teardown is counted too"
chmod 644 $OUT.*
