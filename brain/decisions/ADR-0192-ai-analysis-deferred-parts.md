# ADR-0192: native.ai_analysis, the two deferred parts

Status: accepted (2026-11-14)

## Summary
ADR-0066 shipped (#501). Its two remaining parts wait: artist-tone matching on owner hardware, tuner-follows-key on a session-format key field.

## Context
ADR-0066 section 6 defers artist-tone matching and tuner-follows-key. Neither can start now.

## Decision
1. Artist-tone matching stays parked with the hardware validations (owner, 2026-11-13). It needs amp capture cards recorded on hardware the owner provides (ADR-0011). Covered by `ampcab.artist_tone.rest`. No product, capture or preset name carries an artist's name (ADR-0004, ADR-0011).
2. Tuner-follows-key needs a key field in the session format: its own ADR and protocol bump, on the DAW track, with its own BACKLOG line.
3. No code changes under this item until one of those unblocks.

## Rationale
Starting either now means guessing a hardware capture or a session-format field that the DAW track owns.

## Consequences
native.ai_analysis closes as partial. ADR-0066 gets a dated pointer to this ADR.

## Alternatives rejected
- Add a key field to the session format from the plugin track: crosses tracks and needs a protocol bump.
- Fake artist tones with synthetic captures: misleads users and risks naming artists.

## Unverified
That ORT 1.21 loads keynet.onnx (ADR-0066 Unverified 1); needs the researcher and an image with `SONORA_WITH_ONNX=ON`.
