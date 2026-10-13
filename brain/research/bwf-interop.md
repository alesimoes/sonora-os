# BWF interop: do our takes open elsewhere at the right timecode?

Phase 1's exit criterion (`planning/ROADMAP.md`) says the recorded files open
in another DAW at the right timecode. Until 2026-10-13 nobody had checked.
`tools/test-bwf-interop.sh` now checks it, and CI runs it (job `interop`).

## What was measured, and where

- **Machine:** the owner's Windows 11 host, Docker Desktop, 2026-10-13. No
  audio interface: PipeWire's null sink in the runtime image
  (`tools/Dockerfile.runtime`, the audio stack the ISO ships), quantum 128,
  48 kHz. Nothing here says anything about the UMC1820 or the RT kernel.
- **Recording:** the engine in its interactive key loop on a pseudo-terminal
  (`tools/bwf-interop.py drive`). It pressed record, stop, record, stop, quit.
  18 tracks, two takes of about 2 s, 36 files. The headless `--record` mode
  was not used on purpose: its take always starts at playhead 0, and a
  TimeReference of 0 is also what a writer that never sets the field
  produces. Take 2 starts where take 1 stopped, so its right answer is known
  and non-zero.
- **Readers**, all Debian trixie packages, in a separate image
  (`tools/Dockerfile.bwfreaders`): ffprobe 7.1.5, SoX 14.4.2 (`soxi`),
  libsndfile 1.2.2 (`sndfile-info`), MediaInfoLib 25.04, Ardour 8.12.0.
  BWF MetaEdit is not packaged in trixie (`apt-cache policy bwfmetaedit`
  finds nothing), so it was not used.

Result of the run: `RESULT ok=693 fail=0 xfail=36`, log in
`build-interop/check.log`.

## Results

| Check | Spec | Readers (measured) | Verdict |
|---|---|---|---|
| RIFF/WAVE, RIFF size = file - 8, chunks tile the file | RIFF | own walk; ffprobe, soxi, libsndfile, mediainfo all open it | pass, 36/36 |
| Chunk order `bext`, `fmt `, `data` | EBU Tech 3285 v2 §2.1 | own walk | pass |
| 24-bit PCM, 48 kHz, mono | §A2 | ffprobe `pcm_s24le`/24, soxi `Signed Integer PCM`/24, libsndfile | pass |
| Length = data chunk / block align | | ffprobe `duration_ts`, soxi `-s`, libsndfile `Frames`: 98816 each | pass |
| `bext` is 602 bytes, reserved bytes zero | §2.3 | own walk; mediainfo `bext_Version 0` | pass |
| TimeReference, take 1 | §2.3 | ffprobe, libsndfile, mediainfo: 0 | pass |
| TimeReference, take 2 = length of take 1 | §2.3 | all three: 98816 = take 1's 98816 frames, delta 0 | pass |
| Same TimeReference on every track of a take | | 18/18 per take | pass |
| Originator `Sonora Engine` | §2.3 | ffprobe (as `encoded_by`), libsndfile | pass |
| OriginationDate / OriginationTime | §2.3: "shall be" 10 and 8 ASCII characters | all zero bytes; mediainfo `Encoded_Date: " "`, libsndfile prints blank | **defect**, reported as `xfail` |
| iXML | not required by Tech 3285 | we do not write one | n/a |
| Position on a DAW timeline | | Ardour 8.12 opens all 36 at their TimeReference (0 and 98816) | pass |

