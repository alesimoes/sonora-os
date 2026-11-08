# Harness flow audit (2026-11-08)

Owner's requirement: work flows without a human. A task enters, is planned,
built, tested, integrated, PR'd, and every open PR has an owner that merges
it the moment CI is green. This map is read from `tools/orchestrator.py`
(`tick()`), `tools/harness/*.py` and the live daemon's `events.jsonl` /
`status.json` of 2026-11-07/08. **GAP** marks where work could stall with no
alert; **FIXED here** marks what the PR of this audit closed.

## 1. The tick (every `tick_s` = 30 s)

Order in `Orchestrator.tick()`: drain broker events -> kill switch ->
poll runs -> supervisor (every 180 s) -> open pending PRs -> auto-adopt ->
CLI mailbox -> owner chat -> watch PRs -> nightly -> flow (promote queued,
release holds, sync feature states) -> scheduler plan -> wakes -> launches ->
status.json.

Until this PR the tick was one straight sequence: an exception in any step
(a result.json of the wrong shape in `_on_result`, a `KeyError` in one PR's
poll) aborted every later step, every tick, until someone fixed the data
(34 `tick_error` in the live log; #504). **FIXED here**: `harness/tick_guard.py`
runs each step and each item of the per-run / per-PR / per-launch loops in
isolation, logs the first occurrence (`step_error` event, traceback on
stderr), counts repeats, and shows the standing error as a dashboard alert
until the step succeeds again.

## 2. Roles: when launched, when finished

| Role (agent) | Launched by | Pool / limit | Finished when |
|---|---|---|---|
| architect (`planner` kind, wake task) | `flow.request_plan` on feature promotion (`plan_request`); `_bounce` at the 2nd QA bounce (`bug_ruling`) | `plan`; one run per agent *name* (`running_names`): one architect at a time | run writes result.json; `on_wake_done` -> `ingest_plan`; invalid plan relaunches with the errors |
| lead | `pool_under_floor` wake from the scheduler (a track below floor with no eligible task); `plan_ready` after a plan is accepted; `task_done` / `qa_fail` / `agent_dead` for planned features | `plan`; one lead at a time | result.json; `ingest_assignments` creates the tasks |
| engineer (rt/ui/dsp/plugin/distro/driver/harness/flake-fixer by routing) | scheduler, task `queued` or `bugfix`, deps met, files not claimed | `build`; per-track floors, `cap`, `max_ready_unpr`, hourly budget | result.json `done` -> `qa`; `fail` -> attempt + backoff; `blocked` -> blocked |
| tester | scheduler, task `qa` | `qa`; `max_testers`, `local_builds` | `done` with no blocking finding -> `integrating`; `fail`/blocking -> `bugfix` (3rd bounce blocks) |
| integrator | scheduler, first task of a feature whose tasks are all `integrating`; or `ci_fix` with `fix_kind` conflict/bundle | `merge`; `max_integrators`, `max_open_prs` | result.json -> `_integrated` -> daemon pushes and opens the PR |
| fixer (the track's engineer) | `_red`: a failing check on the PR | `fix`; `max_fixers` | `_fix_done`: daemon pushes, back to `pr_open` |
| principal-engineer | `_escalate`: attempts spent, same cause red after `principal_after_fixes`, red > `principal_after_hours` | `principal`; `max_principals` = 1 | once per task; its own failure blocks |
| janitor | every `janitor_every` merges | `plan` seat | result.json |

Runs end by result.json, by a `done` hub message (grace `done_grace_s`), or
by the heartbeat verdict (`stalled`, `dead`, ceiling): then
`_count_failure` relaunches with backoff up to `max_attempts`, then
escalates or blocks.

## 3. Task lifecycle (who moves each state)

`queued` -(scheduler launches engineer)-> `building` -(engineer done)->
`qa` -(tester done)-> `integrating` -(integrator done, daemon pushes, PR
created)-> `pr_open` -(red check / conflict: `_red`)-> `ci_fix` -(fixer
done, pushed)-> `pr_open` -(green + mergeable: daemon merges)-> `merged`.
Side exits: `bugfix` (QA bounce, back to the engineer), `blocked`
(attempts spent, agent reported blocked, PR could not open 4x, 3rd bounce,
still red after the principal), `closed` (PR closed by hand; terminal).

Feature states mirror the tasks (`flow.sync_states`): `queued` ->
`planning` -> `distributing` -> `building` -> `qa` -> `integrating` ->
`pr_open` -> `merged`. A wake task is `queued` -> `merged` (served) or
`blocked`.

## 4. Where a task enters

- `orchestrator.py import-queue FILE` -> `flow.import_queue` -> features in
  `queued` (with `waits_on`).
- `orchestrator.py add --id ...` -> a one-task feature straight to `queued`
  work (no plan; `plangate` only for planned features).
- Lead `plan_request` message on `pool_under_floor` -> `flow.on_proposal`
  -> feature `queued` (shape normalised, #511).
- Nightly red on main -> `nightly.judge` -> `main-red-<sha8>` work task.
- Dashboard door (`dash_door`), adopt requests, auto-adopt sweep of open PRs.

## 5. Planning and the architect

`flow.promote_queued` opens planning for queued features while
`active < max_active_features` and `waits_on` is met; `request_plan`
reserves the ADR/protocol numbers (a `ForgeError` here leaves the feature
queued with only an event: `reserve_failed`) and raises the architect wake.
`wakes.is_pending` suppresses a second wake only while one is `queued`
(#509). Only one architect runs at a time (`running_names`), so the 13
features in `planning` are served serially. **GAP (cap, now visible)**: the
queue is a design limit; `stall_check` alerts a feature in planning for more
than `stall_alert_s` with no wake pending and re-issues the wake.
**GAP**: `_waits_unmet` treats a *blocked* feature as still pending, so a
queued feature waiting on it waits forever. **FIXED here**: alert
`waits:<feature>` names the blocked feature.

## 6. PR opening and the merge owner

`_integrated` marks the tasks integrated; `_open_pending_prs` (every tick)
runs `pre-pr.sh`, pushes (the daemon owns the push, #506), finds or creates
the PR, records `pr` on the feature (a PR created before a later step failed
is adopted, #498). Back-pressure: `max_open_prs` defers (`pr_deferred`
event once); 4 open failures block the feature.

Merge owners, after #512 and this PR:
- Daemon-opened PR, task `pr_open`: `_watch_pr` every `ci_poll_s`; green
  `Gate` + all checks pass + `mergeable == yes` -> `feature_merge.merge_if_green`
  squash-merges pinned to the judged head. `ci.yml` job `Gate` runs on every
  PR (`if: always()`), harness-only PRs included.
- Adopted PR (not opened by the daemon): `train.step` with the same rule,
  plus retarget of stacked PRs, update-branch / rerun on a first red,
  `train_slots` for CI concurrency.
- Any open, non-draft PR against main that no feature owns: `autoadopt.tick`
  every 300 s; the supervisor adopts after `adopt_grace_s` and alerts if
  that fails.
- **FIXED here**: `forge.enqueue` (the detached `enqueue-pr.sh` ->
  `merge-when-green.sh` one-shot) is no longer started when
  `daemon_merge` is on (the default); it was a second merger with a 4 h
  deadline and no re-queue.
- Red: `_red` -> `ci_fix` -> fixer; `supervisor_checks.check_prs` alerts a
  `ci_fix` with no run after `unowned_max_s`. **GAP**: a non-adopted
  feature whose tasks are `blocked` is not polled, so a hand merge leaves
  the board blocked (adopted ones are, via `_step_blocked`).

## 7. Limits and budgets that can stop the flow

| Limit | Where | Effect when hit | Visible? |
|---|---|---|---|
| `cap` (20 hard, 12 live) and hourly `max_runs_per_hour` | scheduler `seats` | nothing launches | gaps per track ("cap", "hourly run limit") in status |
| floors per track | scheduler pass 1 | below floor and no eligible task -> lead wake | gaps |
| `max_ready_unpr` | engineers of features not already ready | engineers refused ("CI budget") | gaps; siblings exempt (#502) |
| `max_open_prs` | integrator launch, PR open | PR deferred | `pr_deferred` event once; **now** `stall:` alert on the integrating task after 2 h |
| `max_active_features` | `promote_queued` | features stay `queued` | **GAP**: no alert (queue depth only on the board) |
| `max_integrators`, `max_fixers`, `max_testers`, `max_principals`, `local_builds` | `can()` | waits for a slot | `check_prs` for ci_fix; **now** `stall:` for the rest |
| `train_slots` | train | PR waits for a CI slot | train row "aguardando vaga" |
| file claims (broker) | `_launch` -> `claim_denied`, `not_before +60` | task retried every minute, forever if the claimant is gone (item 9) | **FIXED here**: `stall:` alert with the last `claim_denied` detail; release on run end is `harness.release-claims-on-run-end` |
| paused pools / tracks (`paused.json`) | `can()` | nothing launches in that pool | gaps "paused"; `_why_not_launched` names them |
| usage pause, kill switch | scheduler block | nothing launches | status fields |
| `not_before` backoff, HOLD (waits for another feature) | ready() | task invisible to the scheduler | HOLD is intentional; backoff shows in `_why_not_launched` |
| `done_grace_s`, `suspect_s`, `stalled_s`, `build_s`, `ceiling_s` | heartbeat | run killed and relaunched | `run_failed` events, supervisor `check_runs` |
| GitHub rate limit | train `pause_until` | the whole PR sweep stops for 600 s | train row |
| `principal_after_fixes` / `_hours` | `_red` | escalation, then block | `escalated`, blocked groups |

## 8. Communication and handoff

The hub (ADR-0047) works: 363 messages in the live log (daemon->lead 148,
lead->orchestrator 54, tester->lead 47, daemon->eng 34, tester->eng 11).
What was thin is the handoff between consecutive roles on the same work:
`build_prompt` got the brief, the branch and `note`; the tester never saw the
engineer's result.json, the integrator never saw the testers' findings and
unverified lists, a relaunch never saw the previous attempt's summary.
**FIXED here**: `harness/handoff.py` appends the newest finished result.json
of the task and of its sibling tasks (status, summary, commit, unverified,
findings, followups; bounded to 3.5 kB) to every work prompt.

Broker resilience: the broker lives inside the daemon; a restart (20
self-updates in the last 3000 events) gave it a new port and token, so
agents launched before it got WinError 10061 and the edit hook, failing
closed, denied every write. **FIXED here**: `BrokerHost` pins port and
token across restarts (`.harness/broker.json`, `.harness/broker.token`,
owner-only), falling back to a free port if the old one is taken.
**GAP**: during the restart window itself (seconds) a hook call still
fails; the hook could retry once before denying.

## 9. Remaining gaps, prioritised (one-line fix each)

1. `claim` release on run end (task `harness.release-claims-on-run-end`
   is queued): release every claim of a run's name in `_end_run`, not only
   for fix-like roles.
2. Lead wake cooldown (`harness.lead-wake-cooldown`, queued): rate-limit
   `pool_under_floor` wakes per track to one per N minutes.
3. One architect at a time: give planner runs per-feature names
   (`architect-<n>`) and a `max_planners` pool instead of `running_names`.
4. `max_active_features` queue: a `warn` alert when features sit `queued`
   behind the cap for hours (data is in status: features by state).
5. Blocked non-adopted feature with a PR is not polled: poll it like the
   train's `_step_blocked` so a hand merge or close ends the block.
6. `_waits_unmet` on a blocked dependency: let the owner close or unblock
   from the dashboard; the alert now names it.
7. Progress/BACKLOG staleness (item 12): a chore that reads merged features
   and runs `tools/progress.sh set` is the coordinator's; add a nightly
   diff of `progress.json` vs merged features to the status board.
8. Hook retry during the restart window: one retry after 2 s in
   `tools/harness.py run_hook` before "edits denied".
