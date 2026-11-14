# How the build is run

This is the harness: who does the work, where the knowledge lives, and how a
change gets from an idea to a commit.

## The four places

| Question | Answer |
|---|---|
| Where is what we know? | `brain/` — ADRs, specs, research, postmortems. Indexed in `brain/INDEX.md`. |
| Where is what we intend? | `planning/` — `ROADMAP.md` (phases), `BACKLOG.md` (ready work), `STATUS.md` (what is true now). |
| Where are the skills? | `.claude/skills/` — the repeatable procedures: build, audit, write to the brain, choose a test loop, hand off. |
| Where is work in progress? | `planning/handoffs/` — one short file per open branch, deleted on the branch before merge. |
| Where are the agents? | `.claude/agents/` — the roles, each with its own scope and standards. |

## The agents

| Agent | Owns | Invoke when |
|---|---|---|
| `architect` | Decisions spanning components; ADRs | The shape of the solution is not obvious |
| `rt-engineer` | `engine/` — the audio core | Implementing engine work |
| `distro-engineer` | `iso/`, `audio-stack/`, `tools/` | Implementing image work |
| `driver-engineer` | Hardware: USB interfaces, ALSA, control surfaces, clock and sync | The system has to talk to a physical device |
| `ui-engineer` | The operator-facing interface, `keymaps/`, the workflow layer | Anything the engineer in the room sees or touches |
| `plugin-engineer` | LV2 and VST3 hosting, AAF and `.ptx` interchange | Loading third-party code, or reading another DAW's files |
| `dsp-engineer` | `common/dsp`, `plugins/native/*`, `plugins/analyze`, the measurement harness (ADR-0011) | Our own processors, analysis passes and the mix assistant's planner |
| `researcher` | `brain/research/`, `brain/specs/` | A task depends on a fact nobody has established |
| `code-reviewer` | The review gate | Every diff, before commit |
| `rt-safety-auditor` | The audio thread call graph | Callback graph changed, or xruns appeared |
| `integrator` | The merge gate | A feature branch is ready to land on `main` |
| `janitor` | Disk: Docker objects of ours, merged worktrees, stale `build-*/` (`tools/clean.sh`) | After every merge, and when the disk fills |

## Who owns which directory

ADR-0007 §1. A squad writes only in its own directories; a change elsewhere
goes to the owner as a request. "Jointly" means both parties review.

| Directory | Owner |
|---|---|
| `contracts/api` | ui-engineer + rt-engineer jointly |
| `contracts/backend` | rt-engineer + driver-engineer jointly |
| `contracts/plugin`, `contracts/surfaces` | plugin-engineer + rt-engineer; surfaces with driver-engineer |
| `common/base`, `common/audiofile`, `common/model`, `common/session_files`, `common/json`, `common/platform`, `common/midi` | rt-engineer (ui- and plugin-engineer review `common/model`) |
| `common/dsp`, `plugins/native/*` | dsp-engineer |
| `engine/core`, `engine/control`, `engine/app` | rt-engineer |
| `engine/plugin_client` | plugin-engineer writes; rt-engineer and rt-safety-auditor gate |
| `drivers/jack`, `drivers/fake`, `drivers/support`, `engine/sync`, `drivers/surfaces` | driver-engineer |
| `ui/keymap`, `ui/client`, `ui/tui`, `ui/qt`, `keymaps/` | ui-engineer |
| `plugins/host`, `interchange/` | plugin-engineer |
| `iso/`, `audio-stack/`, `tools/`, `.github/` | distro-engineer |

Two CI gates keep this honest. `tools/check-deps.sh` fails on an include
across an edge ADR-0007 §1 does not draw, and on a vendor header (`<jack/>`,
Qt, plugin SDKs) outside its one directory; the allowed edges are in
`tools/module-deps.allow`, violations that exist on main in
`tools/module-deps.known`. `tools/contracts-lock.sh` fails when a contract
header changed and `contracts/CONTRACTS.lock` was not regenerated
(`--update`); regenerating it is the deliberate step that needs the §6
reviews. `ctest -L core|control|ui|contracts|drivers|base|model|native...`
runs one module's tests; the gate remains the full `ctest`.

