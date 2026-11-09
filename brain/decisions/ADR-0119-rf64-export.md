# ADR-0119 - RF64 for BWF and WAV exports over 4 GiB

- Status: Accepted
- Date: 2026-11-09
- Item: `interchange.audio.rf64`
- Amends: ADR-0054 (it says a file over 4 GiB is refused and RF64 is a
  separate item; this is that item)
- Relates to: ADR-0004 (published formats only), `brain/specs/bwf-broadcast-wave.md`

## Context

An export of a long session (a 24-bit stereo 48 kHz mix passes 4 GiB at about
2 h 19 min; 18 tracks of a live set are longer) hit `OutFile::close()`, which
refused any BWF or WAV over 0xFFFFFFFF bytes: the operator got no file after
the whole render. `WavWriter` (the recording writer) already converts a take
to RF64 in place and `WavReader` already reads `ds64`, so the container is
known to us; only the export writer lacked it. EBU Tech 3306 defines RF64:
the file starts `RF64`, a `ds64` chunk holding the 64-bit sizes comes first,
and every 32-bit size that does not fit says 0xFFFFFFFF.

## Decision

1. In `OutFile` (`common/audiofile/src/export_file.cpp`) every BWF and WAV
   export reserves a `JUNK` chunk with a 28-byte body right after `WAVE` (36
   bytes on disk), the same layout `WavWriter` uses. The body is zeroes.
2. At `close()`, when the finished file is over the limit (0xFFFFFFFF bytes),
   the header is patched in place: `RIFF` becomes `RF64`, the RIFF size is
   0xFFFFFFFF, `JUNK` becomes `ds64` with riffSize, dataSize and sampleCount
   (frames) as 64-bit values and tableLength 0, and the `data` chunk size is
   0xFFFFFFFF. At or under the limit, `JUNK` stays as it is and the file is a
   plain RIFF/WAVE.
3. `bext` stays valid in RF64 (EBU Tech 3306 keeps the BWF chunks); the
   TimeReference is written as before.
4. The over-4-GiB refusal is removed for Bwf and Wav. It applies to AIFF, whose
   32-bit sizes have no extension, with the existing message
   (`<path> is over 4 GiB, which a WAV cannot hold`); the export fails and
   `export_audio` removes the partial file. FLAC is unchanged (its STREAMINFO
   has a 36-bit sample count).
5. The size is decided at `close()`, so nothing is buffered and the
   kept-name reservation and the cancel/failure cleanup are untouched.
6. Test seam: the size limit is a constructor parameter of `OutFile`
   defaulting to 0xFFFFFFFF; tests lower it so none writes 4 GiB. `OutFile`
   is private to the library (`src/export_file.h`).
7. The file size is counted by `OutFile` itself, not read back with `ftell`,
   whose `long` is 32-bit on Windows.

## Rationale

The data size is not known until the last block, and the header is at the
front. Reserving the room costs 36 bytes per export and removes any need to
move audio later or to guess. The reserved chunk is a legal `JUNK` chunk, so
every reader that ignores unknown chunks reads an ordinary file under the
limit, and the over-limit file is exactly what EBU Tech 3306 describes.

## Consequences

- Every BWF/WAV export is 36 bytes longer than before; nothing else in a file
  under the limit changes.
- A long export now succeeds; the operator gets a file other DAWs that read
  RF64 open (the format's whole point).
- `OutFile` and the FLAC bit pieces moved to `export_file.*` and
  `flac_bits.*` in a refactor commit before this change; `audio_export.h` is
  unchanged.
- `brain/specs/bwf-broadcast-wave.md` records the export side.

## Alternatives rejected

- **Buffer and decide up front.** Hold the audio, or render twice, to know the
  size before writing the header. A render of that length is hours; buffering
  4 GiB breaks the memory budget and a second pass doubles the time.
- **Always write RF64.** No size problem, but every export would be a file
  that older readers and some importers refuse, to serve the rare long one.
- **A sidecar file for the sizes.** Splits one deliverable into two files that
  can part, and no other tool reads it.
- **Split the export into several files.** Changes what the operator
  receives and breaks the one-file-per-track naming; not asked for.

## Unverified

- Third-party readers on a real file over 4 GiB. The tests lower the limit and
  read the result with `WavReader` and byte-level checks of the header; no
  other DAW or library opened a real >4 GiB export.
- The over-limit path at its true size (a 4 GiB write) was not run.
- `WavReader` on Windows past 2 GiB uses a 32-bit `long` for offsets; the
  export side does not, the reader is outside this change.
