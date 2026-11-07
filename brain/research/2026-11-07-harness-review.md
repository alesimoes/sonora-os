# Harness review, 2026-11-07: who watches the watchers

Owner's complaint: "we (the owner and the coordinator) have become the
orchestrators; nothing monitors the agents." This is a reading of the whole
harness as it runs today (daemon pid 9420, `orchestrator.py run --live`,
status.json, events.jsonl, the lock directory, 92 worktrees under
`.claude/worktrees`), every single point of failure found, what has no
owner, and what is more machinery than the project needs. The fixes that
land with this review are in the last section; the rest are BACKLOG lines.

## 1. How the pieces fit

```
 owner / coordinator        gh (GitHub)            this machine
 ──────────────────         ───────────            ────────────────────────────
 orchestrator.py adopt ─┐   PR list, checks,       build-slot.sh  one lock dir
 orchestrator.py pause  │   statuses, merges        (%LOCALAPPDATA%/sonora)
 STOP / STOP.NOW files  │        ▲                        ▲
                        ▼        │                        │ every heavy step
              ┌─────────────────────────────────┐         │
  tick (30 s) │ Orchestrator.tick()             │  local-ci.sh  (the gate:
              │  drain broker events            │   detached bash per batch,
              │  _poll_runs   heartbeat.classify│   rc + summary files)
              │  localci.prune/poll  ◄──────────┼──────────┘
              │  _open_pending_prs              │
              │  autoadopt.tick ──► train.adopt │
              │  _watch_prs ──► Train.step      │──► claude -p (one per run,
              │  flow.tick (planner wakes)      │     own worktree, stream.log,
              │  scheduler.plan ──► _launch     │     result.json; MCP broker
              │  _write_status ──► status.json  │     for send/recv/claim)
              └─────────────────────────────────┘
                 state.json (one writer)   events.jsonl   dashboard.py reads
```

Nine modules carry the policy: `scheduler` (seats, floors, claims),
`heartbeat` (liveness from stream mtime, git status, lock listing), `train`
(an adopted PR's next cheapest step), `localci` (one batched builder,
bisect on red), `flow`/`plans`/`plangate` (planner roles and their
validators), `broker`/`mailbox`/`brokerhost` (agent messages and file
claims), `dispatch`/`tiers`/`routing` (which agent and model), `autoadopt`,
`forge` (gh). Judgement is in the agents; the daemon counts and retries.

## 2. Single points of failure

| Component | Failure mode seen or latent | Who notices today | Now |
|---|---|---|---|
| The daemon process | Died when the disk filled (`tick_error OSError(28)` in events.jsonl); four `PermissionError(13)` ticks since; nothing restarts it | the owner, hours later | `tools/supervisor.py` (outer watchdog) restarts it; alert + toast |
| The daemon's checkout | Ran a day-old tree; auto-adopt that landed on main never ran | the owner | outer check `daemon:stale-code` names it; the restart on main is the merge-throughput PR's re-exec |
| `state.json`, one writer | A CLI `adopt` while the daemon ran was overwritten by the daemon's in-memory store | the owner | fixed before (adopt requests via file); the rest of the CLI goes through the daemon in the merge-throughput PR |
| `build-slot.sh` lock dir | Reaps only when another caller polls; a holder of another shell kind is never probed; any argv is a command (`--status` queued as a job); the 4 h wait cap runs the build anyway, unlocked | nobody | unknown flags refused; the supervisor reaps dead and orphan entries and alerts on long waits/holds |
| Lock queue vs. agents | A run whose build waited in the queue had no stream output, so `heartbeat.classify` stalled and killed it (`holder_in_list` matched `holds` lines only) | nobody: the run was "stalled" and relaunched, work lost | a waiter counts as building |
| Local-ci batches | A batch kept gating the head of a PR merged by hand (~5 h); a replaced head stayed queued | the owner | `prune(head_is_gone)` landed 11-06; the supervisor drops heads whose train row is merged/closed and alerts on heads with no open PR; replaced heads are the merge-throughput PR's |
| Flakes | A red step was final: the batch was bisected, ~16 h lost in 48 h | the owner | merge-throughput PR: one automatic rerun before bisecting |
| Red / conflicting PRs | `_red` moves the task to `ci_fix`; when the fixer pool is full, paused, or the task is in backoff, it sits with no run and no one is told | nobody | supervisor `check_prs`: backoff cleared, else alert with the reason |
| Disk | No threshold anywhere; the janitor runs every 3 merges only (`janitor_every`) | the owner, when a build goes red | low → janitor wake; critical → build pools paused; alert says "a red build now is the disk" |
| `status.json` as the only board | No place for "something is wrong": `blocked` and `waits_on_owner` are task states, not health | nobody | `alerts` list, rendered by the dashboard, desktop notification per alert |
| `dashboard-up.sh` | Hardcodes the daemon's worktree path (`agent-a7018d355a2572f2a`) | breaks silently on the next daemon worktree | BACKLOG: resolve from a pointer file |
| `forge` / gh | Rate limits pause the train for 10 min; a list failure makes `head_is_gone` say "not gone" (safe) but also hides merged PRs from prune | nobody | supervisor alerts `queues:list` (warn) |
| Broker | Runs in the daemon; a daemon restart changes port and token; clients of the old daemon finish on result.json alone | by design | - |
| Agents' own processes | A `claude -p` that exits naturally leaves children (a `build-slot.sh docker …` it backgrounded) in the lock queue | nobody | supervisor kills lock entries whose cwd is an ended run's worktree |