The three implementer roles do not overlap by accident. A plugin crash must
not take the session down, a device that enumerates is not a device that
works, and a meter that lies is worse than a meter that is missing — each of
those is a different kind of care, and each charter says so.

## Who reviews the code

`code-reviewer` reviews every diff touching a C++ module (`common/`, `contracts/`, `engine/`, `drivers/`, `ui/`), `iso/` or the session hook (`.claude/settings.json`, `tools/session-start.sh`), after the
implementer reports done and before anything is committed. Its priority order
is real-time safety, then correctness on the data path, then concurrency, then
failure behaviour, then simplification.

`rt-safety-auditor` is the second gate, narrower and deliberately paranoid. It
runs when the callback call graph changed, when xruns appear without an
obvious cause, and before any phase is closed.

An implementer does not review its own work. The reviewer reports; the
implementer fixes; the reviewer re-reads the fix.

## The loop

1. Pick the top ready item from `planning/BACKLOG.md`.
2. `git checkout -b feature/<name>`. Work does not happen on `main`.
3. If it needs a decision → `architect`. If it needs a fact → `researcher`.
4. Implement with the role that owns the files: `rt-engineer`,
   `distro-engineer`, `driver-engineer`, `ui-engineer` or `plugin-engineer`.
5. Build and test: `build-engine`, `build-iso`, or the boot and recording
   harnesses in `tools/`.
6. Review: `code-reviewer`. For callback changes, also `rt-safety-auditor`.
   An implementer does not review its own work.
7. Fix findings. Have the reviewer read the fix.
8. Record what was learned: the `brain-write` skill. Update `STATUS.md`.
9. Commit locally. Push the branch and open a PR only once the feature works
   and the tests have been run.
10. `integrator` gates the merge: CI green, every claim in the description
    reproduced, nothing unwanted in the diff, brain current. It merges, or it
    says what is wrong and stops.
11. After merging a PR, the coordinator runs `janitor`.

## Disk

Measured on 2026-10-13: the USB writer test's work volume had reached
350.7 GB, about 20 untagged images from rebuilds held 5 GB, experiment
images and finished worktrees with ISOs (2.3 GB each) the rest. The owner's
other projects share the same Docker. The rules:

- A test that creates images or volumes cleans them up, pass or fail (a
  `trap ... EXIT`), and a test that creates sparse disk images caps them.
- Label everything ours: `docker build --label org.sonora=1`,
  `docker volume create --label org.sonora=1`, a `sonora-` name prefix,
  and `docker run --rm` unless the container must persist.
- After merging a PR, the coordinator runs the `janitor`, which removes
  only what is ours (`tools/clean.sh`). Compacting Docker Desktop's disk
  file is an owner step, described in `.claude/agents/janitor.md`.

Between steps, work moves by handoff: the agent that stops writes
`planning/handoffs/<branch>.md`, the branch name with `/` replaced by `-`
(`feature/x` → `planning/handoffs/feature-x.md`; the `handoff` skill), and
the next agent is a fresh one that starts from that file. Review rounds in
particular get a new reviewer and a new fixer each time. Sending the same
long-running agent round after round makes every round pay for all the
earlier ones. The last commit before step 10 deletes the branch's handoff,
because the integrator squash-merges and never commits.

No PR touches a file that every PR would otherwise rewrite, since each merge
would then force a conflicting rebase of every queued PR.
`planning/handoffs/main-session.md` is the coordinator's local file, ignored
by git, and the coordinator updates it after each merge. `planning/PROGRESS.md`
is generated on `main` by the `publish-progress` CI job after each merge; a
PR flips items in `planning/progress.json` only, and CI fails a PR whose diff
touches `PROGRESS.md`.

Step 8 is the one that gets skipped, and skipping it is what makes the next
session repeat work already done. Step 9 is the one I got wrong first: pushing
every commit put six broken intermediate states on a public repository.

## Spending context

Measured on one task, `write-usb.sh` (2026-10-12): a single agent session
carried the implementation, two review rounds and a rebase. Every round
re-read all the earlier ones, two full 12-minute test runs were thrown away
because a script was edited while the test read it, and the full QEMU loop
was used where the 2-minute container loop would have proved the change.
The rules that follow from it:

