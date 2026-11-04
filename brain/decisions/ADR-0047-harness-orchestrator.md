# ADR-0047 — Harness orchestrator: a Python daemon that runs headless Sonnet roles

- Status: Proposed (architect; recommended defaults, owner not available)
- Date: 2026-11-04
- Scope: development harness only (`tools/`, `.claude/agents/`, `planning/`).
  No product code, no contract, no protocol version.

## Summary

*Navigation only; the sections below are the decision.*

- §1: the control loop is a Python daemon, `tools/orchestrator.py`, not a
  long-lived Claude session. Roles run as headless `claude -p --model sonnet`
  processes, one worktree each. The daemon decides nothing that needs
  judgement; roles decide, the daemon enforces pools, caps, file claims,
  number reservations and retries.
- §2: state is one SQLite file, `planning/orchestrator/state.db` (git-ignored),
  with a state machine from `queued` to `merged`.
- §3: heartbeat = process liveness plus git/worktree/result-file activity (and
  the live broker connection); relaunch from the WIP branch, 3 tries, then a
  "waits on the owner" note.
- §4: roster mapped onto the existing `.claude/agents` files plus two new
  ones, `lead.md` and `tester.md`; the ci-fixer is the track's engineer with a
  fixer brief. Each role ends with a structured `result.json`.
- §4a: one feature = one channel = one branch = one PR. Architect and lead
  are event-driven short runs; the orchestrator reserves ADR, protocol and
  format numbers; a tester bounces bugs to the named engineer (two bounces go
  to the architect, a third blocks).
- §4b: every agent is a client of a loopback TCP broker in the daemon, with a
  channel per feature; the orchestrator sees every message and enforces file
  claims. Client is a per-agent MCP server (CLI fallback); inbound messages
  show at tool boundaries via a hook. `.harness/` is the durable log and
  mailbox.
- §5: staffing in a 20 cap with priority fix > tester > integrator > engineer
  > lead/architect; CI budget, kill switch, dry-run.
- §6: the daemon writes `status.json`; the dashboard reads it.
- §7: PR 0 spike, then three PRs tested with a fake `claude`, then shadow-mode
  rollout beside the in-session coordinator.

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
stays in the `lead`, `architect` and `tester` roles, so the daemon stays small
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
- `qa`: a fresh tester reviews the diff and runs the tests the PR claims.
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
- `ci_fix`: a fixer (the track engineer, red check) or the integrator (conflict) is launched.
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

Broker signal (§4b): an agent registered with the MCP client holds a live
socket to the daemon; traffic or a 15 s `ping` is a beat, and a close marks
the run `suspect` at once. It is an extra signal; the process/git checks above
stay the authority because a hung but connected agent still pings.

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

| Role | Agent file | Lifetime |
|---|---|---|
| architect | `architect.md` (existing) | short run per event (`plan_request`, design `ask`, bug escalated twice) |
| lead | `lead.md` (NEW) | short run per event (`plan_ready`, `task_done`, `qa_fail`, `agent_dead`, `pool_under_floor`) |
| engineer | `rt-engineer`, `ui-engineer` (DAW); `dsp-engineer`, `plugin-engineer` (plugins); `distro-engineer`, `driver-engineer` (OS) | one run per task, on its sub-branch |
| tester | `tester.md` (NEW): `code-reviewer`'s review, plus the task's `accept` commands, plus `rt-safety-auditor` checks when the diff touches the RT call graph | one fresh run per task attempt, never the author |
| integrator | `integrator.md` (existing) | one run per feature integration; also resolves conflicts |
| ci-fixer | the track's engineer agent with a fixer brief (no scope change, no test deletion or weakening) | one run per red PR |
| researcher | `researcher.md` | on an `unverified` item that blocks |
| janitor | `janitor.md` | every 3 merges |

`code-reviewer` and `rt-safety-auditor` remain as sub-tools of the tester,
not scheduled roles. Per-role reads and returns are in each agent file; this
ADR fixes who may do what:

- The architect's plan is the only source of tasks; the lead assigns and
  never creates or codes. The tester alone moves a task from `qa` to
  `qa: pass` and cannot edit the code. The daemon alone opens PRs and
  enqueues them (`enqueue-pr.sh` is idempotent). "No two agents on the same
  files" is enforced by the daemon from the plan's globs and re-checked by
  the tester against the real diff.
