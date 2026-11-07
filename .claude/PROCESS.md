# Process: from item to merged main

Owner, 2026-11-06: "No task leaves without a complete plan." Rules live in
`.claude/rules/`; area facts in `.claude/context/`; machine-readable maps in
`.claude/routing.toml`.

## Lifecycle

1. **Item** - a BACKLOG item, checked not already on main.
2. **Plan** (`architect`, skill `plan-task`) - `planning/plans/<id>.md` from
   `planning/plans/TEMPLATE.md`, broken down by role; every slice graded
   (difficulty x risk -> tier, `.claude/rules/agents-and-models.md`).
3. **Approval gate** - the architect decides the contracts and ADR number and
   sets `Status: approved`. No engineer is launched without it.
4. **Dispatch** (`lead`) - turns the approved plan into assignments: who does
   what, sub-branch, order, start-from, model tier. No engineer starts until
   assigned. Talk goes through the harness hub (`mcp__harness__send/recv`,
   ADR-0047), never around it.
5. **Contract PR** - small, first: headers/wire/format, ADR, fake, contract
   test. Merged before the slices start; slices start from it.
6. **Parallel role agents** - one per plan section (visual-designer,
   rt-engineer, dsp-engineer / plugin-engineer, ui-engineer, tester), each on
   its own sub-branch `sub/<feature>/<task>`, disjoint files, on the slice's
   model tier. Each runs only its module's tests, through
   `tools/build-slot.sh`, and `tools/pre-pr.sh`.
7. **Integrator** (`integrator`) merges the sub-branches into one feature
   branch: the single PR for the item.
8. **Review and QA** - `code-reviewer`, `rt-safety-auditor` on RT files,
   `tester` runs the plan's QA scenarios; fresh agents, never the author.
9. **Pre-adopt review** (coordinator, skill `pre-adopt-review`).
10. **Orchestrator adopts** the PR; batches PRs on the builder, one heavy run
    at a time under the lock (skill `run-local-ci`); the gate passes, the
    orchestrator merges. Nobody else merges.
11. **Main full run** (`tools/local-ci.sh --full`); a red main is routed below.

Loops:
- **Missing definition or ambiguity**, found by anyone: message the architect,
  who updates `planning/plans/<id>.md`; the lead re-dispatches.
- **Bug found by QA**: the lead assigns an engineer by area (`[paths]`) to fix
  it; QA re-tests.

## Which agent by area

Path and agent maps are in `.claude/routing.toml` `[paths]`. By area:
engine/model/contracts rt-engineer; ui ui-engineer; plugins/native and
common/dsp dsp-engineer; plugins/host plugin-engineer; iso and audio-stack
distro-engineer; drivers driver-engineer; tools/harness, local-ci, build
scripts harness-engineer; packaging release-engineer; splitting files
refactor-engineer; looks visual-designer; research researcher.

## Failure routing

| Failure | Agent | Escalate to principal-engineer when |
|---|---|---|
| Product test red (deterministic) | owner engineer of the path (`[paths]`) | 2 fix rounds fail at senior tier, or the cause spans modules |
| Flaky timing test (passes on rerun, unrelated PR) | flake-fixer | not reproducible under load/TSan after one attempt, or it is a real race in RT code |
| Sanitizer compile or time blow-up (ASan/TSan hang, huge TU) | refactor-engineer (split the file/header) | the split is not enough; never switch the sanitizer off |
| Sanitizer finding (real report) | owner engineer; rt-safety-auditor for RT | data race on the audio path |
| Harness, lock or infra (daemon, build-slot, Docker, local-ci) | harness-engineer | state corrupted or the daemon will not recover |
| Merge conflict | integrator | semantic conflict between contracts |
| Stalled more than 2 h (no commit, no message) | coordinator closes it, relaunches the same specialist with the handoff | stalled twice |
| Packaging / portable build / release artefact | release-engineer | - |
| Image boot / QEMU test | distro-engineer | hardware-only behaviour (researcher measures) |
| Missing or unapproved plan | architect (plan), lead (dispatch) | contract disagreement between roles -> principal-engineer |

Principal-engineer (fable tier) is for escalation only, after the senior (opus) slice spent its rounds. A red check is never
answered by switching it off (`.claude/rules/gates-and-checks.md`).
