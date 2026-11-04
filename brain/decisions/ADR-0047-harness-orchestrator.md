# ADR-0047 — Harness orchestrator: a Python daemon that runs headless Sonnet roles

- Status: Proposed (architect; recommended defaults, owner not available)
- Date: 2026-11-04
- Scope: development harness only (`tools/`, `.claude/agents/`, `planning/`).
  No product code, no contract, no protocol version.

## Summary

*Navigation only; the sections below are the decision.*

- §1: the control loop is a Python daemon, `tools/orchestrator.py`, not a
  long-lived Claude session. Roles run as headless `claude -p --model sonnet`
  processes, one worktree each.
- §2: state is one SQLite file, `planning/orchestrator/state.db` (git-ignored),
  with a state machine from `queued` to `merged`.
- §3: heartbeat = process liveness plus git/worktree/result-file activity; the
  agent does not need to cooperate. Relaunch from the WIP branch, 3 tries,
  then a "waits on the owner" note.
- §4: eight roles, each with a structured `result.json`.
- §4a (owner refinement): the flow is architect -> lead -> engineers and
  testers -> integrator; a plan file, the lead's assignment table, and
  one feature = one branch = one PR in the store and on the dashboard.
- §4b (owner addition): agents talk through files in a git-ignored
  `.harness/` (inbox/outbox, plan, claims, board, events), atomic writes,
  file claims that stop two engineers editing one file, polled by a hook.
- §5: three pools (build, qa, fix) with floors, a 20 cap, a CI budget, kill
  switch and dry-run.
- §6: the daemon writes `status.json`; the dashboard reads it.
- §7: three PRs, each tested with a fake `claude`.

## Context

Today the coordinator is a Claude session that launches agents with the Agent
tool, tracks them by hand, and ends every turn with `tools/bg-status.sh`. It
burns context (handoffs every 3-4 merges), stops when the session stops, and
cannot tell a dead agent from a slow one except by looking. The owner wants an
orchestrator that fires the architect and engineers, runs QA after every
build, sends bugs back, merges one PR per feature, has a different agent fix
red CI, relaunches dead agents by heartbeat and never leaves the queue without
builders, testers and PR fixers.

Fixed constraints: Sonnet only (owner, 2026-11-03/04); at most 20 concurrent
subagents (`CLAUDE_CODE_MAX_CONCURRENT_SUBAGENTS`); staffing targets of
>= 8 DAW, 4 OS and 2 plugin agents truly working (an agent that is only
waiting on a build does not count); PRs only end to end; the coordinator
reserves ADR and protocol numbers; GitHub Actions runner time is the real
bottleneck (the sanitizer job alone is about 50 minutes); privileged docker
tests cannot run in a subagent. Pieces to reuse: `tools/enqueue-pr.sh`,
`merge-when-green.sh`, `bg-status.sh`, `build-slot.sh`, `pre-pr.sh`,
`planning/AGENT-QUEUE.md`, `tools/dashboard.py`, `.claude/agents/*.md`.
(At the time of writing `pre-pr.sh`, `build-slot.sh`, `dashboard.py` and
`AGENT-QUEUE.md` live on other branches, not on `main`; the plan below treats
them as interfaces and falls back to no-ops when absent.)

## Decision

### 1. Where the loop lives: a Python daemon

