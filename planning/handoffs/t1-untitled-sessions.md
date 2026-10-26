# t1-untitled-sessions

- **Goal:** ADR-0018 checklist item 4 (untitled sessions), plus #142 N2 and #147 N-A and N-B.
- **State:** in review (PR #150). CI has two red jobs, not yet examined.
- **Commits:** 2c49cc5 Untitled sessions: a folder per new session, named by its first Save.

## Verify
    gh pr checks 150 | cut -f1,2      # expected: every job pass (now: plugins and rtsan fail)

## Done
- `--sessions` flag, a fresh Untitled folder at start and on an empty-path Load, empty untitled folders removed, the first Save renames the folder and is refused during a take, a folder Load opens an unsaved recording, protocol 20. Code: `engine/control/src/active_folder.cpp`, `engine_control_server.cpp` (load, save, fence_saver), `engine/app/src/main.cpp`.
- UI: N2 naming in `ui/qt/src/session_model.cpp` writeSession; Open panel "Unsaved recordings" in `ui/qt/qml/Main.qml`.
- CI on PR #150: these jobs pass: Engine build and tests, Qt interface build and headless tests, PipeWire record, BWF interop, Shell scripts.
- ADR item 4 marked Done; `brain/specs/session-format.md` "What a session is on disk" updated.

## Open
- CI job "Plugin host against real LV2 and VST3 plugins" fails. Most likely cause: the new `plugin_hosting_flush_read_across_load` (`engine/control/tests/test_plugin_hosting.cpp`, timing driven by SONORA_TEST_SLOW_SAVE_MS) — read its log with `gh run view 40333459943 --log-failed`. Fixed means: the job is green.
- CI job "RealtimeSanitizer" fails: read its log and check whether the cause is new code or the slowed test-plugin `save()` (`plugins/host/tests/lv2/sonora_test.cpp` worker_save, nanosleep). Fixed means: the job is green.
- TSan/ASan job was still pending at handoff.
- No local build: Docker Desktop was down and the sandbox could not start it. The new tests have not been run against the old code. No UI screenshot of the Open panel yet.
- The PR body notes, for the coming ADR, where ADR-0018 assumes the stick (owner direction 2026-10-26).
- Delete this file before asking for the merge gate.

## Do not redo
- Starting Docker or WSL from the Bash tool: the sandbox refuses both.

## Pointers
- brain/decisions/ADR-0018-session-folder-owns-its-media.md, item 4 Done note.
