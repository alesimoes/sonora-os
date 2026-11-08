# automation.list: verification record

Closure item (ADR-0083). No code change.

## 13. Verification result (2026-11-08)

Bundle #439 (7c794eef) already landed window.event_list, automation.write_selection
and automation.snapshot, with keys, menu entries, the screenshot and the tests.

    $ git log origin/main --oneline -S snapshot_automation
    8637e8c4 ui/qt: split session_model.cpp by domain (no behaviour change) (#463)
    7c794eef Bundle editing-ui: #378, #430, #385, #387, #388, #389, #390, #408 (#439)

`test_ui_event_list` is defined in ui/qt/CMakeLists.txt; it was not run here
(unverified). Write to All Enabled is out of scope.