- A tester passes only if there is no blocking finding and every claim is
  verified or listed `unverified`.

Not delegable to a headless subagent: privileged docker / loop-device tests.
A task whose brief says `needs_privileged` ends `building` -> `qa` with that
check listed `unverified`, and the daemon writes a `privileged_pending` line
to `status.json`; the in-session coordinator (or the owner) runs it per the
standing "run privileged tests myself" rule and posts the result to the PR.
The task does not block on it unless the PR is partial by design.

### 4a. Feature flow: architect, lead, engineers and testers, integrator

Owner refinement (2026-11-04). The unit of delivery is a **feature**, not a
task; every feature has a channel (§4b).

**Event-driven architect and lead.** Neither is long-lived. The daemon starts
a short run when an event arrives; the run replays the channel log (its only
memory), acts, sends its messages and exits. No context to compact, no seat
held while idle. Events:

| Event | Wakes | The run does |
|---|---|---|
| `plan_request` (feature opened) | architect | writes the plan |
| design `ask` from an engineer or lead | architect | answers |
| a bug bounced twice (step 5) | architect | rules: spec fault or code fault |
| `plan_ready` | lead | writes assignments, sends `assign` |
| `task_done`, `qa_fail` | lead | reorders or reassigns if needed |
| `agent_dead` (3 tries spent) | lead | reassigns or escalates to `owner` |
| `pool_under_floor` | lead | proposes the next feature for the track |

**Flow.**

    orchestrator opens feature + channel, reserves numbers
      --> architect (plan.json) --> lead (assignments)
      --> engineers (one sub-branch each) + a tester per finished task
      --> integrator (merge sub-branches, pre-pr, ONE PR, enqueue)
      --> fixer on red, integrator on conflict --> merged

1. **Orchestrator opens.** It picks the next V1 feature (AGENT-QUEUE seed),
   **reserves the ADR, protocol-version and format numbers itself** and
   writes them into the plan request. Agents never choose a number; this ends
   the collisions we had (format numbers 19/20, protocol 43-50). It opens the
   channel and sends `plan_request` to the architect.
2. **Architect plans.** It writes `.harness/plan/<feature>.plan.json` (a copy
   is committed on the feature branch under `planning/orchestrator/plans/`;
   the daemon ingests it): the ADR (if any), then tasks ordered contract
   first, then engine, UI, tests, docs. Every task lists `files` (globs) and
   `accept` commands; the daemon rejects a plan whose globs overlap any active
   feature's. It writes no product code.
3. **Lead distributes.** It writes `assignments.json` and sends `assign`. It
   never writes product code and creates no tasks (the plan does).
4. **Engineers** claim files, code the whole slice, build at most three times
   through `build-slot`, commit, and send `done` with commands and outputs.
   They may `ask` the architect or lead; the orchestrator wakes the addressee,
   default answer after 20 minutes (§4b).
5. **Tester**, one fresh run per task attempt and never the author, reviews
   the diff, runs the task's `accept` commands and sends `qa_report` `pass`,
   or `bug` addressed to the engineer by name. The orchestrator relaunches
   that name on the same sub-branch with mailbox replay. **Bounce limits:**
   two bounces on one task go to the architect (spec or code fault?); a third
   marks the task `blocked` and puts it on the `owner` list. A feature never
   waits on a blocked task silently: the lead is woken.
6. **Integrator**, when every task is `qa: pass`, merges the sub-branches in
   `merge_order`, runs the full affected tests once and `tools/pre-pr.sh`;
   the daemon opens ONE PR for the feature and runs `tools/enqueue-pr.sh`. A
   single-task feature skips the merge but not the rest.
7. **Red check: the ci-fixer is the track's own engineer agent with a fixer
   brief**, in the same tick. Fixes need domain knowledge (an RT test, MSVC
   CMake, the DSP quality bar, a keymap test), so no generic fixer role
   exists. A conflict goes to the integrator. The same job red after two
   fixes marks the feature `blocked`. A root cause shared by many PRs (a
   main-wide check) gets one fixer on `main`; the other PRs just merge `main`
   after it.

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

