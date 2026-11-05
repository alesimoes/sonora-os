# SonoraOS

A Debian-based Linux distribution that boots straight into a multitrack
recording engine written from scratch. Boots and runs from a USB stick.

## What this project is not

We do not decompile, disassemble or copy code from Pro Tools, Logic Pro or any
other proprietary DAW. What we *do* reproduce is the **workflow**: key bindings,
edit modes, comping, mixer topology. Interoperability work is limited to
published formats (BWF, AAF, MIDI) and to reverse-engineering results already
published under a free licence. Any task that asks for more than that is out of
scope — say so instead of doing it.

## Layout

| Path | What lives there |
|---|---|
| `engine/` | The recording engine. C++17, CMake. Real-time audio core. |
| `iso/` | `live-build` configuration producing the bootable USB image. |
| `audio-stack/` | PipeWire, RT limits and kernel tuning shipped into the image. |
| `keymaps/` | Swappable command maps (`protools.json`, `logic.json`). |
| `brain/` | Durable knowledge: decisions, research, format specs, postmortems. |
| `planning/` | Roadmap, backlog and the current state of the work. |
| `.claude/` | Agent definitions and skills that drive the build. |
| `tools/` | Build and test scripts. |

## Rules for the real-time path

`Session::process()` and everything it calls run on the audio thread. In that
call graph, never:

- allocate or free (no `new`, no `std::vector` growth, no `std::string`)
- lock a mutex or wait on a condition variable
- touch the filesystem, log, or call anything that might
- throw

Buffers are sized when the session is created. Hand data to the disk thread
through a `RingBuffer`. A short write means an overrun — count it, never block.

## Build

Both builds need Linux. From Windows, use the container wrappers:

    tools/build-engine.sh      # cmake + ctest
    tools/build-iso.sh         # produces iso/sonora.iso

## Working in sessions

Context is the main cost: short sessions, one agent per task and review round.
The SessionStart hook (`tools/session-start.sh`) prints a starting summary
from `planning/handoffs/`; run the branch handoff's Verify command before
relying on it. Starting and stopping: `handoff` skill. Testing and keeping
output out of the context: `test-loop` skill.
Never read `planning/BACKLOG.md`, `planning/progress.json`, `brain/specs/*.md`
or an ADR whole: read the ADR's Summary, grep the heading, Read with
offset/limit. Progress goes through `tools/progress.sh get|list|set`.

The coordinator runs up to 8 agents at once, one worktree each, no two on the
same files: the 4 DAW tracks, 2 OS packaging and 2 native plugins (owner,
2026-11-01). Keep every slot full; do not wait for CI before launching the
next items. After 3–4 merges, or near 150k of context, it writes the handoff
and carries on.

The agent queue is `planning/AGENT-QUEUE.md`: when an agent reports, the
coordinator closes it (`tools/bg-status.sh --kill`) and launches the next line
in the same turn, keeping up to 10 working.
The coordinator runs unattended: no human reads the terminal, answers a
question or runs a slash command such as /clear. Never end a turn waiting
on the owner. A decision with a recommended default is taken and recorded;
a real blocker goes in the handoff under "waits on the owner" and the work
moves to the next item. When agents finish and none are running, launch
the next items in the same turn. Context is reclaimed by auto-compaction,
which the handoff survives.
A PR is opened only for a task complete end to end: the whole path a user or
the next layer depends on (contract, engine, UI, tests, docs, screenshots), so
fewer PRs wait on CI and fewer branches conflict. No PR for half an item (an
engine half whose UI half is a separate PR, a standalone ADR for code that is
still to come): the ADR travels with the code it decides, and a slice stays on
its branch until the feature works end to end. A PR may be partial only when
the rest needs hardware or an owner decision, and its title says so. Contracts go first, as a task of their
own: a small PR with the headers or wire change, the ADR, a fake implementation
and a test, merged before the work that builds on it; agents that depend on it
start from its branch and rebase when it lands. Changing a merged contract is
an amendment asked of its owner, never a side edit. When two or three agents
work on one task, each pushes to a sub-branch, a named integrator merges them
into one feature branch, and that branch is the single PR. The coordinator
reserves ADR numbers and the next protocol version when it launches an agent.
Commit to the branch at the end of every stage, PR or not, so a stopped agent
loses nothing. One PR per BACKLOG item, follow-ups included. Every agent and every review runs on Sonnet,
never Opus (owner, 2026-11-03: it burns the weekly quota); the RT path,
contracts, storage and recovery get a more careful brief, not a bigger model. The one exception is `principal-engineer`, the escalation engineer the orchestrator calls after two failed fix rounds on the same cause, hours of red, or spent relaunches (owner, 2026-11-05); it runs on Opus, one at a time. Only a blocking finding sends a PR back; non-blocking ones become
BACKLOG lines, batched into one follow-ups PR per track. A re-review is a
fresh Sonnet agent that checks only the fix's diff. A PR enters the merge queue the moment it is opened:
`tools/enqueue-pr.sh PR...` (one detached merge-when-green per PR, a lock
refuses duplicates, it gives up after 4 h); a blocking finding runs
`tools/enqueue-pr.sh --cancel PR`. The `integrator` is for conflicts.
Agents never wait for CI, never background anything, and stop what they
started before reporting (see "Process hygiene" in each agent file). The
coordinator ends every turn with `tools/bg-status.sh` (`--kill` removes
duplicates and orphan docker runs).