A negative control (one take-2 file's TimeReference patched to 12345) was
read as 12345 by all three readers and failed the run, so the checks can fail.

## How each reader gets the TimeReference

- **ffmpeg** stores it as metadata `time_reference` and files the Originator
  under `encoded_by` (`libavformat/wavdec.c`, `wav_parse_bext_tag` and
  `wav_metadata_conv`, release/7.1). Empty bext strings are not stored at all.
  It never uses TimeReference for `start_time`, so ffmpeg is a reader here,
  not a timeline.
- **MediaInfo** reports it as `Delay` in seconds with
  `Delay_Source: Container (bext)`.
- **Ardour** reads it through libsndfile (`SFC_GET_BROADCAST_INFO`):
  `SndFileSource::open()` sets the source's natural position from it
  (`libs/ardour/sndfilesource.cc` 438-450, tag 8.12), and import "at file
  timestamp" places the region at that natural position
  (`gtk2_ardour/editor_audio_import.cc` 979-990). The headless `ardour8-lua`
  host has no import call (no import in `libs/ardour/luabindings.cc`, 8.12),
  so the test declares the files as external sources of a fresh session and
  reads `Source:natural_position()` back. That is the value the import would
  use, computed by Ardour's own code; it is not the import dialog itself.

## Findings

1. **Engine defect: OriginationDate and OriginationTime are left empty.**
   `common/audiofile/src/wav_writer.cpp:83-92` zero-fills the 602 bytes and
   writes only Originator, TimeReference and Version. EBU Tech 3285 v2 §2.3:
   OriginationDate is "10 ASCII characters containing the date of creation of
   the audio sequence", year-month-day, separator recommended from `- _ : .`
   or space; OriginationTime is "8 ASCII characters containing the time of
   creation", hour-minute-second. Readers tolerate it (none failed to open the
   file), but every DAW shows a blank creation date. In BACKLOG. The checker
   validates the format automatically once the writer fills them; then
   remove the `xfail` branch in `tools/bwf-interop.py`.
2. **Ardour treats TimeReference 0 as "no timestamp".**
   `editor_audio_import.cc:989`: if the natural position is 0, the region goes
   to the edit point, or after the previous region on a single track. Our
   first take of every session has TimeReference 0, because the playhead
   starts at 0 and there is no locate yet. So in Ardour, take 1 lands at 0 only
   if the edit point is at 0. This is not a spec violation. A non-zero
   session origin would avoid it. That is a design decision, so it is in
   BACKLOG as an open question, not a defect.
3. **TimeReference is session-relative, not "since midnight".** §2.3 defines
   it as "the first sample count since midnight". We write the playhead. The
   two agree when the session origin is 00:00:00:00, which is how every
   reader above uses it. Worth stating in the session-format spec.
4. **The 4 GB limit is stated, but the writer does not enforce it.**
   `planning/BACKLOG.md` (Known gaps) and `brain/specs/bwf-broadcast-wave.md`
   both state the RF64 limit: 2^32 bytes / 144 000 B/s is about 8 h 17 min per
   mono 24-bit 48 kHz file, whatever the track count. But
   `wav_writer.cpp:144` and `:147` `static_cast<uint32_t>` the sizes without a
   check. A longer take would write a wrapped header, so readers would see a
   short or garbled file instead of an error. Not exercised (an 8-hour take),
   only read from the code. Added to the RF64 backlog item.
5. **Latent: no RIFF pad byte.** RIFF requires an odd-sized chunk to be
   followed by a zero pad byte that `ckSize` does not count (Microsoft/IBM,
   *Multimedia Programming Interface and Data Specifications 1.0*, 1991, RIFF
   chunk definition; copy at
   https://ftp.nic.funet.fi/pub/sci/audio/audiofileformats/WAV-format, not
   checked against the original). A mono 24-bit file has an odd `data` size when the
   frame count is odd. `close()` (`wav_writer.cpp:138-153`) never pads.
   Unreachable today, because PipeWire periods are 64 or 128 frames, so every
   take is even. Noted in the same backlog item.

## Open

- 32-bit float takes (`--float`) were not tested. The writer gives them a
  16-byte `fmt ` with format tag 3 and no `fact` chunk
  (`wav_writer.cpp:95-102`). Tech 3285 §2.1 requires `fact` only for MPEG.
  Microsoft's WAVE documents were not checked here for what they say about
  non-PCM `fmt ` sizes, and no reader was measured on these files.
- 24-bit PCM is written as `WAVE_FORMAT_PCM`, not `WAVE_FORMAT_EXTENSIBLE`.
  Tech 3285 §A2 allows that, and every reader above accepted it.
- No proprietary DAW was tried: Pro Tools, Logic, Reaper, Cubase. The claim is
  "Ardour 8.12 places the files at their TimeReference". It is not "every
  DAW does".
- Not run on the target hardware. The file format does not depend on the
  interface, but the take length does depend on the period, and real
  hardware periods were not seen here.

## Running it

    tools/test-bwf-interop.sh      # verdict lines; logs in build-interop/

Measured on the host above: 1 min 51 s with the reader image built cold,
mostly installing Ardour (about 830 MB), and 32 s with the images cached.
That is cheap enough for CI, so CI runs it in the `interop` job alongside
`record`. A CI runner always builds cold, and the job's time there has not
been measured yet.