`tools/orchestrator.py` (standard library only, Python 3.9+, runs under Git
Bash or WSL on the owner's machine) owns the loop. It launches each role as

    claude -p --model sonnet --agent <role> --output-format stream-json \
           --max-turns N --permission-mode <as the role file> "<task prompt>"

with `cwd` set to that task's worktree. The `--model sonnet` flag is hard-coded
in one function (`launch()`); a unit test fails if any argv lacks it, and the
daemon refuses to start if `CLAUDE_MODEL` or an agent file says otherwise.

Why not a long-lived Claude session as orchestrator:

- Its context fills; it needs handoffs and auto-compaction, and the part that
  most needs to be exact (counting live agents, honouring caps) is the part a
  model gets wrong after compaction. A daemon's counters do not decay.
- It cannot be tested without spending quota; the daemon can, against a fake
  `claude`.
- Its heartbeat is the thing being monitored: if the session dies, nothing
  relaunches it. A daemon is restarted by one line (Task Scheduler / systemd
  user unit / the SessionStart hook) and recovers from the state file.
- A script cannot call the in-session Agent tool, but it does not need to:
  `claude -p` processes are real agents with the same `.claude/agents` files.

Costs we accept: headless agents cannot be steered mid-run (corrections go in
the next relaunch prompt); each is an independent process with its own startup
cost; permissions must be pre-granted in the agent files and
`.claude/settings.json` because nobody answers a prompt. The daemon never
decides *what* to build or whether a diff is good; it only schedules. Judgement
stays in the `lead`, `architect` and `qa` roles, so the daemon stays small
(target under 1200 lines).

The in-session coordinator remains for the owner's ad-hoc work and for
privileged docker tests (below). It reads `status.json` instead of polling.

### 2. State store

One SQLite file (`sqlite3` is in the standard library; WAL mode;
`BEGIN IMMEDIATE` per transition gives atomicity and a single-writer lock for
free). A JSON file would need our own lock and rewrite-whole-file atomics and
would not survive two writers (daemon plus a manual `orchestrator.py set`).
Located in the main checkout under `planning/orchestrator/`, git-ignored;
worktrees are under `.claude/worktrees/<task>`.

Tables: `tasks`, `runs`, `events`, `reservations`.

- `tasks(id, backlog_id, track, pool, state, branch, worktree, pr, adr_no,
  protocol_no, attempts, depends_on, created, updated, note)`
- `runs(id, task_id, role, pid, started, last_beat, ended, exit, result_path,
  session_id)`
- `events(ts, task_id, kind, detail)` append-only; the audit trail.
- `reservations(kind, number, task_id)` for ADR and protocol numbers.

State machine (one task is one BACKLOG item, one branch, one PR):

    queued -> designing (only if the lead marks needs_adr)
           -> building -> qa -> integrating -> pr_open -> merged
                           |                     |
                           v                     v
                        bugfix -> qa          ci_fix -> pr_open
    any state -> blocked (retries exhausted or owner decision needed)

- `building`: an engineer runs; ends with `result.json {status: done}` and a
  pushed WIP-committed branch.
- `qa`: a fresh qa agent reviews the diff and runs the tests the PR claims.
  `pass` -> `integrating` (or `pr_open` when there is one engineer);
  `fail` with findings -> `bugfix`, the same engineer role relaunched with the
  findings file and the branch. QA/bugfix may loop 3 times, then `blocked`.
  Only blocking findings return a task; non-blocking ones are written to the
  `followups` list in the result and become a per-track follow-ups task
  (existing rule).
- `integrating`: only for multi-engineer features (sub-branches). The
  integrator merges them into the single feature branch. Single-engineer tasks
  skip it.
- `pr_open`: the daemon runs `tools/pre-pr.sh`, opens the PR, calls
  `tools/enqueue-pr.sh`. A GitHub poll (every 120 s, `gh pr list --json
  number,statusCheckRollup,mergeable,state`) moves it to `ci_fix` on a red
  check or a conflict, to `merged` when the PR is merged.
- `ci_fix`: a ci-fixer (red check) or conflict-fixer (conflict) is launched.
  It pushes once, and the task returns to `pr_open`. 3 attempts, then
  `blocked`.
- `merged`: worktree removed, branch deleted, janitor run counted.

Transitions are a function `advance(task, event)` in one table-driven module,
unit-tested exhaustively; no role writes the state.

Numbers: `reservations` hands out the next ADR number as
max(main, all open PRs via `gh pr diff --name-only`, own reservations) + 1, and
the next protocol version likewise, at the moment the task enters `designing`
or `building`. The number goes in the task prompt. This is the coordinator's
existing duty, moved into code.

### 3. Heartbeat

The agent does not have to cooperate (a model forgetting to touch a file would
look dead). The daemon samples every 30 s per run:

1. Process: `pid` alive (and its children, since `claude` spawns builds).
2. Activity: the newest of the mtimes of (a) the stream-json output file the
   daemon captures, (b) `git -C worktree` HEAD / index / `git status
   --porcelain` count, (c) any file under the worktree modified in the last
   interval, (d) the docker child process CPU time if `build-slot.sh` holds a
   lock for this worktree. `last_beat` is that maximum.
3. Result: `result.json` present and valid means finished, whatever the pid.

Classification: pid gone without a valid result = dead, relaunch at once.
pid alive and `now - last_beat` > 15 min = suspect; > 30 min = stalled, kill
the process tree and treat as dead. Build tasks that hold a build slot get
60 min (the sanitizer and ISO builds are long); qa runs 45 min; hard ceiling
per run 3 h. All thresholds in `orchestrator.toml`. The stream-json output
doubles as the progress log the dashboard shows.

Relaunch: before relaunching, the daemon commits any uncommitted work in the
worktree to the branch as `WIP: orchestrator checkpoint` (never pushes to main,
never force-pushes) and starts a new run of the same role with the prompt
"continue; read `git log`, `git diff origin/main...HEAD` and the task file".
Fresh context each time, which is cheaper than resuming a session and avoids a
poisoned session. Counted in `tasks.attempts`: 3 relaunches per role per state,
with backoff 1, 5, 15 min. A usage-limit exit (429 text in the stream) does not
count: the daemon pauses all launches until the reset time it parses (or 30 min
if unparseable) and then resumes, per the existing owner rule.

Escalation: after the retries the task is `blocked`, the worktree and branch
are kept, a section is appended to the current handoff under "waits on the
owner" (title, branch, last 20 lines of the stream, the reason), and the pool
slot is given to the next queued task. The daemon never stops and never asks.
Orphans: on startup it reconciles `runs` with `ps` and `docker ps` and
adopts or kills strays (the job of `bg-status.sh --kill`, which it calls).

### 4. Roles and contracts

All roles are `.claude/agents/<role>.md` files (`model: sonnet`). The prompt
the daemon appends is generated from the task row and always carries: task id,
BACKLOG id, branch, worktree, numbers reserved, `result_path`. Every role ends
by writing `result.json` (schema `planning/orchestrator/result.schema.json`,
validated by the daemon; an invalid file is treated as a dead run):

    { "role": "...", "task": "...", "status": "done|fail|blocked",
      "summary": "<= 20 lines", "commit": "<sha>", "branch": "...",
      "findings": [ {"severity": "blocking|nonblocking", "where": "path:line",
                     "text": "..."} ],
      "followups": ["BACKLOG-style line"], "unverified": ["..."],
      "tasks": [ ... lead only ... ], "pr": 0 }

| Role | Reads | Returns |
|---|---|---|
| lead | `tools/progress.sh list --open --track T`, the BACKLOG heading for each candidate (never whole), `status.json`, open PRs | `tasks[]`: item, track, pool, files it touches (for the no-overlap rule), `needs_adr`, depends_on, one-paragraph brief. Does not write code. Run when a pool is under floor and the queue has fewer than 2x floor `queued` tasks. |
| architect | brief, CLAUDE.md, INDEX, ROADMAP, ADR Summaries | an ADR file plus INDEX line, committed on the task branch; the ADR travels with the code it decides, so this is a stage of the same task, not a PR. |
| engineer | the brief, ADR sections, the module it owns; one of the existing `rt-engineer`, `dsp-engineer`, `ui-engineer`, `driver-engineer`, `plugin-engineer`, `distro-engineer` chosen by track | commits on the branch, a `result.json` with commands run and what they printed, `unverified`, the proposed `[x]` line and follow-ups for the PR text. Never edits BACKLOG or progress. |
| qa | the diff, the PR claims, the ADR; fresh each time, never the author | `findings[]`; may run the narrow tests the claims name (inside build budget) and `rt-safety-auditor` checks on RT paths; cannot edit the code. `pass` only if no blocking finding and every claim is verified or listed `unverified`. This is today's `code-reviewer`, extended. |
| integrator | sub-branches of one feature | one feature branch, conflicts resolved; no PR; today's `integrator`. |
| ci-fixer | `gh run view --log-failed` (trimmed), the diff | one fix push for a red check, no scope change, no test deletion or weakening (qa re-checks the fix diff only). |
| conflict-fixer | `git merge origin/main` conflicts | resolved branch, tests of touched files run. May share a prompt with ci-fixer but is a separate role so the pool can prioritise it. |
| janitor | `bg-status.sh`, `docker ps`, worktree list | removes merged worktrees, stale containers, scratch logs; today's `janitor`. Daemon-triggered every 3 merges. |

The `lead` is the only role that creates tasks; QA the only role that moves
`qa` -> `integrating`; the daemon alone opens PRs and enqueues them
(`enqueue-pr.sh` is already idempotent). "No two agents on the same files" is
enforced by the daemon: a task is launched only if its declared file set does
not intersect any active task's set; the lead's file list is checked again by
QA against the real diff.

Not delegable to a headless subagent: privileged docker / loop-device tests.
A task whose brief says `needs_privileged` ends `building` -> `qa` with that
check listed `unverified`, and the daemon writes a `privileged_pending` line
to `status.json`; the in-session coordinator (or the owner) runs it per the
standing "run privileged tests myself" rule and posts the result to the PR.
The task does not block on it unless the PR is partial by design.

### 4a. Feature flow: architect, lead, engineers and testers, integrator

Owner refinement (2026-11-04). The unit of delivery is a **feature**, not a
task. Flow:

    feature --> architect --> plan.json --> lead --> assignments
        --> engineers (one sub-branch each) + QA per finished task
        --> integrator (merge sub-branches, pre-pr, ONE PR, enqueue)
        --> ci-fixer / conflict-fixer on red --> merged

1. **Architect plans.** For a BACKLOG item or group it writes
   `.harness/plan/<feature>.plan.json` (a copy is committed on the feature
   branch under `planning/orchestrator/plans/`; the daemon ingests and
   validates it): the ADR (if any) and the reserved numbers, then a task list
   ordered contract first, then engine, UI, tests, docs slices. It owns file
   ownership: every task lists `files` (globs) that no other task of any
   active feature may list; the daemon rejects a plan with overlap. It writes
   no product code.
2. **Lead distributes.** It reads the plan and the live `status.json`, writes
   `assignments.json` and starts nothing itself. It never writes product code.
3. **Engineers and testers** work the assignments; each finished task gets its
   own fresh QA agent (the lead's table names the tester role per task).
4. **Integrator merges.** When every task of the feature is `qa: pass`, the
   integrator merges the sub-branches into the feature branch in the plan's
   order, resolves conflicts, runs `tools/pre-pr.sh`, and the daemon opens the
   single PR and runs `tools/enqueue-pr.sh`. A separate ci-fixer takes over on
   red. A single-task feature skips the merge but not the rest.

Plan-file schema (`plan.schema.json`, version 1):

    { "version": 1, "feature": "time.varispeed", "backlog_ids": ["..."],
      "track": "daw|os|plugin", "feature_branch": "feat/<feature>",
      "reserved": { "adr": 47, "protocol": null, "formats": [] },
      "adr": "brain/decisions/ADR-0047-....md" | null,
      "tasks": [
        { "id": "t1", "kind": "contract|engine|ui|tests|docs",
          "title": "...", "brief": "<= 15 lines, what and done-when",
          "files": ["contracts/...", "engine/..."],
          "depends_on": [], "agent": "rt-engineer|ui-engineer|...",
          "needs_privileged": false, "accept": ["command that must pass"] } ],
      "merge_order": ["t1","t2","t3"],
      "pr": { "title": "...", "partial": false, "partial_reason": null } }

A contract task has no dependencies and merges before dependants start from
its sub-branch (CLAUDE.md contract rule); when a contract is a separate
feature, `depends_on_feature` names it and the feature waits in `queued`.

Lead's assignment table (`assignments.json`, one row per task, rewritten on
every change and appended to `events`):

| task | engineer agent | tester | sub-branch | order | start-from | slot |
|---|---|---|---|---|---|---|
| t1 | rt-engineer | qa | `feat/<f>/t1` | 1 | feature branch | build |
| t2 | ui-engineer | qa | `feat/<f>/t2` | 2 | `feat/<f>/t1` | build |

The daemon enforces what the lead decides: it launches a row only when
`depends_on` tasks are `qa: pass`, the files are free, and a pool seat is
open; the lead can reorder or reassign, never exceed floors or overlap. A
lead that returns an invalid table is relaunched, not obeyed.

Store additions: table `features(id, plan_path, branch, track, state, pr,
adr_no, protocol_no, partial, created, updated)`; `tasks.feature_id`
references it, and a task's `branch` is its sub-branch. Feature state is
derived: `planning` -> `distributing` -> `building` -> `integrating` ->
`pr_open` -> `ci_fix` -> `merged` (or `blocked`). Exactly one row of
`features` owns a `pr`, and a unique index on `pr` forbids two features
sharing one; a task never has a PR of its own. The task-level state machine
in §2 applies per task up to `qa: pass`; `integrating`, `pr_open`, `ci_fix`
and `merged` are feature states. The `designing` state of §2 is the feature's
`planning` state, and the lead does not create tasks (the architect's plan
does), superseding the lead wording in the §4 table.

Dashboard view (`status.json` `features[]`, rendered by `tools/dashboard.py`):
a table grouped by feature, one line per PR: feature, track, PR number and
link, feature state, tasks done/total with a per-task strip (state letter and
agent), CI status, attempts, age, blocked reason. Below it, open PRs by
feature against `max_open_prs`, and the "waits on the owner" list.

### 4b. Inter-agent communication: `.harness/`

Owner addition (2026-11-04). Purpose: no code conflicts, everyone in tune.
Files, not a socket: a headless `claude -p` agent has no listener, so a socket
would need a broker process, a port, and a client the model must drive
through Bash; files are inspectable, survive daemon and agent restarts,
replay in tests and need no extra moving part. A socket is justified only if
polling latency measured in the fake-claude tests exceeds 5 s, which we do not
expect (agents work in minutes).

**Location.** `<main checkout>/.harness/`, added to `.gitignore`. Worktrees
share it because the daemon passes `SONORA_HARNESS=<abs path>` (plus
`SONORA_AGENT_ID`, `SONORA_TASK`, `SONORA_FEATURE`) to every run; agents and
`tools/harness.py` resolve it from that variable, never from the cwd. If the
variable is unset the helper exits non-zero (fail loudly at start-up).

    .harness/
      plan/<feature>.plan.json        architect output, read-only after ingest
      board.json                      task states, rewritten by the daemon only
      claims/<path-hash>.json         one file per claimed path (see below)
      inbox/<agent-id>/<seq>-<type>-<msgid>.json   messages to an agent
      outbox/<agent-id>/...           messages an agent has sent (audit copy)
      acks/<agent-id>/<msgid>         empty file: message read
      runs/<run-id>/{stream.log,beat,result.json}
      events.log                      append-only JSON lines (daemon + helper)
      STOP, STOP.NOW                  kill switch (see §5)

`agent-id` is `<role>.<task>.<n>`, unique per run, plus the standing ids
`daemon`, `lead.<feature>` and `architect.<feature>`. The daemon routes: a
message is written once to the sender's outbox and delivered (hard-linked or
copied, then renamed) to the recipient's inbox by `tools/harness.py send`, so
the agent needs no routing knowledge. Broadcast = `to: "feature:<f>"` expands
to every live agent of that feature.

