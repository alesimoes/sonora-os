# ADR-0033 — Varispeed: a plain roll at a ratio, pitch following

- Status: Accepted (recommended defaults, taken by the implementer with no
  owner available, 2026-11-04)
- Date: 2026-11-04
- Progress item: `time.varispeed` (Logic Pro: Use Varispeed)

## Summary

*Navigation only; the sections below are the decision.*

- §1: `Session::set_varispeed(ratio)` sets the playback speed, 0.5 to 2.0,
  1.0 at every session open. It is the tape model: pitch follows speed.
- §2: the audio thread reads each track's source at `start + i * ratio`,
  linear interpolation, and advances the playhead by the source frames
  consumed, carrying the fractional phase between chunks and blocks.
- §3: scope. Only a plain roll plays off-speed. A take, a playback loop, a
  pending post-roll play at 1.0; the click is silent off-speed. Automation,
  fades and plug-in chains evaluate at the chunk's start frame.
- §4: no new allocation: one `2 x max_block` buffer sized at construction.
  No protocol or session-format change (the ratio is not saved).

## Decision

1. Range [0.5, 2.0], a non-finite or out-of-range value is refused and
   changes nothing. The ratio is a relaxed atomic read once per callback.
2. A callback chunk of n output frames reads
   `floor(phase + (n-1) * ratio) + 2` source frames (the timeline render
   unchanged, into `vs_buf_`) and interpolates. The chunk is cut so that
   count fits `max_block`. The playhead moves by `floor(phase + n * ratio)`;
   the remainder is the next phase. A locate (playhead differs from the one
   expected) clears the phase.
3. Recording at another speed would need the take's file to carry the
   ratio; a loop and a roll end count timeline frames one for one. Those
   stay at 1.0 until an ADR says how they would work. Linear interpolation
   aliases above 1.0; a better kernel (sinc) is a later quality step that
   changes no contract.
4. The UI control (a transport field and a key) and the protocol message
   follow in the control and UI tracks; this ADR is the engine contract.
