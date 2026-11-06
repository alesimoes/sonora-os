# Build lock and the machine

- One heavy build or test at a time on the machine, through
  `tools/build-slot.sh COMMAND...` (queue, then run). Why: 2026-11-05 agents
  building outside the lock overloaded the machine and every build slowed.
- Write the whole slice first; at most three build cycles per task; narrowest
  target; keep the build directory (incremental); no second container for the
  same worktree; never wait on a build.
- The full local CI (all tests, sanitizers) is the orchestrator's, not an
  agent's. An agent runs its module's tests (`ctest -L <label>`) and no more.
- Never touch the live daemon's checkout, its `.local-ci` output, its
  database or its lock directory. Never `git switch`, `reset` or `checkout`
  in a checkout you were not given. Why: an agent switched branches in the
  orchestrator's checkout (2026-11-06) and broke a run.
- Work only in your own worktree. Another worktree is read-only to you, and
  not even that unless the task names it.
- Stop what you started (`docker ps`, `ps -ef`) before reporting. No
  `run_in_background`, `nohup`, `&`, or polling loops.
