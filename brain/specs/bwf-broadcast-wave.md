# Broadcast Wave Format (BWF)

Source: EBU Tech 3285, "Specification of the Broadcast Wave Format", v2.
A BWF file is a plain RIFF/WAVE file with an extra `bext` chunk. Any reader
that ignores unknown chunks sees an ordinary WAV, which is why every DAW opens
these.

## Why we care

The `bext` chunk carries `TimeReference`: the sample position of the first
frame relative to midnight. That single field is what lets a recording drop
back onto a timeline at the right place in another DAW. Without it, imports
land at zero and someone re-syncs by hand.

## `bext` chunk layout (v0, 602 bytes of payload)

| Offset | Size | Field |
|---|---|---|
| 0 | 256 | `Description` — ASCII, space-padded |
| 256 | 32 | `Originator` — we write `Sonora Engine` |
| 288 | 32 | `OriginatorReference` |
| 320 | 10 | `OriginationDate` — `yyyy-mm-dd` |
| 330 | 8 | `OriginationTime` — `hh:mm:ss` |
| 338 | 4 | `TimeReferenceLow` — low 32 bits of the sample count |
| 342 | 4 | `TimeReferenceHigh` — high 32 bits |
| 346 | 2 | `Version` — 0, 1 or 2 |
| 348 | 64 | `UMID` (v1+) |
| 412 | 10 | loudness fields (v2) |
| 422 | 180 | reserved, must be zero |

Total 602. Versions 1 and 2 keep the same offsets and only add meaning to
fields that v0 leaves zeroed, so writing zeroes there is valid.

## Implementation notes

- Strings are **not** null-terminated by the spec; they are padded. Readers
  vary, so we zero-fill the whole chunk first and write only the bytes we mean.
- All integers are little-endian, like the rest of RIFF.
- A `bext` chunk is fixed-size at 602 bytes for v0. Writing a short chunk makes
  some readers mis-parse everything after it.
- Our writer: `engine/src/wav_writer.cpp`. Field offsets are asserted in
  `engine/tests/test_wav_writer.cpp`.

## Open question

Files over 4 GB overflow the 32-bit RIFF size fields. Pro Tools and most tools
handle this with **RF64** (`RF64`/`ds64` chunks, EBU Tech 3306). We do not
write RF64 yet; a 24-bit mono 48 kHz take hits the limit at about 8 hours, so
this is not urgent but it is a real ceiling. Tracked in the backlog.
