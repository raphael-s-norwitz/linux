# Rebase git hooks

Versioned git hooks (and their installer) for the CRIU RDMA rebase effort.
These are the single source of truth; the copies living inside each repo's
`.git/hooks/` are installed from here by `install-hooks.sh`.

## Files

| File               | Role                                                        |
|--------------------|-------------------------------------------------------------|
| `commit-msg`       | Validates + normalizes commit messages.                     |
| `pre-commit`       | Runs `checkpatch --strict` on the staged C/header diff.     |
| `install-hooks.sh` | Copies the two hooks into the rebase repositories.          |

## What the hooks enforce

### `commit-msg`
- Subject line `<= 52` characters.
- Blank line required after the subject.
- Body wrapped at `<= 72` characters. Exempt: trailer lines
  (`Key: value`) and lines whose longest single token already exceeds 72
  (e.g. URLs), which cannot be wrapped.
- Auto-appends these trailers at the very end, in order, de-duplicated:
  - `Assisted-by: Cursor:claude-opus-4.8-high`
  - `Signed-off-by: <name> <email>` — derived from the repo's
    `git config user.name` / `user.email` (omitted if either is unset).
- Skips `Merge`, `Revert "..."`, `fixup!`, `squash!`, `amend!` commits.

### `pre-commit`
- Collects the staged `*.c`, `*.h`, `*.S` files (docs/plans and other
  non-code changes are ignored, so those commits are never blocked).
- Feeds the staged diff to `checkpatch.pl --strict --no-signoff
  --ignore=FILE_PATH_CHANGES` and blocks the commit unless the result is
  **fully clean: 0 errors, 0 warnings, 0 checks**.
- checkpatch is resolved per repo:
  - kernel repo -> its own `scripts/checkpatch.pl` (run in-tree, `--root`);
  - criu repo -> the kernel's `/opt/builds/linux/scripts/checkpatch.pl`
    (run with `--no-tree`), since criu ships no checkpatch of its own.
- `FILE_PATH_CHANGES` (the "does MAINTAINERS need updating?" nag) is
  suppressed because it fires on every new file and is a final-submission
  concern, not a per-commit style issue.

Bypass either hook for a single commit with `git commit --no-verify`.

## How the installer works

`install-hooks.sh` copies `commit-msg` and `pre-commit` from this directory
into each target repository's hooks directory.

Key points:

- **Targets are repositories, not worktrees.** Git stores hooks in the
  *common* git dir, so a repo's worktrees all share one hooks directory.
  The default targets are therefore just the two repos:
  - `/opt/builds/linux` — kernel; the shared hooks also apply to the
    `linux-poc-ref` worktree.
  - `/opt/builds/criu` — criu; the shared hooks also apply to the
    `criu-poc-ref` worktree.
- **Destination resolution.** For each repo it runs
  `git -C <repo> rev-parse --path-format=absolute --git-path hooks`
  to get the exact directory git will use (this also honours
  `core.hooksPath` if it is ever set), then installs the hooks there with
  mode `0755`.
- **Safe + idempotent.** If a destination hook already exists and differs,
  it is copied to `<hook>.bak.<timestamp>` before being overwritten.
  Re-running with the same sources makes no further changes.

### Usage

Run from anywhere (the script locates its own sources):

```bash
# Install into the two default repos (/opt/builds/linux, /opt/builds/criu):
tools/testing/criu_rdma/hooks/install-hooks.sh

# Install into explicit repositories instead:
tools/testing/criu_rdma/hooks/install-hooks.sh /path/to/linux /path/to/criu

# Preview actions without changing anything:
CRIU_HOOKS_DRYRUN=1 tools/testing/criu_rdma/hooks/install-hooks.sh
```

The setup orchestrator runs this script after the worktrees are created so
that both repos get the hooks automatically.