| task | engineer agent | tester | sub-branch | order | start-from |
|---|---|---|---|---|---|
| t1 | rt-engineer | tester | `feat/<f>/t1` | 1 | feature branch |
| t2 | ui-engineer | tester | `feat/<f>/t2` | 2 | `feat/<f>/t1` |

The lead sends `assign` to each engineer and tester name. The daemon enforces
what the lead decides: it launches a named run only when its `depends_on`
tasks are `qa: pass`, the files are free and a seat is open; the lead can
reorder or reassign, never exceed floors or overlap. A lead that returns an
invalid table is relaunched, not obeyed.

Store additions: table `features(id, plan_path, branch, track, state, pr,
adr_no, protocol_no, partial, created, updated)`; `tasks.feature_id`
references it, and a task's `branch` is its sub-branch. Feature state is
derived: `planning` -> `distributing` -> `building` -> `integrating` ->
`pr_open` -> `ci_fix` -> `merged` (or `blocked`). Exactly one row of
`features` owns a `pr`, and a unique index on `pr` forbids two features
sharing one; a task never has a PR of its own. The task-level state machine
in §2 applies per task up to `qa: pass`; `integrating`, `pr_open`, `ci_fix`
and `merged` are feature states. The `designing` state of §2 is the feature's
`planning` state.

Dashboard view (`status.json` `features[]`, rendered by `tools/dashboard.py`):
a table grouped by feature, one line per PR: feature, track, PR number and
link, feature state, tasks done/total with a per-task strip (state letter and
agent), CI status, attempts, age, blocked reason. Below it, open PRs by
feature against `max_open_prs`, and the "waits on the owner" list.

### 4b. Inter-agent communication: a broker in the daemon

Owner decision (2026-11-04), replacing the first draft of this section (file
inboxes polled by hooks). Purpose: no code conflicts, everyone in tune. The
orchestrator is the single access point: it opens a socket server, every agent
connects to it and registers under a name, messages are addressed to a name,
and the orchestrator routes them, enforces file claims and reads a live
connection as a heartbeat. The files in `.harness/` stay, but as the durable
log and per-recipient mailbox, not as the transport.

**Transport: TCP on 127.0.0.1, not a Unix socket.** The dev machine is Windows
(PowerShell, Git Bash, Docker) and the daemon, the hooks and the MCP servers
may run under native Windows Python or WSL. `AF_UNIX` exists in Windows 10+
but Python's `asyncio` Unix-server support is missing on Windows, Git Bash and
native Python disagree on path forms, and a socket file cannot cross the
WSL/Windows boundary, whereas `127.0.0.1:<port>` works from all of them and
from a container with host networking. Cost: a port can collide and anything
local can connect, so see Auth below. Protocol: newline-delimited JSON frames
over TCP (one request, one reply, plus server-pushed `deliver` frames to
connections that asked for them); Python `asyncio` streams, no dependency.

**The constraint that shapes the client.** An LLM agent cannot hold a
connection or listen: it acts only when it calls a tool, and nothing can be
pushed into the model between its steps. So the connection belongs to a tool
process, not to the model, and the agent sees inbound messages only at tool
boundaries.

1. **Recommended client: a small per-agent MCP server**
   (`tools/harness_mcp.py`, stdio, started by `claude` from the run's
   `--mcp-config`). It connects to the broker at start-up, registers the name
   from `SONORA_AGENT_NAME`, keeps the socket for the life of the run (this
   live connection is the heartbeat; see §3) and exposes five tools:
   `whoami()`, `send(to, type, body, reply_to?)`, `recv(max?, wait_s?)`,
   `claim(paths[])` and `release(paths[])`. If the broker is unreachable at
   start-up the MCP server exits non-zero, `claude` reports the tool server as
   failed and the run's first `whoami` fails: a loud start-up failure, not a
   silent degrade.
2. **Fallback client: `tools/harness.py`** with the same verbs
   (`whoami|send|recv|claim|release|board|check`), one short TCP connection
   per call, name from `SONORA_AGENT_NAME`, called through Bash. It is also
   the tool for humans and for the hooks. Used when the MCP server cannot be
   loaded in headless mode (Unverified 7). No heartbeat from a live socket in
   this mode; the §3 process/git signals carry it, which is why they remain the
   authority.