## 3. What has no owner or watchdog

- The daemon itself: no pid file, no restart, no "am I current" check.
  Both are the outer watchdog's now; it writes `supervisor-alerts.json`,
  which the daemon folds into `status.json`.
- The lock directory: a shared mutable thing with no process responsible
  for it. The supervisor's `check_lock` is that process.
- `ci_fix` tasks without a run, `pr_open` tasks the train marked "fixer"
  or "conflito" that never moved: nothing timed them. `check_prs` does.
- Disk space and worktrees: 92 worktrees today; `clean.sh` exists but ran
  only when someone asked. `check_disk` wakes the janitor by threshold.
- The agents' liveness is well covered (stream mtime, git status, lock
  holder, `done` message, result.json, ceiling, usage-limit parsing) but a
  suspect run was only ever killed, never asked. `check_runs` nudges it
  once through the broker before the heartbeat kills it.
- The dashboard: nothing tells it the daemon is unhealthy; it shows the
  last status.json forever. `alerts` and `daemon_pid` are in status.json
  now; a stale `updated` is still only visible as a timestamp (BACKLOG:
  grey the board when `updated_ts` is older than two ticks).

## 4. Over-engineered: candidates for removal

The project got too big to develop; these are the parts whose cost is now
higher than their value. None is removed in this PR (each is a decision of
its own); the recommendation is to remove them one PR at a time.

1. **File claims through the broker** (`broker.claim_for`, 214
   `claim_denied` events in 68 h, more than `launched` + `result`). Adopted
   PRs skip claims already (`SONORA_UNCLAIMED_OK`); every task has its own
   worktree and branch, and conflicts are the integrator's job at merge
   time. Remove claims and the `claim`/`release` MCP tools; keep `send`,
   `recv`, `whoami`.
2. **Bundles** (`train.bundle`, `open_bundle`, `bundle_brief`, `_member_to_pr_open`):
   five bundles ever; local-ci already batches every ready PR into one
   build, which was the reason for bundles. Remove.
3. **Shadow mode** (`shadow.py`, `--shadow`, `--shadow-agents`): the
   rollout tool of 11-04; the daemon has been live since. Remove.
4. **Three staffing passes with floors per track** (`scheduler.plan`: floor
   round-robin, planner seats, burst): with local-ci gating one batch at a
   time, the bottleneck is the builder, not seats. One priority queue
   (fix > tester > integrator > engineer > planner) under one cap is
   enough; the `pool_under_floor` lead wake goes with it.
5. **Two adoption doors plus the CLI path** (`autoadopt.tick` requests,
   `train.adopt(auto=True)`, `orchestrator.py adopt` offline): one door,
   the daemon's sweep, and the CLI as a request file only (the
   merge-throughput PR does this).
6. **Portuguese phase strings in `train`** ("adotado", "CI rodando",
   "aguardando vaga"): the dashboard matches on them, tests compare them,
   and `localci` and `status.py` use English states. One vocabulary.
7. **`heartbeat.Activity` worktree signatures** (a `git status` per run per
   tick): the stream log mtime and the lock listing decide in practice.
   Measure; likely removable.
8. **`local-ci.sh` as the one place for the context table** while
   `ci.yml` `changes` repeats it and `train.LOCAL_CI_JOBS` names the
   GitHub jobs it replaced. The GitHub table now has a `harness_only`
   output as well; this review documents the map (section 6) so the three
   stay in step until one generator writes them.

## 5. What lands with this review