**Message schema** (`msg.schema.json`, version 1):

    { "v": 1, "id": "<ulid>", "seq": 17, "ts": "...", "from": "...",
      "to": "...", "feature": "...", "task": "...",
      "type": "assign|ask|answer|blocked|file_claim|file_release|handoff|"
              "qa_report|bug|done",
      "reply_to": "<id>|null", "needs_ack": true,
      "body": { ... type specific ... } }

Types: `assign` (lead to engineer or tester: task, branch, start-from, files,
accept); `ask` / `answer` (any to lead or architect and back; the lead answers
within its own turn or relays to the architect; unanswered `ask` after 20 min
makes the daemon append the recommended default, per owner rule, and answer
with it); `blocked` (agent cannot proceed, names the blocker; the lead
reassigns or escalates); `file_claim` / `file_release` (see claims);
`handoff` (engineer to qa: sub-branch, commit, commands run and printed,
unverified); `qa_report` (qa to lead and daemon: pass/fail and findings);
`bug` (qa to engineer: one blocking finding with where and text; the engineer
returns it fixed in a `handoff`); `done` (final, mirrors `result.json`).
Unknown types are ignored by agents and logged by the daemon.

**Atomic writes and ordering.** A writer creates `inbox/<id>/.tmp.<pid>.<rand>`
in the same directory, fsyncs, then `os.rename`s to the final name (atomic on
one filesystem; the daemon verifies at start-up that `.harness/` and the
worktrees share a volume, else it refuses to start). Readers ignore names
starting with `.tmp.`. Order is the zero-padded `seq`, allocated per recipient
under an `O_EXCL` lock-file (`inbox/<id>/.seq.lock`, held for microseconds);
delivery is at-least-once, handlers are idempotent on `id`. Ack =
creating `acks/<id>/<msgid>`; the sender or the daemon re-delivers an
unacked `needs_ack` message after 10 min and, after two, tells the lead.

