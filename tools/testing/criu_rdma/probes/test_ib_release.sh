#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Check MLX5_VFMIG_RX_FENCE_F_IB_RELEASE on VF 0 of $PF with
# umr_release_probe: the first MR creates mlx5_ib's UMR CQ, the fence raise
# with the flag removes it, the next MR creates it again, and the lift and
# a LIFT with the flag behave. Creates one VF if the PF has none and
# removes it again afterwards. Run as root:
#
#   make -C tools/testing/criu_rdma probes
#   sudo PF=0000:08:00.0 tools/testing/criu_rdma/probes/test_ib_release.sh

set -uo pipefail

PF="${PF:-0000:08:00.0}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROBE="$HERE/umr_release_probe"
SYS=/sys/bus/pci/devices/$PF

[[ -x "$PROBE" ]] || { echo "missing $PROBE: make -C tools/testing/criu_rdma probes"; exit 2; }

made_vfs=0
if [[ "$(cat "$SYS/sriov_numvfs")" == 0 ]]; then
	echo 1 > "$SYS/sriov_numvfs" || exit 1
	made_vfs=1
fi
vf_bdf="$(basename "$(readlink "$SYS/virtfn0")")"

ibdev=""
for _ in $(seq 1 30); do
	ibdev="$(ls "/sys/bus/pci/devices/$vf_bdf/infiniband" 2>/dev/null | head -1)"
	[[ -n "$ibdev" ]] && break
	sleep 1
done
[[ -n "$ibdev" ]] || { echo "VF $vf_bdf has no ib device"; exit 1; }
echo "PF $PF VF 0 $vf_bdf ibdev $ibdev"

mark="test_ib_release $$ $(date +%s)"
echo "$mark" > /dev/kmsg
"$PROBE" "$PF" 0 "$ibdev"
rc=$?

echo "== kernel log"
dmesg | awk -v m="$mark" 'index($0, m) {p = 1} p'

[[ "$made_vfs" == 1 ]] && echo 0 > "$SYS/sriov_numvfs"
exit $rc