3. **Delivery at tool boundaries.** The daemon installs, in each worktree's
   `.claude/settings.local.json`, a `PostToolUse` hook (and
   `UserPromptSubmit`) running `tools/harness.py recv --brief --hook`. It asks
   the broker for the agent's unread messages and prints nothing when there are
   none (zero tokens), otherwise one line per message ("17 bug from tester-2: ...")
   which reaches the model after its next tool call. Messages are marked
   delivered by the broker when the hook (or `recv`) returns them. A long
   single Bash build delays delivery to the end of that call; acceptable.
   There is no push into the model and the design does not pretend otherwise.

**Channels.** The broker opens one channel per feature (`feat/<id>`) when
the orchestrator opens the feature. Members are the agents working it:
architect, lead, engineers, testers, integrator; the orchestrator is a member
of every channel, sees every message and is the only access point (agents have
no route to each other outside the broker). A message goes to a name
(`eng-2`) or to the channel (every member). The channel log is the feature's
history; it is what an event-driven run replays on start (§4a), so no role
needs a long-lived context.

**Names.** An agent's name is stable and role-shaped, not per run: `lead`,
`architect`, `eng-1..N`, `tester-1..N`, `integrator`, `fixer-N`, `janitor`,
plus `daemon` (the broker's own address) and `owner` (the "waits on the owner"
mailbox, read by the dashboard). The daemon assigns the name at launch
(`SONORA_AGENT_NAME`) and records name -> task -> run in the store, so a
relaunched run for the same task gets the same name and therefore the same
mailbox. Register is refused if the name is already connected (a duplicate is
a bug, reported to `lead`), if the store does not expect it, or if the agent
is not a member of the channel it names. Addressing: `to` is a name,
`feat/<id>` for the channel, or `role:<r>` for the broker to expand to live
names.

**Broker duties.**

- Route: stamp `id` (ulid) and a per-recipient `seq`, append to the durable log
  and the recipient's mailbox, then push to the live connection (if any).
- Claims: the broker holds the claim table (SQLite, §2) and is the only writer.
  `claim` is all-or-nothing and atomic in the broker's single event loop: a
  second claim on a path (or on a path under a claimed directory glob) is
  refused with the owner's name, and the client must `send` an `ask` to `lead`
  rather than edit. Claims are released by `release`, on task `qa: pass`, or
  by the daemon when a run is declared dead after the WIP checkpoint; the
  relaunched run (same name) inherits them.
- Heartbeat: a connection with traffic or a protocol `ping` (every 15 s, sent
  by the MCP server, not by the model) is alive; a closed socket marks the run
  `suspect` at once and the §3 checks decide dead. Fallback-mode agents rely
  on §3.
- Timeouts: an unanswered `ask` after 20 min gets the recommended default
  appended by the broker (per owner rule) and is answered with it.

**Message schema** (`msg.schema.json`, version 1; also the wire frame body):

    { "v": 1, "id": "<ulid>", "seq": 17, "ts": "...", "from": "eng-2",
      "to": "tester-2", "feature": "...", "task": "...",
      "type": "register|assign|ask|answer|blocked|claim|claim_denied|release|"
              "handoff|qa_report|bug|done|ping|ack|"
              "plan_request|plan_ready|task_done|qa_fail|agent_dead|pool_under_floor",
      "reply_to": "<id>|null", "needs_ack": true,
      "body": { ... type specific ... } }

`register` (client to broker: name, token, last_acked_seq; the reply carries
every unacked message with `seq` above it, which is how a relaunched agent
gets what it missed); `assign` (lead to eng or tester: task, branch,
start-from, files, accept); `ask` / `answer` (any to lead or architect and
back); the daemon's wake events `plan_request`, `plan_ready`, `task_done`,
`qa_fail`, `agent_dead`, `pool_under_floor` (§4a);
`blocked` (names the blocker; lead reassigns or escalates); `claim` /
`claim_denied` / `release` (broker-enforced, above; the request and reply
share `reply_to`); `handoff` (eng to qa: sub-branch, commit, commands run and
printed, unverified); `qa_report` (qa to lead and daemon); `bug` (qa to eng:
one blocking finding with where and text; the engineer returns it fixed in a
`handoff`); `done` (final, mirrors `result.json`); `ping`/`ack` (transport).
Unknown types are ignored by agents and logged by the broker.

