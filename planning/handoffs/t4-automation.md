# t4-automation handoff (ui.mixer.automation, automation.lanes)

State: first slice done and green. Read playback of volume and pan, EditAutomation with undo, mode off/read. Rebased on origin/main (after #167, #172, #173). tools/build-engine.sh: 270/270 passed. ui/qt not built: the only UI change is the undo label in edit_clipboard.cpp.
ADR-0027 (automation lanes and modes): Accepted on the recommended defaults.
Numbers: session format 13, window format 10, protocol 32 (31 is #167's).
Tests (all new behaviour, so none compiled before the change; a mutation check stands in for "fails before the fix"):
- engine/core `automation`: lane math, read playback, block-split invariance, locate mid-ramp, loop wrap, Off and pan, heap counter. Removing the crossfade fails locate; removing the wrap fails loop; anchoring the grid to the block fails invariance.
- model `model_automation` (new); `model_session_file` v12 migration and lane round trip; `playback_control` test_automation_reaches_the_plan; conformance check_automation (fake and real engine).
Open, in order:
1. Open the PR (body draft: scratchpad pr-t4-automation.md; it holds the gap list), then an Opus review for the RT path.
2. Run tools/build-ui.sh once (the model changes affect the UI build).
3. BACKLOG lines added under tracks 1 to 4: the write slice (touch, latch, write, trim), the lane UI, the mode selector, plugin parameter lanes, follow-ups.
Verify: tools/build-engine.sh; then in sonora-build:trixie run `ctest --test-dir build-container -R "automation|model_session_file|playback_control|api_"`.
