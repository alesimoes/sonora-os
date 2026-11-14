---
name: run-micro-build
description: Build what a branch changes and run its tests the way the PR gate does (tools/micro-build.sh through the machine-wide lock). Use when a fixer or an implementer must check a branch before pushing.
---

# Run the micro build

1. See what the gate would do, building nothing:
   `tools/micro-build.sh --dry-run` (changed paths since `origin/main`, the
   image, whether the RTSan stage applies).
2. Run it from the checkout you were given (never the live daemon's); it
   takes the lock itself:
   `tools/micro-build.sh` (`--base REF` for another base, `--no-rtsan` to
   skip the RT stage while iterating, `--rtsan` to force it).
3. Read the plan: `.micro-build/plan.json` says which targets were built and
   which tests ran, and why per path. A path with "no target" built nothing.
4. The last line is `micro-build: all passed` or `micro-build: FAIL <stage>`.
   Red: fix the root cause; never widen a filter or skip a test to pass.
5. Longer than your command time limit (a UI build with its wait for the
   lock often is): start it detached and collect it, never in your own
   background:
   `tools/build-slot.sh --detach tools/micro-build.sh --no-rtsan` prints a job
   id; `tools/build-wait.sh` blocks up to 9 minutes and prints the result and
   the log's tail. Exit 75 = still queued or running: call it again (nothing
   is lost). Any other exit is the build's own. `tools/build-job.sh cancel`
   stops it (its container too). One job per checkout at a time.
   `micro-build: phase NAME N s` lines say where the time went.
6. Waiting in the queue is normal (`tools/build-slot.sh --list`); do not start
   a second run, bypass the lock or lower the checks. The full build is the
   nightly on main (`.github/workflows/nightly.yml`), never something to run
   by hand for a PR.
