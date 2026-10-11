# How the build is run

This is the harness: who does the work, where the knowledge lives, and how a
change gets from an idea to a commit.

## The four places

| Question | Answer |
|---|---|
| Where is what we know? | `brain/` — ADRs, specs, research, postmortems. Indexed in `brain/INDEX.md`. |
| Where is what we intend? | `planning/` — `ROADMAP.md` (phases), `BACKLOG.md` (ready work), `STATUS.md` (what is true now). |
| Where are the skills? | `.claude/skills/` — the repeatable procedures: build, audit, write to the brain. |
| Where are the agents? | `.claude/agents/` — the roles, each with its own scope and standards. |

## The agents

| Agent | Owns | Invoke when |
|---|---|---|
| `architect` | Decisions spanning components; ADRs | The shape of the solution is not obvious |
| `rt-engineer` | `engine/` — the audio core | Implementing engine work |
| `distro-engineer` | `iso/`, `audio-stack/`, `tools/` | Implementing image work |
| `researcher` | `brain/research/`, `brain/specs/` | A task depends on a fact nobody has established |
| `code-reviewer` | The review gate | Every diff, before commit |
| `rt-safety-auditor` | The audio thread call graph | Callback graph changed, or xruns appeared |

## Who reviews the code

`code-reviewer` reviews every diff touching `engine/` or `iso/`, after the
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
2. If it needs a decision → `architect`. If it needs a fact → `researcher`.
3. Implement with `rt-engineer` or `distro-engineer`.
4. Build and test: the `build-engine` or `build-iso` skill.
5. Review: `code-reviewer`. For callback changes, also `rt-safety-auditor`.
6. Fix findings, re-review.
7. Record what was learned: the `brain-write` skill. Update `STATUS.md`.
8. Commit.

Step 7 is the one that gets skipped, and skipping it is what makes the next
session repeat work already done.

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