**Durable log and mailbox: `.harness/`** (git-ignored, shared with worktrees
through `SONORA_HARNESS=<abs path>`, plus `SONORA_AGENT_NAME`, `SONORA_TASK`,
`SONORA_FEATURE`, `SONORA_BROKER=127.0.0.1:<port>` and `SONORA_TOKEN`; none is
resolved from the cwd, and a client with any of them unset exits non-zero).

    .harness/
      plan/<feature>.plan.json        architect output, read-only after ingest
      board.json                      task states and claims, daemon only
      broker.json                     port, pid, start time (no token)
      mail/<name>/<seq>-<type>-<id>.json   every message addressed to a name
      mail/<name>/.acked              highest acked seq (atomic rewrite)
      log/messages.jsonl              append-only, every routed message
      runs/<run-id>/{stream.log,result.json}
      events.log                      append-only JSON lines
      STOP, STOP.NOW                  kill switch (see §5)

The broker writes the mailbox file (temp file in the same directory, fsync,
`os.rename`; readers ignore `.tmp.*`) before it pushes or acknowledges the
send, so a message the sender saw accepted is on disk. Only the broker writes
`mail/` and `log/`; clients never touch them. Consequences: a dead agent
relaunched under its name receives everything above its `.acked` on
`register`; a broker or daemon restart loses nothing, because on start it
rebuilds the claim table and the per-name `seq` counters from the store and
the mailbox files and live clients reconnect (the MCP server retries with
back-off for 60 s, then exits and the run is treated as dead). Delivery is
at-least-once; handlers are idempotent on `id`. An unacked `needs_ack`
message is redelivered after 10 min and, after two, reported to `lead`. The
helper refuses a final `done` while unacked blocking `bug` or `ask` messages
exist. The daemon verifies at start-up that `.harness/` and the worktrees
share a volume (rename atomicity) and refuses to start otherwise.

**Auth and exposure.** The broker binds `127.0.0.1` only, never `0.0.0.0`
(fail at start-up if the bind address is not loopback). A per-run random token
(32 bytes) is generated at broker start, stored in memory and in the launch
environment of each child only (never in `broker.json`, the log or a
committed file); `register` without the token is closed. Names are bound to the
token's launch record, so a process cannot claim another agent's name. This
defends against other local users and stray processes, not against a hostile
process running as the same user, which can read the environment anyway; that
is outside the threat model of a single-user development laptop.

**Edit-time claim enforcement.** The daemon writes the task's globs as
claims at launch, and rejects overlapping globs at plan time (§4a), as
before. At edit time a `PreToolUse` hook on Edit/Write/MultiEdit runs
`tools/harness.py check PATH`, which asks the broker whether PATH is claimed
by another name and exits with the hook's deny code if so; an unclaimed path
outside the task's globs is denied with the message "claim it first", so the
engineer calls `claim`, and on `claim_denied` sends `ask` to `lead`. The rule is
mechanical, not a prompt request. If the broker is unreachable, `check`
denies (fail closed) with a message naming the broker; a git pre-commit hook
runs the same check as a second line of defence. The integrator is the only
role allowed to touch claimed files across tasks, after all are `qa: pass`.

**Retention.** Mailbox and log of a feature move to
`.harness/archive/<feature>/` on `merged` and the janitor deletes them after 7
days; `events.log` rotates at 5 MB; start-up reconciliation removes `.tmp.*`
older than 1 hour and claims whose task is not in the store.

**What the fake-claude tests assert.** The test double is a fake agent client
(`tools/fake-claude.py` plus `tools/fake_agent_client.py`) that speaks the real
protocol over the real loopback socket to a real broker started in the test on
port 0 (the OS picks the port, so tests never collide); it can register, send,
receive, claim, and exit on a script. Asserts: register with a bad token or a
duplicate name is refused; addressed delivery (a message to `tester-2` reaches only
`tester-2`); per-recipient ordering by `seq` and idempotent redelivery by `id`;
two fake engineers claiming one path, exactly one wins and the other gets
`claim_denied` and sends `ask`; `check` denies a foreign edit and denies when
the broker is down; a killed client's socket close marks the run `suspect`
and its claims are inherited by the relaunch and freed on `merged`; a
relaunched client registering with `last_acked_seq` receives exactly the
missed messages; broker restart mid-run loses no accepted message and
rebuilds claims; a writer killed between temp and rename leaves no partial
mailbox file; a `bug` from a fake qa returns the task to `bugfix` and the
`handoff` returns it to `qa`; an unanswered `ask` gets the default after the
timeout (clock injected); `recv --brief --hook` prints nothing when empty and
one line per message otherwise; `done` is refused with an unacked blocking
message; the broker refuses a non-loopback bind; a client with
`SONORA_BROKER` unset fails loudly. The MCP server gets one protocol test
(initialize, list tools, call `whoami`) against the same broker. No test uses
the real `claude` or any non-loopback network.