| Waste | Rule | Where |
|---|---|---|
| One session across many rounds | Fresh agent per task and round, joined by a handoff | `handoff` skill |
| Full test loop used to iterate | Fast loop to iterate, full loop once | `test-loop` skill |
| Logs and diffs in the context | Output to `build-*/`, read the verdict line | `test-loop` skill |
| Files read whole to find one line | `grep -n`, then read the range | `CLAUDE.md` |
| Reports restating `brain/` | Reports point to files | `handoff` skill |
| Stale skill text, rediscovered by every session | Fix the skill in the same change that made it stale | review checklist |

Interactive sessions: `/clear` between unrelated tasks. Skills load only
when invoked, so procedures belong in skills, not in `CLAUDE.md`, which every
session pays for.

## Running it unattended

Long unattended runs are possible and they have a specific failure mode: an
agent with nothing verifiable to do produces plausible code that was never
compiled. Guard against that rather than against idleness.

Rules for any unattended run:

- **Only ready work.** An agent takes the top item in `BACKLOG.md` that needs
  no decision from the owner. It does not invent scope.
- **No unverified claim.** If the build cannot run — no Docker daemon, no
  Linux — the agent stops and says so. It does not report code it never
  compiled as done.
- **Nothing lands unreviewed.** `code-reviewer` runs on every diff, including
  at three in the morning.
- **One commit per item,** so a bad run is one revert rather than an
  archaeology project.
- **Escalate instead of guessing.** Anything needing an owner decision goes to
  the bottom of `STATUS.md` under "Next decision needed" and the agent moves to
  the next item.
- **Hard stops.** Out of ready work, three consecutive failures on the same
  item, or any task that would cross ADR-0004.

### What is actually blocking unattended work right now

The Docker daemon is not running on the owner machine, so neither build can
execute. Until that is fixed an unattended run can only produce unverified
code, which is the exact failure mode above. Start Docker first.

To schedule a run, use `/loop` with an interval, or the `schedule` skill for
cron-based cloud runs. Both are owner actions: this file defines how a run
behaves, it does not start one.

## Portability

Source: [ADR-0008 section 1, "The rule"](../brain/decisions/ADR-0008-portability.md#1-the-rule),
copied as the ADR stands today (its section 1 is unchanged since 2026-10-12 in
substance; the ADR wins on any difference).

**Platform-neutral modules use standard C++17 and the modules they may link
under ADR-0007 §1. Nothing else.** That means:
- no POSIX, Linux, JACK, PipeWire, ALSA, Win32 or macOS headers;
- no `#if`/`#ifdef` on a platform macro (`__linux__`, `_WIN32`, `__APPLE__`,
  `__unix__`, `_POSIX_*`);
- no OS paths in string literals (`/proc/`, `/sys/`, `/dev/`,
  `$XDG_RUNTIME_DIR`).

Platform code lives only in **edge modules**, and is reached through a small
seam whose header is itself neutral.

Compiler feature tests are not platform tests and stay allowed (for example
`__has_cpp_attribute(clang::nonblocking)` behind `SONORA_RT`). `ui/qt` may also
use Qt, and only Qt.

| Platform-neutral (standard C++17 only) | Edge: platform code allowed |
|---|---|
| `common/base` | `drivers/*`, each backend (`drivers/jack` now, `drivers/asio` later) |
| `common/audiofile` | `engine/app`, the process entry layer (§3) |
| `common/model` *(P2)* | `ui/tui`, the POSIX terminal front end |
| `contracts/*` | `common/platform/src/<os>/` (§3) |
| `engine/core`, `engine/control` | `drivers/surfaces` *(P5)*: MIDI I/O |
| `engine/plugin_client` *(P4)*, `engine/sync` *(P5)* | `plugins/host` *(P4)*: editor windows are display-server specific |
| `drivers/fake` | `<module>/tests/posix/`: OS fault-injection tests, built `if(UNIX)` |
| `ui/keymap`, `ui/client` | `tools/`, `iso/`, `audio-stack/`, `.github/` |
| `ui/qt` (plus Qt) | |
| `interchange/` *(P4)* | |

Check: the `portable` job in `.github/workflows/nightly.yml` (builds
`sonora_portable`, which collects the neutral targets).