- `tools/harness/supervisor.py`, `supervisor_checks.py`,
  `supervisor_sys.py`: the in-daemon watchdog, run from
  `Orchestrator.tick()` every `interval_s` (180 s), with the checks of
  section 2. Every action is logged with its reason to
  `.harness/supervisor.log` and as a `supervisor` event; alerts are in
  `status.json["alerts"]`; the owner gets a desktop notification per alert,
  repeated hourly while it stands. Thresholds: `[supervisor]` in the
  daemon's `planning/orchestrator/orchestrator.toml` (an untracked local
  file; the defaults below hold when the table is absent):

  ```toml
  [supervisor]
  interval_s = 180            # seconds between sweeps
  lock_wait_alert_s = 7200    # a lock waiter older than this is an alert
  lock_hold_alert_s = 10800   # a lock holder older than this is an alert
  unowned_max_s = 1800        # a red PR with no fixer run for this long
  queue_orphan_s = 900        # a queued head with no open PR for this long
  adopt_grace_s = 600         # an open PR the daemon's sweep did not adopt
  disk_low_gb = 40            # janitor wake (once per disk_janitor_every_s)
  disk_critical_gb = 15       # build pools paused, restart refused
  notify = true               # desktop notification per alert
  notify_repeat_s = 3600
  daemon_stale_s = 300        # outer: status.json older than this
  daemon_restart = true       # outer: restart a dead or hung daemon
  ```
- `tools/harness/supervisor_outer.py`, `tools/supervisor.py`: the outer
  process. Daemon alive (restart on a dead pid, kill-and-restart on a hung
  one, refused while the disk is critical), daemon on origin/main's harness
  code (alert), disk (alert). Start it next to the daemon:
  `python tools/supervisor.py --root <daemon checkout>`.
- `scheduler.holder_in_list`: a lock waiter counts as building.
- `build-slot.sh`: unknown flags are refused (`tools/test-build-slot.sh`).
- `.github/workflows/ci.yml`: `harness_only` output; a harness-only PR runs
  the Harness job (unit tests, shellcheck of the lock and gate scripts,
  `test-build-slot.sh`) and the cheap catalogue jobs, not the Shell-scripts
  job with its GHCR images.
- `tools/dashboard.html`: the alerts panel.

## 6. The map: harness paths vs. build-dependent scripts

Harness-only (GitHub: Harness job; local gate: "nothing to build"):
`tools/harness/**`, `tools/harness*.py`, `tools/orchestrator.py`,
`tools/supervisor.py`, `tools/dashboard*`, `tools/build-slot.sh`,
`tools/test-build-slot.sh`, `.claude/**`, `planning/**`, `brain/**`,
`docs/**`, `*.md`.

Mapped exception, builds every Sonora context although it lives in
`tools/`: `tools/build-*.sh` (except `build-slot.sh`), `tools/local-ci.sh`,
`tools/Dockerfile*`, `tools/*-entrypoint.sh`, `tools/test-engine-docker.sh`,
`tools/test-bwf-interop*.sh`, `tools/verify-recording.py`, `tools/*.cmake`,
`tools/*.supp`. `.github/workflows/ci.yml` itself is not harness-only: a
change to it must run the jobs it defines.

The local classifier (`tools/local-ci.sh`, "which context") already says
"nothing to build" for the harness paths but maps `tools/build-slot.sh`
through `tools/build-*` to all three Sonora contexts; the merge-throughput
PR owns that table (BACKLOG: add `tools/build-slot.sh|tools/test-build-slot.sh) ;;`
before `tools/build-*`).

## 7. Follow-ups (BACKLOG lines)

- [ ] harness: remove file claims and the claim/release MCP tools (section 4.1)
- [ ] harness: remove bundles (4.2) and shadow mode (4.3)
- [ ] harness: one priority queue under one cap; drop floors and the pool_under_floor wake (4.4)
- [ ] harness: English phase vocabulary in train, dashboard and tests (4.6)
- [ ] harness: measure `heartbeat.Activity` cost; drop if the stream mtime and lock decide (4.7)
- [ ] harness: `local-ci.sh` classifier: `tools/build-slot.sh` is harness-only (6)
- [ ] dashboard: resolve the daemon's status.json from a pointer file, not a hardcoded worktree; grey the board when `updated_ts` is stale
- [ ] harness: a duplicate `--step plugins` waiter sits behind an identical holder in the lock (seen 2026-11-07 06:30); the per-step wrapper in local-ci.sh queues a step twice when the plugins and pluginhost steps share a build (merge-throughput PR item "record and pluginhost steps without duplicate builds" may cover it; verify)
- [ ] harness: start `tools/supervisor.py` from the SessionStart hook or a Task Scheduler entry so it survives the terminal