### 5. Staffing policy

Pools and floors count agents truly working, not waiting on a build; a run is
"working" if `last_beat` is under 10 minutes old or it holds a build slot
with CPU activity. The hard cap is 20 (`CLAUDE_CODE_MAX_CONCURRENT_SUBAGENTS`),
enforced by the daemon, which launches nothing above it.

| Pool | Roles | Steady state |
|---|---|---|
| build | engineers (DAW, plugin, OS) | 12-14 = 8 DAW + 4 OS + 2 plugin floors |
| qa | testers | 3-4 |
| fix | ci-fixer (track engineer, fixer brief), conflict, janitor | 1-2 |
| merge | integrator | 1 |
| plan | architect, lead | none standing: event-driven short runs borrow burst seats |

Priority each tick (30 s), highest first, with a free-seat count:

1. **Fix**: every `ci_fix`/conflict task gets an agent. Red PRs block the
   merge queue and cost the most CI time per hour waiting.
2. **Tester**: every task in `qa` or re-check. Short runs that keep engineers
   unblocked.
3. **Integrator**: a feature whose tasks are all `qa: pass`.
4. **Engineer**: fill each track-family's floor from ready tasks (files free,
   dependencies `qa: pass`). Families have independent seats.
5. **Lead / architect**: event runs (§4a). They are short, so they preempt no
   running agent; they wait one tick for a seat, which is why steady state
   leaves the top seats open rather than full.
6. **Burst**: if 1-3 are empty and seats remain under 20, extra engineers.

If a track has no eligible task, the daemon wakes the lead with
`pool_under_floor` (§4a) and meanwhile promotes the next-priority task from
another track, so the total never idles. "Never empty queue" is a daemon
invariant shown on the dashboard: each pool's live count is at or above its
floor, or the reason is shown (`no eligible task`, `usage pause`, `CI budget`,
`kill switch`). A floor that cannot be met because nothing is eligible is an
event for the lead, not a silent gap.

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

### 7. Implementation plan: PR 0 spike, then three end-to-end PRs, then shadow mode

Each PR is complete for its layer and tested with a fake `claude`
(`tools/fake-claude.py`: reads the prompt, sleeps N seconds, writes a canned
`result.json`, optionally exits non-zero, hangs, or writes invalid JSON). None
spends quota.

- **PR 0, spike (no product code, a measurement note in `brain/research/`).**
  Prove that `claude -p --model sonnet --agent X` loads a project MCP server
  and runs `PreToolUse`/`PostToolUse` hooks headlessly; measure 15 concurrent
  `claude` processes on this laptop (RAM, CPU, start-up time) and the token
  cost of the hook (zero when the mailbox is empty). The result decides MCP
  versus CLI client (Unverified 7) and the launch cadence of §5. Nothing else
  starts until it lands.
- **PR 1, core, heartbeat and broker.** `tools/orchestrator.py` with the
  state store, `advance()`, launch/heartbeat/relaunch, WIP checkpoint, retries
  and `blocked` note, kill switch, dry-run, `status.json`, `result.schema.json`,
  `tools/fake-claude.py`, `tools/test-orchestrator.sh` (plain Python CI job).
  The broker (`tools/harness_broker.py`, inside the daemon) with per-feature
  channels, the clients `tools/harness_mcp.py` and `tools/harness.py`, the
  `.harness/` layout, `msg.schema.json`, per-run token, hook installer,
  `--mcp-config`, the fake agent client and every §4b test. Roles: engineer and
  tester. Tests: happy path building -> qa -> done; a tester `bug` bounces and
  the bounce limits of §4a hold; dead pid relaunches; hung process killed;
  invalid result treated as dead; Sonnet flag on every argv; 429 pause; STOP
  and STOP.NOW; two writers on the DB.
