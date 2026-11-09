# ADR-0143 — Pitch editing of audio: notes are detected and edited in the editor, the shift is an offline render into a copy

- Status: Accepted (recommended defaults, architect, 2026-11-09)
- Date: 2026-11-09
- Builds on: ADR-0136 (elastic: offline copy, one Compound, capability), ADR-0052 §4
  (`sample.edit` verbs write a copy; undo repoints), ADR-0009 (sources append-only),
  ADR-0008 (portability), ADR-0004 (workflow, not binaries)
- Progress item: `time.pitch` (Flex Pitch class: pitch of monophonic audio edited note by note)

## Context
The operator wants to select a monophonic region (vocal, bass, lead), see its notes, drag a
note to another pitch, straighten drift, scale vibrato, nudge formant and gain, or snap notes
to a key. Doing it live needs a pitch shifter on or beside the audio thread and persisted
note data (format bump, plan builder, resident pre-shifted chunks): a large RT-path change
resting on unmeasured CPU. ADR-0136 already settled the same trade for time stretch.

## Decision
1. **Same route as ADR-0136: destructive by copy.** Notes and edits live in the editor while
   the operator works. Nothing enters the session: no session format, no protocol bump, no
   new Operation, no catalogue entry. Apply renders `Audio Files/<track>_pitch_NN.wav`
   (exclusive create, synced) and submits ONE Compound (AddSource, SetRegionSources); the
   take is never opened for writing; one undo restores the original.
2. **Command: a new verb `pitch` of the existing `sample.edit`** (engine/control
   `SampleVerb`), not a new command. It needs no `command.h` entry and no fake-engine
   change; the capability is the existing `sample.edit` plus the new reply field `pitch` in
   the capability string only when a shifter is built. PARAM is a space-free note list,
   `n=START:END:CENTS[:FORMANT_CENTS[:GAIN_DB]];...` (frames relative to the region,
   at most 64 notes, strictly increasing and non-overlapping, cents and formant cents
   -2400..2400, gain -48..12 dB kept to 0.01 dB, at most 4096 characters, numbers
   read and printed without the locale), parsed and formatted by
   `contracts/api/include/sonora/api/pitch.h` (header-only, with `why` on every refusal).
   Cost later: one verb carries a note list; if edits outgrow 64 notes a dedicated command is
   an amendment.
3. **Analysis is ours, pure and off the audio thread:** `common/pitch` holds a `PitchTracker`
   interface with a deterministic YIN-class implementation (published algorithm), note
   segmentation (onset, offset, median cents, drift, vibrato depth), the `PitchPlan`
   (per-note shift, drift and vibrato scale, formant, gain, scale snap with an amount) and
   a `PitchShifter` interface with a fake. No ONNX here (ADR-0011 keeps it in analyze).
4. **Shifter:** Rubber Band (GPL-2.0-or-later), offline, per-segment pitch scale with
   formant option, behind `PitchShifter`; the adapter is optional through pkg-config. Absent
   library: the `pitch` capability is off and the UI shows why. It must not link the
   elastic adapter's code: if time.elastic has merged, a follow-up unifies the two adapters.
5. **Refusals, all held notices:** polyphonic or unpitched material (too few voiced frames),
   a note list outside the region or overlapping, take open, free space short, result
   length differs from the region (never ripples; the shift keeps length).
6. **Overlap with in-flight features:** this ADR's code lands in new directories only.
   CMake hooks go at the top of `plugins/analyze/CMakeLists.txt` (guarded `if(EXISTS)`);
   `engine/pitch` is added by a `cmake_language(DEFER DIRECTORY <root>)` call that runs
   once the root is read and only `if(TARGET sonora_control)`, so its own
   `CMakeLists.txt` adds its sources to `sonora_control` with a plain `target_sources`
   and labels its tests itself. The contract test is `contracts/api/pitch`. They move to the root and `engine/control/CMakeLists.txt`
   once time.elastic and ui.mixer.sidechain merge.

## Rationale
Reuses the one proven destructive path, adds nothing to `process()`, cannot lose a take.

## Consequences
Notes are re-detected after Apply (the copy is re-analysed, or the operator undoes). The
window hook and menu entry (EditWindow.qml, scene files) are claimed by other features: the
first PR ships the module, the panel and screenshots, and the one-line hook follows once
they free (PR title says partial).

## Alternatives rejected
- Live non-destructive pitch (format bump, shifter near the audio thread, unmeasured CPU).
- A new `audio.pitch` command: forces edits to `command.h` and the fake engine, both
  claimed now, and duplicates the render job's plumbing.
- Extending `audio.elastic` with a pitch mode: same files, and couples two unmerged ADRs.
- A proprietary or ML pitch tracker: licence and ADR-0004; YIN is published.

## Unverified (researcher)
1. Rubber Band quality for pitch-only shifts of voice at +-300 cents with formant on;
   needs listening plus a measured pitch error (target < 5 cents) on a sung test file.
2. YIN accuracy and CPU on 10 min mono 48 kHz (target: < 20 % of real time, < 10 cents
   error on the voiced frames of a reference file).
3. The deferred `add_subdirectory(engine/pitch)` and its `target_sources` into
   `sonora_control` under CMake of the shipped toolchain (3.19+ needed;
   older warns); fallback is the root hook after time.elastic merges.
4. Whether sample.edit's own line parsing takes a 4096-character PARAM. The wire is
   settled: `kMaxPitchParamChars` (64 x 64) is below `kMaxText` (64 KiB), checked by the
   contract test.
