#!/usr/bin/env bash
#
# install-hooks.sh — install the versioned git hooks into the kernel and
# criu rebase repositories.
#
# The canonical hook sources live next to this script:
#   commit-msg   subject/body/trailer enforcement for commit messages
#   pre-commit   checkpatch --strict gate on the staged C/header diff
#
# Default targets (one per repository -- NOT per worktree):
#   /opt/builds/linux   kernel repo   (shared hooks also cover linux-poc-ref)
#   /opt/builds/criu    criu repo     (shared hooks also cover criu-poc-ref)
#
# Git stores hooks in the *common* git dir, so they are shared by every
# worktree of a repo. Installing once per repo therefore covers all of that
# repo's worktrees. The exact destination is resolved with:
#   git -C <repo> rev-parse --path-format=absolute --git-path hooks
# which also honours core.hooksPath if it is ever set.
#
# An existing hook that differs from the versioned copy is backed up to
# <hook>.bak.<timestamp> before being overwritten. Re-running is idempotent.
#
# Usage:
#   ./install-hooks.sh                    # install into the two default repos
#   ./install-hooks.sh /path/a /path/b    # install into the given repos
#   CRIU_HOOKS_DRYRUN=1 ./install-hooks.sh  # print actions, change nothing
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOOKS=(commit-msg pre-commit)
DEFAULT_REPOS=(/opt/builds/linux /opt/builds/criu)
DRYRUN="${CRIU_HOOKS_DRYRUN:-0}"

if [ "$#" -gt 0 ]; then
	REPOS=("$@")
else
	REPOS=("${DEFAULT_REPOS[@]}")
fi

say() { printf '%s\n' "$*"; }

for h in "${HOOKS[@]}"; do
	[ -f "$SCRIPT_DIR/$h" ] || {
		echo "error: missing hook source: $SCRIPT_DIR/$h" >&2
		exit 1
	}
done

rc=0
for repo in "${REPOS[@]}"; do
	say "== $repo =="
	if ! git -C "$repo" rev-parse --git-dir >/dev/null 2>&1; then
		echo "  skip: not a git repository" >&2
		rc=1
		continue
	fi

	hooksdir="$(git -C "$repo" rev-parse --path-format=absolute --git-path hooks)"
	[ "$DRYRUN" = 1 ] || mkdir -p "$hooksdir"

	for h in "${HOOKS[@]}"; do
		src="$SCRIPT_DIR/$h"
		dst="$hooksdir/$h"

		if [ -e "$dst" ] && ! cmp -s "$src" "$dst"; then
			bak="$dst.bak.$(date +%Y%m%d%H%M%S)"
			if [ "$DRYRUN" = 1 ]; then
				say "  [dry-run] backup differing $h -> $bak"
			else
				cp -p "$dst" "$bak"
				say "  backed up existing $h -> $(basename "$bak")"
			fi
		fi

		if [ "$DRYRUN" = 1 ]; then
			say "  [dry-run] install $h -> $dst"
		else
			install -m 0755 "$src" "$dst"
			say "  installed $h"
		fi
	done
	say "  hooks dir: $hooksdir"
done

say ""
if [ "$DRYRUN" = 1 ]; then
	say "dry-run complete (no changes made)."
else
	say "done."
fi
exit "$rc"