## Build budget (every agent)

Builds, not coding, are the bottleneck: ten agents each running a 16-job build
saturate the machine and every build slows down. Write the whole slice first
(code, tests, docs), then build and test once per milestone, at most three
build cycles per task. No rebuild per edit; read the code instead. Use the
narrowest target, keep the build directory so it stays incremental, never
start a second container for the same worktree, and never wait on a build.
The machine runs ONE heavy build or test at a time, with every CPU, the rest
queued (`tools/build-slot.sh`, one lock for the whole host, owner 2026-11-05).
Implementers do not run the full builds or the whole test suite: they build
the narrowest target, through `tools/build-slot.sh`, only when the slice needs
it. The orchestrator's local CI gate (`tools/local-ci.sh`, run in batches by
`tools/harness/localci.py`) is what builds and tests the whole tree.

## Before opening a PR (every agent)

Each of these cost a fix round of 30–60 minutes this session. CI is slow
(the sanitizer job alone takes about 50 minutes), so a PR that fails on
something below is a PR that waits an hour.

- Run `tools/pre-pr.sh` (whitespace, exec bits, shellcheck as CI runs it, ADR
  numbers against main and open PRs, no BACKLOG edit, protocol version reserved)
  and fix what it lists before you push. Most of our red CI came from these.
- First check the BACKLOG item is not already done on main (`git log
  origin/main --oneline -S`, grep the code). Many checkboxes are stale: if it
  is done, say so in your report and stop, or take the next item.
- Build and run the tests your change touches, and say in the PR which
  command ran and what it printed. A check you did not run is listed as
  unverified, never as passing.
- Plugins and DSP: run the suite the way CI runs it, `SONORA_DSP_QUICK=1`,
  and time it. A new suite must stay under about 60 s in Release; a new
  test target must be reachable from `sonora_portable` (defined in
  `plugins/native/CMakeLists.txt`; it collects every executable and static
  library under common/json, common/dsp, common/platform, common/audiofile
  and plugins/native) and be named `dsp_*` or `native_*`, because the local
  gate `tools/local-ci.sh` runs `tools/build-portable.ps1`, which builds
  `sonora_portable` with MSVC and runs `ctest -R 'dsp_|native_'`. A processor over its
  CPU class ships only with an eco mode that meets the class (ADR-0011 §3.5);
  until then gate it off so it is not installed.
- New or changed scripts: mode 100755 in the index (`git update-index
  --chmod=+x`) for `tools/*.sh`, hooks and `usr/local/bin`; the CI "Executable
  bits" job checks it. Read C++ string literals you edited: an `\n` must not
  become a real line break.
- UI changes that show on screen: regenerate and commit the screenshots of
  every scene the change touches under `docs/images`, open them yourself, and
  list them in the PR. Stale or missing pictures block the PR.
- PowerShell 5.1 scripts: a one-element array out of an `if` expression
  becomes a string; wrap it in `@(...)`. Run it for real when you can.
- Open one PR per item with a body written from your own text, and name
  scratch files uniquely: a shared `pr.md` overwrote another agent's PR body.
  Do not push a chain of small fixes (each push restarts the whole CI); push
  once, when the work is complete. An implementer agent never edits
  `planning/BACKLOG.md` or runs `tools/progress.sh set` (`get` and `list` are
  fine): it puts the `[x]` line, the follow-ups and the progress change in the
  PR description. The coordinator's chore PR is the one place that edits them.
- Never tick a line `[x]` whose own text still names an open part; split it.

## Conventions

- C++17, 4-space indent, `snake_case` members with a trailing underscore.
- Comments explain *why*, never *what* the line already says. No comment that
  restates the code.
- Every bug fixed in the real-time path gets a regression test first.
- Decisions that shape the architecture go in `brain/decisions/` as an ADR
  before the code lands.
