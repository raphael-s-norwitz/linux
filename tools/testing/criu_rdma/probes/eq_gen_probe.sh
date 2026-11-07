#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Record the EQs/CQs/QPs the driver sees on a normal VF and on restored
# generations 1..GENS, to check whether the source's EQs stay allocated.
# Run: sudo GENS=4 bash tools/testing/criu_rdma/probes/eq_gen_probe.sh
PF=${PF:-0000:08:00.0}
GENS=${GENS:-4}
S=/sys/bus/pci/devices/$PF
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
K="$(cd "$HERE/../../../.." && pwd)"
T=$K/tools/testing/criu_rdma/tools/mlx5_vfmig
OUT=${OUT:-$PWD/eq_gen_probe}
BLOB=$OUT.blob
DBG=/sys/kernel/debug/mlx5
exec > >(tee "$OUT.log") 2>&1

make -C $K/tools/testing/criu_rdma >/dev/null || exit 1
mountpoint -q /sys/kernel/debug || mount -t debugfs none /sys/kernel/debug

# Bare `wait` would also wait on the tee above and never return.
bind_vf() {
	local bdf=$1 bg i
	echo mlx5_core > /sys/bus/pci/devices/$bdf/driver_override
	( echo "$bdf" > /sys/bus/pci/drivers/mlx5_core/bind ) 2>/dev/null &
	bg=$!
	for i in $(seq 1 180); do
		[ -e /sys/bus/pci/devices/$bdf/infiniband ] && { wait $bg; sleep 2; return 0; }
		kill -0 $bg 2>/dev/null || break
		sleep 0.5
	done
	[ -e /sys/bus/pci/devices/$bdf/infiniband ]
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

bdf_of() { basename "$(readlink $S/virtfn$1)"; }

snap() {	# label vf_id
	local bdf d e f line
	bdf=$(bdf_of $2)
	d=$DBG/$bdf
	echo "== $1: vf $2 ($bdf) msi_irqs=$(ls /sys/bus/pci/devices/$bdf/msi_irqs 2>/dev/null | wc -l)" \
		"CQs=$(ls $d/CQs 2>/dev/null | wc -l) QPs=$(ls $d/QPs 2>/dev/null | wc -l)"
	for e in $(ls $d/EQs 2>/dev/null | sort -V); do
		line="   eq $e:"
		for f in $(ls $d/EQs/$e); do
			line="$line $f=$(cat $d/EQs/$e/$f 2>/dev/null | tr -d '\n')"
		done
		echo "$line"
	done
	echo "   CQs: $(ls $d/CQs 2>/dev/null | sort -V | tr '\n' ' ')"
	echo "   QPs: $(ls $d/QPs 2>/dev/null | sort -V | tr '\n' ' ')"
}

echo "criu_rdma eq_gen_probe start" > /dev/kmsg
echo 0 > $S/sriov_numvfs
echo 0 > $S/sriov_drivers_autoprobe
echo 2 > $S/sriov_numvfs
for v in 0 1; do
	tf=/sys/bus/pci/devices/$(bdf_of $v)/iommu_group/type
	[ "$(cat $tf)" = identity ] && echo DMA > $tf
	$T $PF set_tracked $v 1 >/dev/null
	$T $PF enable_migratable $v >/dev/null
done

cur=1
bind_vf $(bdf_of $cur) || { echo "   VF$cur bind FAILED"; exit 1; }
snap "gen 0 (normal)" $cur

for g in $(seq 1 $GENS); do
	dst=$((1 - cur))
	[ $g -gt 1 ] && fresh $dst
	$T $PF save_vhca_state $cur $BLOB >/dev/null || { echo "   SAVE vf $cur FAILED"; break; }
	sz=$(dmesg | grep "vfmig: saved vf $cur " | tail -1 | sed 's/.*state: //')
	echo "-- gen $g: saved vf $cur FW state $sz"
	$T $PF load_vhca_state $dst $BLOB >/dev/null || { echo "   LOAD vf $dst FAILED"; break; }
	bind_vf $(bdf_of $dst) || { echo "   VF$dst bind FAILED (gen $g)"; break; }
	snap "gen $g (restored)" $dst
	cur=$dst
done

echo "== dmesg (drift / EQ / errors since start)"
dmesg | sed -n '/criu_rdma eq_gen_probe start/,$p' |
	grep -E 'drift detection armed|DRIFT|CREATE_EQ|failed to create|restored;|size mismatch|vfmig: (adopted|creating a new|source .* EQ|dropped|could not|comp vector)' | tail -80
echo "== leaving VFs as they are; reset with: echo 0 > $S/sriov_numvfs"
chmod 644 $OUT.*