**Cheap polling without a background shell.** Agents cannot listen. The
daemon installs, in each worktree's `.claude/settings.local.json`, a
`PostToolUse` hook (and `UserPromptSubmit`) that runs
`tools/harness.py inbox --brief`: it does one `os.scandir` of the agent's
inbox, prints nothing when empty (zero tokens) and otherwise prints one line
per new message ("17 bug from qa.t2.1: src/x.cpp:40 ..."), moving it to
`read/`. A message the agent must obey is thus injected after its next tool
call, a few seconds later. The role prompts say: before finishing, run
`harness.py inbox` once more and answer every `needs_ack`; a final `done` is
refused by the helper while unacked blocking `bug` or `ask` messages exist.
Hooks fire between tool calls, so a long single Bash build delays delivery to
the end of that call, which is acceptable. (Assumption: headless `-p` runs
honour project hooks; see Unverified 6.)

**File-claim rules (no two engineers on one file).** Ownership is decided
three times, from coarse to fine:

1. Plan time: the daemon rejects overlapping `files` globs across active
   features and tasks (§4a).
2. Launch time: the daemon writes the task's globs to `claims/` as `owner:
   task` entries; a task whose globs hit an existing claim is not launched.
3. Edit time: an engineer sends `file_claim` for any path outside its globs
   before editing it (e.g. `CMakeLists.txt`, a shared header). The helper
   `harness.py claim PATH...` is atomic: it creates `claims/<hash>.json` with
   `O_EXCL`; on success it returns ok; if the file exists it prints the owner
   and returns non-zero, and the engineer must send `ask` to the lead (who
   sequences the two tasks, or assigns the edit to the owner) rather than edit.
   Claims carry `task`, `agent`, `ts`, `expires`; they are released by
   `file_release`, on task `qa: pass`, or by the daemon when the run is
   declared dead after the WIP checkpoint (the relaunched run inherits the
   claims of its task id). `.claude/settings.local.json` also installs a
   `PreToolUse` hook on Edit/Write that calls `harness.py check PATH` and
   denies the edit when the path is claimed by another task: this makes the
   rule mechanical, not a prompt request. The integrator is the only role
   allowed to touch claimed files across tasks, after all are `qa: pass`.

**Heartbeat sharing.** `runs/<run-id>/beat` is touched by the same hooks (each
helper call updates it), so the daemon gets a free, accurate activity signal
in addition to the process/git signals of §3; the §3 checks remain the
authority because a hung agent also stops touching it. The daemon also writes
`board.json` each tick so any agent can `harness.py board` to see who has what.

**Retention and cleanup.** Messages of a feature move to
`.harness/archive/<feature>/` on `merged` and are deleted after 7 days by the
janitor; `claims/` entries are removed with the task; `runs/` logs follow the
7-day rule of the Consequences; `events.log` rotates at 5 MB. Startup
reconciliation removes `.tmp.*` files older than 1 hour and claims whose task
is not in the store.

**What the fake-claude tests assert** (`tools/fake-claude.py` can read its
inbox, send, claim and exit on a script): atomicity (a writer killed between
temp and rename leaves no partial message); ordering and idempotent redelivery
by `id`; hook-style `inbox --brief` prints nothing when empty and one line per
message otherwise; two fake engineers claiming the same path: exactly one wins,
the other receives non-zero and sends `ask`; `PreToolUse` `check` denies a
foreign edit; a `bug` from a fake qa returns the task to `bugfix` and the
`handoff` returns it to `qa`; an unanswered `ask` gets the default after the
timeout (clock injected); claims of a killed run are inherited by the
relaunch and freed on `merged`; `done` is refused with an unacked blocking
message; the tree has no `.tmp.*` after a crash-and-reconcile; the helper
fails loudly when `SONORA_HARNESS` is unset; no test touches the network or
the real `claude`.

### 5. Staffing policy

Pools and floors (agents truly working, not waiting on a build; a run is
counted "working" if `last_beat` is under 10 minutes old or it holds a build
slot with CPU activity):

| Pool | Roles | Floor | Ceiling |
|---|---|---|---|
| build | architect, engineer, integrator | 14 = 8 DAW + 4 OS + 2 plugin | 14 |
| qa | qa, lead | 2 | 3 |
| fix | ci-fixer, conflict-fixer, janitor | 1 | 2 (+1 burst) |

Total at floor is 17; the hard cap is 20, leaving 3 as burst. The daemon
enforces `total <= min(20, CLAUDE_CODE_MAX_CONCURRENT_SUBAGENTS)` itself and
launches nothing above it.

Scheduling each tick (30 s), in priority order, with a free slot count:

1. Fix pool first up to its need: every `ci_fix`/conflict task gets an agent
   (red PRs block the merge queue and cost the most CI time per hour waiting),
   bounded by the ceiling.
2. QA next: every task in `qa` or re-check gets an agent up to the ceiling.
   QA is short and keeps the build agents unblocked.
3. Build: fill each track's floor from `queued`. If a track has no queued
   task, launch the `lead` for that track (counts in the qa pool) and
   meanwhile promote the next-priority task from another track, so the total
   never idles. Floors are per track-family: DAW tracks have 8 independent
   seats, OS 4, plugins 2.
4. Burst: if fix and qa are empty and slots remain under 20, extra build seats
   up to 20.

"Never empty queue" is a daemon invariant checked each tick and shown on the
dashboard: for each pool the live count is >= floor, or the reason is shown
(`no eligible task`, `usage pause`, `CI budget`, `kill switch`). A floor that
cannot be met because the backlog has nothing eligible is an event for the
`lead`, not a silent gap.

CI budget (the real bottleneck): at most `max_open_prs` (default 10) PRs open
at once and at most 4 PRs in `ci_fix` or CI-running state per workflow class.
When `pr_open` count is at the limit, finished tasks wait in `integrating`
(branch pushed, no PR) rather than opening another; builders are not idled,
they take the next task, which bounds the queue by `max_ready_unpr`
(default 6), after which build seats drop to the QA/fix side. A tick that sees
the same PR red twice with the same failing job, after two fix attempts,
blocks it rather than spending a third agent. QA never starts a CI run; it
runs local narrow tests only, so qa concurrency is limited by local CPU: a
semaphore `build-slot` limit (`local_builds`, default 3) shared by all
roles, via `tools/build-slot.sh` once it lands (a lock-directory fallback
until then).

Kill switch: `touch planning/orchestrator/STOP` stops new launches within one
tick (drain); `STOP.NOW` also terminates the process trees; removing the file
resumes. `orchestrator.py stop|drain|status|pause --pool qa` are thin wrappers.
Dry-run: `--dry-run` runs the whole loop, writes events and `status.json`, and
replaces `launch()` with a log line (and `gh` calls with reads only); the
default for a first start. `--fake-claude PATH` swaps the binary for tests.

Budget guards beyond concurrency: Sonnet only; per-run `--max-turns`; a global
`max_runs_per_hour` (default 60); a daily cap on relaunches; a pause on the
429 signal. Opus is impossible by construction (see §1).

### 6. Dashboard integration

The daemon writes `planning/orchestrator/status.json` atomically (temp file and
rename) on every tick: pools with floor/live/working, every task with state,
role, age, last_beat, attempts and PR, `blocked` list, the invariant results,
and `privileged_pending`. `tools/dashboard.py` (when it lands) gets a section
that only reads this file, plus the tail of each run's stream log; it never
opens the database, so a dashboard crash cannot corrupt the state.
`tools/bg-status.sh` stays the process-level view and is called by the daemon.
`planning/AGENT-QUEUE.md` becomes the *input* seed: the daemon imports its
lines into `queued` tasks on start (and on `orchestrator.py import`), and
afterwards the database is the truth, with AGENT-QUEUE regenerated read-only
from it so existing habits keep working.

### 7. Implementation plan: three end-to-end PRs

Each is complete for its layer and tested with a fake `claude`
(`tools/fake-claude.py`: reads the prompt, sleeps N seconds, writes a canned
`result.json`, optionally exits non-zero, hangs, or writes invalid JSON). None
spends quota.

- **PR 1, core, heartbeat and messaging.** Also carries `tools/harness.py`
  (`send`, `inbox --brief`, `claim`, `check`, `board`, `release`), the
  `.harness/` layout, `msg.schema.json`, the `.gitignore` entry, the hook
  installer for worktrees and every §4b test. Original scope: `tools/orchestrator.py` with the state store,
  `advance()` state machine, launch/heartbeat/relaunch, WIP checkpoint,
  retries and `blocked` note, kill switch, dry-run, `status.json`, number
  reservations, `result.schema.json`, `tools/fake-claude.py`,
  `tools/test-orchestrator.sh` (wired into CI as a plain Python job). Roles in
  this PR: engineer and qa only. Tests: happy path building -> qa -> done;
  QA fail returns to bugfix and loops out at 3; dead pid relaunches; hung
  process killed at the threshold; invalid result treated as dead; Sonnet flag
  present on every argv; 429 pause; STOP and STOP.NOW; two writers on the DB.
- **PR 2, PR lifecycle and pools.** Staffing scheduler with floors, ceilings,
  cap 20 and the CI budget; pr_open/ci_fix/conflict handling against a fake
  `gh` (recorded JSON); `pre-pr.sh` and `enqueue-pr.sh` calls; ci-fixer,
  conflict-fixer, integrator and janitor roles and their agent files;
  file-overlap exclusion. Tests: with a fake backlog of 40 tasks the pools
  hold 17 live and never exceed 20; a red PR gets a ci-fixer within one tick;
  CI-budget back-pressure; no overlap launches.
- **PR 3, architect, lead, feature flow, import and dashboard.** Adds the
  `features` table, plan and assignments schemas with validators, and the
  feature-grouped dashboard view of §4a. PRs 1 and 2 already wrap every item
  in a one-task feature so the schema does not change later. Original scope: `lead` and `architect`
  roles and agent files; the AGENT-QUEUE import/regenerate; `status.json`
  section in `tools/dashboard.py`; systemd/Task Scheduler/SessionStart
  start-up snippet; first live run in dry-run, then a supervised live run with
  1 task per pool before the floors are raised (a config change, not a PR).
  Tests: lead output creates tasks; the invariant report; adopting runs after a
  daemon restart mid-flight.

Every step keeps `--dry-run` as the default until the owner or the coordinator
flips `live = true` in `orchestrator.toml`.

## Rationale

The expensive failures this month were coordination, not coding: stalled
agents noticed late, floors silently below target, red PRs waiting for a human,
numbers collided. All are bookkeeping that a small deterministic program does
better than a model, and a program is testable for free. Keeping the judgement
(what to build, is this diff right, why is CI red) in Sonnet roles preserves
the "agents do the work, reviewers review" rule: no role reviews its own diff,
the daemon never reads code.

SQLite over JSON because the transition needs to be atomic across tasks, runs
and reservations, and because the daemon and the CLI both write.

Observing activity from outside (process, git, files) over agent-written
heartbeats because the failure we care about is an agent that has stopped
doing anything, including remembering to report it.

## Consequences

- One more long-lived process to keep alive; mitigated by state recovery and a
  start-up hook. If the daemon dies the agents it started are orphans until it
  returns (they finish and write `result.json`, which is adopted).
- Headless agents cannot ask questions: a brief that needs a decision gets a
  recommended default recorded in the ADR, per existing practice, else the
  agent returns `blocked`.
- Permissions for headless runs must be complete; a permission denial shows as
  a stalled or failed run and, after retries, a `blocked` task with the
  stream tail. Setting up `.claude/settings.json` allowlists is part of PR 1.
- `status.json` and the stream logs can grow; the janitor prunes logs of
  merged tasks after 7 days.
- The daemon may launch up to 20 processes on a laptop; `local_builds` limits
  builds, not agents, so memory pressure is possible. Default launch rate is
  one per tick (30 s) to smooth start-up.

## Alternatives rejected

- **Long-lived Claude session as orchestrator.** Compaction degrades exact
  counting, it cannot be tested without quota, it is its own single point of
  failure with no supervisor, and a script-less loop has no way to be
  restarted when the session ends. Kept only as the owner-facing session.
- **Agents touch a heartbeat file / report progress.** Depends on the model
  remembering; a stuck agent is exactly the one that forgets. Kept as an
  optional extra signal only.
- **JSON file with lock.** Reimplements transactions badly; rejected for SQLite.
- **Resume the dead session (`--resume`) instead of a fresh context.** Cheaper
  in tokens on paper, but a session that died on a corrupt state or a loop
  resumes into the same loop. Fresh context from the branch is deterministic.
  Revisit if relaunch cost shows in the quota.
- **GitHub Actions or a hosted scheduler as the orchestrator.** Spends the
  runner budget that is already the bottleneck and cannot reach the owner's
  worktrees.
- **Let QA and the fixer be the same agent as the engineer.** Defeats the
  independent check and the rule that the author does not review.

## Unverified (hand to the researcher)

1. That `claude -p --agent <name>` honours the agent file's `tools`, `model`
   and `maxTurns` headlessly on this install, and which flag names the 429
   signal exposes in `stream-json`. Measurement: run one trivial agent with a
   deliberately long prompt and record argv, exit code and the final event.
2. That 17-20 concurrent headless `claude` processes plus builds fit the
   owner's laptop (RAM and the API rate limit). Measurement: launch 20 fake
   sleeping agents, then 20 real one-turn agents, and watch memory and 429s.
3. That `last_beat` from git/file activity does not false-positive during long
   silent builds. Measurement: replay the stream logs and `build-slot` timings
   of three real sanitizer/ISO runs against the thresholds in §3.
4. Real CI minutes per PR and the ceiling of concurrently queued workflows on
   the repository, which set `max_open_prs`. Measurement: `gh run list` over
   the last 7 days.
6. That headless `claude -p` runs execute `PostToolUse` and `PreToolUse`
   hooks from the worktree's `.claude/settings.local.json` and that a hook's
   stdout reaches the model. Measurement: one fake task with a hook that
   prints a marker; check the stream. If it does not, fall back to the
   prompt rule "run `harness.py inbox` after every build or commit" and keep
   `check` as a git pre-commit hook.
5. Whether the account's weekly Sonnet quota sustains 17+ continuous agents;
   the `max_runs_per_hour` default is a guess.

## Open questions for the owner

None blocking. Recommended defaults are taken: daemon over session, dry-run
first, floors 14/2/1 within the cap of 20. The one real owner action is to
authorise headless permission allowlists in `.claude/settings.json` when PR 1
lands.
