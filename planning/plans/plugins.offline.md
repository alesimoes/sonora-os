# Plan: plugins.offline - Offline processing of a selection (AudioSuite-style)

Status: approved
Written by: architect   Dispatched by: lead   Architect sign-off: architect 2026-11-08
Machine plan: `.harness/plan/plugins.offline.plan.json`. ADR-0087 reserved.

## 1. Goal and user experience  (architect)
Process Selection (ADR-0051, #432) already prints a selected range through the
track's WHOLE chain. The AudioSuite-style addition is applying ONE plug-in to a
selection. Flow: select a range on a track, Track > Process Selection With >
pick an insert; a render runs with a progress notice; the range becomes a
processed copy of the playlist; only that insert becomes inactive, the others
keep playing.
Out of scope: faster-than-real-time rendering through hosted plug-ins (ADR-0051
s2: hosts run only at the audio callback; a second host driver rests on an
unmeasured assumption), in-process native stock rendering (ADR-0011 1.4),
plug-ins that are not inserts of the track, batch processing of many clips.
Grouping: one backlog item (plugins.offline); engine and UI ship in one PR.
Acceptance: the acceptance lists in the JSON plan.

## 2. Design  (visual-designer)
N/A - one menu entry in the existing Track menu, no new faceplate.

## 3. Contracts and integrations
`insert=<id>` word of `track.process_selection`, `RenderOptions::only_insert`,
capability `track.render.insert`, model `RenderPlacement::only_insert`, fake.
ADR-0087. Reserved: ADR 87, no protocol bump (capability-gated), no format.
Acceptance: wire and model tests of the `contract` task.

## 4. DAW core  (rt-engineer)
Render-scoped solo through PluginHosting (override posted to hosts, document
untouched, restored on every exit, an unconfirmed host fails the render).
Nothing on the audio thread changes.

## 5. Plugins, DSP, host
N/A - no processor or host protocol change; the override reuses the bypass post.

## 6. UI  (ui-engineer)
Menu entry bound to the contract; screenshot docs/images/sonora-ui-process-with.png.

## 7. Tests  (tester)
Wire, model, engine (fake host: output, cancel/failure restore, document
unchanged) and ui_render_in_place; existing labels; run via tools/micro-build.sh.

## 8. QA  (tester)
Range on a track with limiter then reverb; process with the reverb only; undo
restores; cancel mid-render restores bypass; frozen-track entry disabled.

## 9. Quality checks
TSan on the render tests; rt-safety-auditor confirms no audio-thread change;
no file past ~500 lines (new code in new files).

## 10. Code standards
`.claude/rules/code-structure.md`, `testing-and-flakes.md`, `gates-and-checks.md`.

## 11. Dependencies and order
contract -> engine and ui in parallel -> integrator -> one PR. `interchange.stems` is active and owns
`engine_control_server.cpp`, so this feature does not touch it: the engine slice
adds `RenderAsk::from_options` and tests through `plan_render`; the one-line
wiring and the capability advertisement are a follow-up PR after
`interchange.stems` merges (this PR is titled partial for that reason; the UI
entry stays disabled without the capability).
| Slice | Agent | Difficulty | Risk | Tier | Sub-branch | Starts from | Files |
|---|---|---|---|---|---|---|---|
| contract | plugin-engineer | M | high | opus | feat/plugins.offline-contract | main | render.h, fake_engine, model render_in_place, ADR |
| engine | rt-engineer | L | high | opus | feat/plugins.offline-engine | contract | engine/control render + hosting |
| ui | ui-engineer | M | med | sonnet | feat/plugins.offline-ui | contract | Main.qml, session_model_panels |
Integrator: named by the lead.
Single PR title: plugins.offline: Process Selection through one chosen plug-in (ADR-0087)

## 12. Risks and open questions
| Question | Recommended default |
|---|---|
| Can hosts be driven faster than real time? | Unverified; researcher measures a host run from a worker at >1x; real time until then |
| Can hosts confirm a bypass override synchronously? | Assumed via the existing post/ack; engineer verifies, else fail the render |
| engine_control_server.cpp is owned by interchange.stems | Not edited here; wiring is a follow-up PR after it merges |

## 13. Done =
- [ ] contract merged into the feature branch
- [ ] every slice merged by the integrator
- [ ] tests pass, commands quoted
- [ ] QA done, screenshot committed
- [ ] `tools/pre-pr.sh` clean

## 14. Closure note (2026-11-14)

Closed by ADR-0219. On `main`: the one-insert rule and `insert=` (`54af7493`,
`89ca1b44`), the capability `track.render.insert` (announced in Hello,
`89ca1b44`), the UI entry `Track > Process Selection With ▸`
(`ui/qt/qml/Main.qml`, `54af7493`) and the control-server wiring
(`89ca1b44`, #707). The §13 boxes are not ticked here: the test run and the
pre-PR check were not run by this closure, and no screenshot is committed
(owner rule, 2026-11-13). Open, for the researcher and not blocking: the
worker-thread render speed above 1x real time (ADR-0087 §Unverified).
