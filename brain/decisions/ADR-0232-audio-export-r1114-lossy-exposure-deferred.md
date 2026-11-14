# ADR-0232 - `interchange.audio.export.r1114`: lossy exposure needs its own protocol slot

- Status: Accepted (recommended default)
- Date: 2026-11-14
- Item: `interchange.audio.export.r1114`
- Relates to: ADR-0182 (lossy codec seam), ADR-0054, ADR-0119

## Context
r1114 repeats `interchange.audio.export` (status doing, PR #740). Done on main: WAV/BWF, AIFF, FLAC, RF64, dither, SRC and the lossy encoder seam (ADR-0182). Open: `api::BounceFormat` (`contracts/api/include/sonora/api/bounce.h`) is Int24/Float32/Int16 only, so BounceWindow cannot select Opus or Vorbis. ADR-0182 section 4 says no wire change.

## Decision
No code in this feature. Exposing a lossy codec adds an optional field to the bounce wire message, which needs a reserved protocol number; the daemon reserved none for r1114. The work stays on the parent item `interchange.audio.export`, planned when a protocol number is reserved: contract PR first (optional `codec` field, absent = lossless as today, plus fake and codec test), then engine refusal at start (ADR-0182 section 3), then BounceWindow selector, one PR.

## Rationale
Guessing a protocol number would collide with other PRs. A duplicate id must not open a second PR for the same item.

## Consequences
Unverified: whether real libopus/libvorbis encoders are linked in the image build; the nightly build settles it.

## Alternatives rejected
- Overload `BounceFormat` with codec values: breaks the lossless/lossy refusal split of ADR-0182.
- Implement now without a protocol number: wire change without a reservation.
