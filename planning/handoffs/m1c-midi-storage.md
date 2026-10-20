# m1c-midi-storage

- **Goal:** ADR-0015 M-1c PR 1, storage and format (BACKLOG Track 2 item 4).
- **State:** in review.
- **Commits:** see `git log origin/main..m1c-midi-storage`.

## Verify
    tools/build-engine.sh    # expected: 100% tests passed, 0 tests failed out of 237
    tools/build-ui.sh        # expected: 100% tests passed, 0 tests failed out of 249

## Done
- `.msq` codec, canonical, fuzzed — evidence: `common/model/tests/test_sequence_file.cpp`
- Format 6, v5 -> v6, missing files open silent (O2), Load bundle — evidence:
  `test_model_session_file` (19 passed), `test_model_midi` (14 passed)
- Interface saver writes sidecars first, cleans up (O4) — evidence: `test_ui_edit`
  `test_midi_sequence_files`
- Budget at open and its 90 % Notice on the real engine — evidence: `test_control`
  `midi_budget_at_open_and_its_notice`
- Unverified 2 and 6 measured — evidence: `brain/specs/midi.md` §4.6

## Open
- PR 2: chunks (§1, §2), chunk table and Snapshot in parts (§4), Unverified 1, 3, 7.
- PR 3: sync pacing behind checkpoints with the engine's saver thread, unplaced
  takes, rebuild from the take file, Unverified 5.

## Do not redo
- An engine that reads the interface's folder at Load: ModelAction::Load carries
  bytes by design (ADR-0007), so it carries a session bundle instead.

## Pointers
- `brain/decisions/ADR-0015-midi-sequence-storage-at-scale.md`
- `brain/specs/session-format.md` ("Saving", "Opening", "Versioning")
- `brain/specs/midi.md` §4.4.1, §4.6