- **PR 2, PR lifecycle and pools.** Staffing scheduler with floors, ceilings,
  priorities, cap 20 and the CI budget; `pr_open`/`ci_fix`/conflict handling
  against a fake `gh`; `pre-pr.sh` and `enqueue-pr.sh` calls; ci-fixer (track
  engineer, fixer brief), integrator/conflict and janitor roles; number
  reservation (ADR, protocol, format); file-overlap exclusion. Tests: with a
  fake backlog of 40 tasks the pools hold their floors and never exceed 20; a
  red PR gets a fixer within one tick; same job red after two fixes blocks;
  CI-budget back-pressure; no overlap launches; two features never get the
  same reserved number.
- **PR 3, architect, lead, feature flow, dashboard.** The new `lead.md` and
  `tester.md` (drafted with this ADR), the `features` table, plan and
  assignments validators, event-driven architect/lead runs with channel-log
  replay, the AGENT-QUEUE import/regenerate, the feature-grouped section of
  `tools/dashboard.py`, a start-up snippet. PRs 1 and 2 wrap every item in a
  one-task feature so the schema does not change. Tests: lead output creates
  assignments; the invariant report; adopting runs after a daemon restart.
- **Shadow-mode rollout (configuration, not a PR).** The daemon runs
  `--dry-run` beside the in-session coordinator and must log the same launch
  decisions for a day (differences are read and either fixed or accepted by
  the coordinator). Then it takes over, one pool at a time: the fix pool, then
  testers, then builders. Each step is a `live_pools` list in
  `orchestrator.toml`.

`--dry-run` stays the default until the coordinator flips a pool live.

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
- **File-only messaging (inboxes polled by hooks, the first draft of §4b).**
  Needs no port and survives anything, but no component sees all traffic:
  claims are `O_EXCL` files with races across volumes, there is no live
  presence so a dead agent is only found by §3, routing and `seq` allocation
  sit in every client, and the owner asked for one access point. Kept as the
  durable layer under the broker, not as the transport.
- **Unix domain socket.** Filesystem permissions for free, but the dev host is
  Windows: asyncio has no Unix server there, Git Bash and native Python
  disagree on paths, and the socket cannot cross WSL/Windows. TCP on loopback
  with a token works everywhere. Revisit if the harness ever runs only on Linux.
- **Agent holds the connection through a Bash background process.** Violates
  the no-background rule and dies with the shell; the MCP server is the
  supported long-lived tool process.
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
   prompt rule "call `harness.py recv` after every build or commit" and keep
   `check` as a git pre-commit hook.
7. That MCP servers named by `--mcp-config` load in headless `-p` mode, stay
   up for the whole run, expose tools to a `--agent` whose `tools` list names
   them (and whether `--allowedTools mcp__harness__*` is needed), and exit
   with the run. Measurement: a one-turn agent that calls `whoami`; record
   argv, the stream's tool list and the server's pid before and after. If it
   fails, `tools/harness.py` over Bash is the client and the live-connection
   heartbeat is lost (§3 carries it).
8. Port allocation: that binding port 0 and publishing it through
   `broker.json` and `SONORA_BROKER` is reliable on the owner's Windows host
   (firewall prompt, excluded port ranges, Docker or WSL port forwarding for
   container agents). Measurement: start the broker on port 0 from native
   Python and connect from Git Bash, WSL and a `docker run --network host`
   container; record each result. Fall back to a configured fixed port.
9. Auth: that a token in the child environment is not exposed beyond the
   same user (process listings on Windows, stream logs echoing env).
   Measurement: grep `stream.log` and the environment dump of a fake run for
   the token; the logger must redact it.
10. Delivery latency and cost: that a `PostToolUse` hook which round-trips to
    the broker adds under 100 ms per tool call and no tokens when empty.
    Measurement: time 200 tool calls with and without the hook.
5. Whether the account's weekly Sonnet quota sustains 17+ continuous agents;
   the `max_runs_per_hour` default is a guess.

## Open questions for the owner

None blocking. Recommended defaults are taken: daemon over session, dry-run
first, floors 14/2/1 within the cap of 20. The one real owner action is to
authorise headless permission allowlists in `.claude/settings.json` when PR 1
lands.
