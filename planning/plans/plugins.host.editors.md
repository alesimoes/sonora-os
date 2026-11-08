# Plan: plugins.host.editors - Plugin editor windows from the UI

Status: approved
Written by: architect   Dispatched by: lead   Architect sign-off: architect 2026-11-08
Grouped items: only `plugins.host.editors` is open in this area. The host half (#322, #339) is already on main; this feature is the engine and UI half, one PR. D10 `ui.design.plugin_chrome` stays its own item.

## 1. Goal and user experience
The user clicks "Open editor" on an insert with a native VST3 UI. The plugin's window appears as its own toplevel (WSLg in development, labwc on the appliance); values moved there reach the document; closing it updates the slot. If it cannot open (no display, LV2 UI, no X11) the insert panel says why and offers the generic editor. Out of scope: LV2 UIs, embedding in Qt, plugin window chrome (D10), Windows.
Acceptance: slot shows Opening/Open/Failed(reason); a second click raises, never duplicates.

## 2. Design
N/A - reuses spec ui-design.md "Plugin window chrome" States; only a status banner is new (ui-engineer, from the spec).

## 3. Contracts and integrations
ADR-0088 (reserved). Header engine/control/include/sonora/control/plugin_editor_service.h with fake and contract test, merged first. The wire (OpenPluginEditor, ClosePluginEditor, PluginEditorState) needs a protocol number; none is reserved yet, so the engine slice asks the lead. The contract PR carries no wire.
Acceptance: per task in the plan JSON.

## 4. DAW core
Engine slice only (engine/control service over HostSupervisor). No audio-thread change: editors run on the host's main thread (plugins/host/src/editor.h).

## 5. Plugins, DSP, host
N/A - host side done (#322, #339); no change to plugins/host or HostControl.

## 6. UI
insert_editor_controller and EditorStatusBanner, bound to the new commands and event.

## 7. Tests
Contract test (fake service), engine tests with fake host, codec round trip, UI controller test with fake engine. Labels per module.

## 8. QA
Scenarios: open, double open, failed open (no DISPLAY), host crash while open, chain removed while open. Screenshots docs/images/insert-editor-*.png.

## 9. Quality checks
No audio path touched; reviewer confirms. Files under ~500 lines (plugin_hosting.cpp and host main.cpp not grown). BACKLOG `[x]` line goes in the PR body.

## 10. Code standards
`.claude/rules/code-structure.md`, `testing-and-flakes.md`, `gates-and-checks.md`.

## 11. Dependencies and order
contract -> engine -> ui, one feature branch, one PR.
| Slice | Agent | Difficulty (S/M/L) | Risk | Tier | Sub-branch | Starts from | Files |
|---|---|---|---|---|---|---|---|
| contract | plugin-engineer | M | high | opus | feat/plugins.host.editors-contract | main | ADR-0088, plugin_editor_service.h, test |
| engine | plugin-engineer | L | high | opus | feat/plugins.host.editors-engine | contract | engine/control, contracts/api |
| ui | ui-engineer | M | med | sonnet | feat/plugins.host.editors-ui | engine | ui/qt, docs/images |

Integrator: lead names one.
Single PR title: Plugin editors: open, close and report from the UI (ADR-0088)

## 12. Risks and open questions
| Question | Recommended default |
|---|---|
| Protocol number | Lead reserves the next free one before the engine slice starts |
| Editor behaviour under WSLg and labwc (placement, focus back to the UI) | Unverified. Researcher measures with a real VST3 (also the BACKLOG item "editors under the kiosk compositor"); this feature does not wait for it |
| LV2 editors | Refused with a reason; generic editor offered |

## 13. Done =
- [ ] contract PR merged
- [ ] slices merged into the feature branch
- [ ] tests pass (commands in PR)
- [ ] QA done, screenshots committed
- [ ] `tools/pre-pr.sh` clean; no file past ~500 lines

## Sequencing note

The engine slice registers its sources and tests in `engine/control/CMakeLists.txt`,
which the active feature `interchange.stems` also edits. The plan therefore sets
`depends_on_feature: interchange.stems`: the contract slice (ADR, header, test)
may start at once, the engine slice starts from main after stems merges.
