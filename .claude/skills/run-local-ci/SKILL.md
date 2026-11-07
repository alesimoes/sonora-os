---
name: run-local-ci
description: Run a context's local CI gate through the machine-wide lock and read its summary. Use when the orchestrator or a fixer must check a branch the way the gate does.
---

# Run the local CI gate

1. See what would run, building nothing:
   `tools/local-ci.sh --dry-run` (contexts, steps, ctest filter). The path to
   context table is in the script header.
2. Run it through the lock, from the checkout you were given (never the live
   daemon's):
   `tools/build-slot.sh tools/local-ci.sh` (a PR: the contexts the branch
   touches since `origin/main`). `--full` is main's run, `--os` the image,
   `--base REF` another base.
3. Read the summary on stdout: one line per step. Open only the failing
   step's log, `.local-ci/<step>.log` (or `$SONORA_LOCAL_CI_DIR`), and tail
   it; never paste a log into the context.
4. Exit 0 only when every step passed. Anything else is red: route it with
   the failure table in `.claude/PROCESS.md`.
5. Waiting in the queue is normal (`tools/build-slot.sh --list`); do not start
   a second run, bypass the lock or lower the checks.
