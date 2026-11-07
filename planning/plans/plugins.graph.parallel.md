# Plan: plugins.graph.parallel - Parallel processing graph on the plan, nodes dropped on deadline

Status: approved
Written by: architect   Dispatched by: lead   Architect sign-off: architect 2026-11-07
Grouped items: only `plugins.graph.parallel` (BACKLOG DAW 4 item 8); buses, sends and automation of that line are on main (ADR-0022/0024/0027), so nothing else of the area ships here.

## 1. Goal and user experience  (architect)
A big session (64+ strips, aux buses) no longer overruns the callback because playback/mix nodes are serial. Independent nodes may run on worker threads; a node that misses its deadline is dropped (ramped silence, counted), never stalling the audio thread. Default workers = 0: behaviour unchanged. Out of scope: UI, protocol surfacing of drop counters, plugin-host changes, capture path.
Acceptance: ADR-0063 holds; N workers equal serial bit for bit with no drops; a stalled node is dropped within the deadline and counted; pool start failure fails session creation.

## 2. Design  (visual-designer)
N/A - no screen; protocol none reserved, counters are `Session` accessors only.

## 3. Contracts and integrations  (architect + owner engineer)
ADR-0063. `PlanNode::level`, `PlaybackPlan::level_first/level_count` in `engine/core/include/sonora/playback_plan.h`; `GraphScheduler` interface and serial scheduler in new `graph_schedule.h`; fake scheduler with injectable stall. No `contracts/` change, no wire, no format.
Deliverables: ADR file, headers, serial implementation, `test_graph_schedule_contract.cpp`, CMake glob `graph_*.cpp` in `engine/core` (sources are listed explicitly today; the contract slice owns the CMake edit so later slices do not overlap).
Reserved numbers (ADR, protocol, format): ADR 63, protocol none, format none.
Acceptance: `ctest -L engine -R graph_schedule` passes via `tools/build-slot.sh`.

## 4. DAW core  (rt-engineer)
Levels pass in the builder; worker pool scheduler; Session integration (dropping, ramp, counters, fail-loud creation). RT: no alloc, lock, throw, syscall on the audio thread; bounded spin only.
Deliverables: `engine/core/src/graph_levels.cpp`, `graph_workers.cpp`, `plan_graph.cpp` hook, `session.cpp` call site only.
Acceptance: section 7 tests pass; RTSan clean on the node call graph.

## 5. Plugins, DSP, host  (dsp-engineer, plugin-engineer)
N/A - InsertChain nodes stay on the audio thread unchanged (ADR-0063 section 7); host protocol untouched.

## 6. UI  (ui-engineer)
N/A - no protocol field reserved; surfacing the counters is a follow-up.

## 7. Tests  (tester)
`test_graph_schedule_contract`, `test_graph_levels` (edges only go up, deterministic order, PDC and delay-line ids unchanged), `test_graph_workers` (bit-exact vs serial at N=1..4; stalled node dropped by deadline; late result ignored; 8 drops silence the node; start failure errors), `test_session_parallel` (zero allocations; a take's recorded samples identical with and without drops). Label `engine`; progress-bounded waits, no sleeps; stalls injected by the fake, not by time.
Acceptance: `tools/micro-build.sh` for the diff, then `ctest -L engine`.

## 8. QA  (tester)
64-strip session at N=0 and N=3 with an injected stall; recording continues, drops counted. No screenshots (no UI).

## 9. Quality checks
RTSan and TSan on workers; rt-safety-auditor rules on the bounded spin and epoch ordering; no file past ~500 lines (session.cpp is 3.2k: call site only, no refactor in this PR); docs: engine-rt context line.

## 10. Code standards
`.claude/rules/code-structure.md`, `testing-and-flakes.md`, `gates-and-checks.md`; DI through `GraphScheduler`.

## 11. Dependencies and order
contract -> levels || workers -> session -> one PR.
| Slice | Agent | Difficulty | Risk | Tier | Sub-branch | Starts from | Files |
|---|---|---|---|---|---|---|---|
| contract | rt-engineer | M | high | opus | plugins.graph.parallel-contract | feature branch | ADR, graph_schedule.h, serial impl, contract test, CMake |
| levels | rt-engineer | M | high | opus | plugins.graph.parallel-levels | contract | graph_levels.cpp, plan_graph.cpp, test_graph_levels.cpp |
| workers | rt-engineer | L | high | opus | plugins.graph.parallel-workers | contract | graph_workers.*, test_graph_workers.cpp |
| session | rt-engineer | L | high | opus | plugins.graph.parallel-session | levels, workers | session.cpp call site, test_session_parallel.cpp, engine-rt context |
Integrator: named by the lead.
Single PR title: Parallel processing graph: levels, help-first workers, nodes dropped on deadline (ADR-0063)

## 12. Risks and open questions
| Question | Recommended default |
|---|---|
| Default worker count | 0 until the bare-metal measurement (ADR-0063 Unverified 1) |
| Deadline fraction | 70 % of the period |
| Surface drop counters on the wire | Later protocol bump, not here |
| Wake mechanism | Timer-paced, wake gate off |

## 13. Done =
- [ ] contract slice merged into the feature branch
- [ ] every slice merged by the integrator
- [ ] tests of section 7 pass (commands and output in the PR)
- [ ] QA scenario done
- [ ] quality checks of section 9 reported, measurements handed to the researcher
- [ ] `tools/pre-pr.sh` clean
