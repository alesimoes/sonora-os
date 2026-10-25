# Broadcast Wave Format (BWF)

Source: EBU Tech 3285, "Specification of the Broadcast Wave Format", v2.0
(May 2011), https://tech.ebu.ch/docs/tech/tech3285.pdf, §2.3 for the chunk.
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
| 320 | 10 | `OriginationDate` — `yyyy-mm-dd` (any separator; `- _ : .` or space recommended) |
| 330 | 8 | `OriginationTime` — `hh:mm:ss`, same separator rule |
| 338 | 4 | `TimeReferenceLow` — low 32 bits of the sample count |
| 342 | 4 | `TimeReferenceHigh` — high 32 bits |
| 346 | 2 | `Version` — 0, 1 or 2 |
| 348 | 64 | `UMID` (v1+) |
| 412 | 10 | loudness fields (v2) |
| 422 | 180 | reserved, must be zero |

Total 602. Versions 1 and 2 keep the same offsets and only add meaning to
fields that v0 leaves zeroed, so writing zeroes there is valid.

## Implementation notes

- A string shorter than its field "shall be" followed by a null (§2.3, for
  Description, Originator and OriginatorReference); a full-length string has
  none. We zero-fill the whole chunk first and write only the bytes we mean,
  which satisfies both cases.
- All integers are little-endian, like the rest of RIFF.
- A `bext` chunk is fixed-size at 602 bytes for v0. Writing a short chunk makes
  some readers mis-parse everything after it.
- What we write: Originator, TimeReference (the session playhead at record
  start, so session-relative rather than "since midnight"), Version 0, and
  OriginationDate/OriginationTime as below.
- A take's `Description` is ours to define: `sonora take=<n> track=<id>
  punch=<A>-<B> id=<session_id> session=<name>`, with the session's name
  last as the rest of the field and the whole at most 255 bytes, so the
  field keeps its null. The session id (36 bytes as written) says which
  session a take is of, which the name cannot: two unsaved sessions are
  both "Untitled". The mapping and its rules are in
  [session-format.md](session-format.md), "BWF metadata, both ways".
- `OriginationDate` and `OriginationTime` are filled when the file is opened,
  from the **local** wall clock. Tech 3285 names no time zone; the fields
  describe when the recording was made where it was made, which is local
  time. If the clock cannot be read the fields stay zero rather than wrong.
  The clock is injectable (`WavWriter::set_clock`) so the test can pin it.
- An odd-sized `data` chunk is followed by one zero pad byte, which the data
  chunk's size does not count and the RIFF size does (RIFF chunk rule). Mono
  24-bit with an odd frame count is the case that needs it.
- Our writer: `common/audiofile/src/wav_writer.cpp`. Field offsets are
  asserted in `common/audiofile/tests/test_wav_writer.cpp`.
- Checked against ffprobe, soxi, libsndfile, mediainfo and Ardour 8.12 by
  `tools/test-bwf-interop.sh`; results and reader quirks in
  `brain/research/bwf-interop.md`. Ardour treats a TimeReference of 0 as
  "no timestamp".

## Open question

Files over 4 GB overflow the 32-bit RIFF size fields. Pro Tools and most tools
handle this with **RF64** (`RF64`/`ds64` chunks, EBU Tech 3306). We do not
write RF64 yet; a 24-bit mono 48 kHz take hits the limit at about 8 hours, so
this is not urgent but it is a real ceiling. Tracked in the backlog.

Until RF64 lands the writer never wraps: a write that would take the RIFF
size past 2^32 - 1 (counting the pad byte) writes only the whole frames that
fit, marks the take failed with "take reached the 4 GB WAV limit", and
`Session::stop()` reports it with the file name. The file stays valid.
