# ADR-0126: Chord track and chord following

Status: accepted (2026-11-09)

## Summary

`common/midi` gains `Chord` (root, quality, optional slash bass), a
`ChordTrack` (sorted, one chord per tick) and `follow_chords`, which makes a
`NoteList` follow the track by transposing from a reference chord or by
snapping to chord tones. Control thread only; nothing here runs in
`Session::process()`, no contract, protocol or session format changes.

## Decisions

- Qualities: major, minor, dim, aug, 7, maj7, m7, sus2, sus4. Text form is
  `C`, `Cm`, `F#dim`, `Bbmaj7`, `C/E`; format picks sharps or flats.
- A chord applies from its tick until the next change. Notes starting before
  the first chord are left alone.
- Transpose moves by the shortest root interval, -6..+5 (a tritone goes
  down). SnapToChord moves to the nearest chord tone, ties down.
- Pitches stay in 0-127 by octave shift. The source list is never modified.
