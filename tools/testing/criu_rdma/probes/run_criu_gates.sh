#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Run the CRIU tree's hardware vfmig gates one after another and
# summarize. Run as root:
#
#   sudo CRIU_TREE=/path/to/criu tools/testing/criu_rdma/probes/run_criu_gates.sh
#   sudo CRIU_TREE=... GATES="qp qp_traffic" tools/testing/criu_rdma/probes/run_criu_gates.sh
#
# Env: CRIU_TREE (required), PF (default 0000:08:00.0), GATES
# (space-separated gate names, default below), OUT (results dir), KEEP=1 to
# keep every gate's work dir. A gate <g> is test/rdma/run_vfmig_<g>_cr.sh,
# or test/rdma/run_vfmig_<g>.sh if that does not exist.
#
# Each gate's output goes to $OUT/<gate>.log and the kernel log it
# produced to $OUT/<gate>.dmesg. The summary counts kernel WARN/BUG/oops
# lines per gate; three kmod_dup_request_exists_wait warnings per CRIU
# dump are known.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CRIU_TREE="${CRIU_TREE:?set CRIU_TREE to the CRIU tree}"
PF="${PF:-0000:08:00.0}"
VFMIG_TOOL="${VFMIG_TOOL:-$HERE/../tools/mlx5_vfmig}"
GATES="${GATES:-cr pd cq mr qp xvf qp_traffic restore_guard}"
OUT="${OUT:-/var/tmp/vfmig-gates-$(date +%Y%m%d-%H%M%S)}"

mkdir -p "$OUT"
cd "$CRIU_TREE" || exit 1

declare -A RESULT
for g in $GATES; do
	script="test/rdma/run_vfmig_${g}_cr.sh"
	[[ -f "$script" ]] || script="test/rdma/run_vfmig_${g}.sh"
	if [[ ! -f "$script" ]]; then
		RESULT[$g]="MISSING"
		continue
	fi
	mark="run_criu_gates: $g $$ $(date +%s)"
	echo "$mark" > /dev/kmsg
	echo "== $g ($script)"
	env PF="$PF" VFMIG_TOOL="$VFMIG_TOOL" VFMIG_CR_HW=1 ${KEEP:+KEEP=1} \
		bash "$script" > "$OUT/$g.log" 2>&1
	rc=$?
	dmesg | awk -v m="$mark" 'index($0, m) {p = 1} p' > "$OUT/$g.dmesg"
	bad=$(grep -cE "WARNING:|BUG:|Oops|general protection|KASAN|refcount_t" "$OUT/$g.dmesg")
	RESULT[$g]="rc=$rc kernel_warn=$bad"
	tail -2 "$OUT/$g.log"
done

echo
echo "== summary ($OUT)"
for g in $GATES; do
	printf '%-14s %s\n' "$g" "${RESULT[$g]}"
done
