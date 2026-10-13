# feature/midi-core

- **Goal:** `common/midi`, platform-neutral MIDI core (owner decision 2026-10-13: full MIDI); catalogue `engine.midi.*` in `planning/progress.json`.
- **State:** in review (PR open, not merged).
- **Commits:** see `git log origin/main..feature/midi-core`.

## Verify
    tools/build-engine.sh    # expected: 100% tests passed, 0 tests failed out of 14

## Done
- Event model, tempo map, notes, SMF codec, edits, live stream parser — evidence: `ctest -L midi` (6 tests: midi_event, midi_tempo_map, midi_notes, midi_smf, midi_edit, midi_stream_parser)
- RT `sonora::MidiBuffer` in `common/base` — evidence: `midi_buffer` ctest, allocation counter via replaced global `operator new`
- 0 warnings on a clean Release build (GCC 14, trixie container); TSan and ASan+UBSan builds pass all 14 tests (TSan run with `--security-opt seccomp=unconfined` + `setarch x86_64 -R`)

## Open
- ADR-0007 §1 table has no `common/midi` row (layer 1, links `common/base` only) and §2 should name `MidiBuffer` as common/base RT code — architect to amend before merge.
- `interchange.midi.smf` (import/export through `common/model`) waits on M8 (`feature/timeline-model`).
- MIDI regions in the model: `TempoMap` is shaped to move into `common/model`; tempo is µs/quarter (SMF), the session format stores BPM — reconcile when it moves.

## Do not redo
- Tempo conversions in floating point: replaced by integer µs·tick arithmetic; exact over 3 h at 192 kHz (test `long_positions_stay_exact`).

## Pointers
- `common/midi/include/sonora/midi/*.h` (decisions are in the header comments: FIFO pairing, nested same-pitch limit, swing convention, transpose clamping)
- `brain/specs/timeline-model.md`, ADR-0004, ADR-0007 §1–2, ADR-0008
