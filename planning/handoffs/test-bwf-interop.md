# test/bwf-interop

- **Goal:** verify Phase 1's "takes open in another DAW at the right
  timecode" with third-party readers (progress item
  `interchange.audio.bwf_other_daw`).
- **State:** in review. The engine was not changed.
- **Commits:** see `git log origin/main..test/bwf-interop`.

## Verify
    tools/test-bwf-interop.sh   # expected: RESULT ok=693 fail=0 xfail=36, TEST PASS

## Done
- Two takes x 18 tracks read back by ffprobe, soxi, libsndfile, mediainfo
  and Ardour 8.12: RIFF/WAVE, 24-bit, 48 kHz, length, TimeReference 0 and
  98816 (= take 1 length), Originator. Evidence: `build-interop/check.log`.
- Negative control: a TimeReference patched to 12345 fails the run.
- CI job `interop` added. shellcheck 0.9.0 is clean.

## Open
- OriginationDate/Time are empty (`common/audiofile/src/wav_writer.cpp:83-92`):
  BACKLOG Phase 1, printed as `xfail`.
- Session origin 0 versus Ardour's "0 = no timestamp", the writer wrapping
  past 4 GB, and the missing pad byte: BACKLOG "Known gaps".
- The item stays `doing` until the date defect is fixed and this merges.

## Do not redo
- BWF MetaEdit is not in trixie.
- Headless Ardour has no import call in Lua. Declare the files as session
  sources instead (`ardour_positions()` in `tools/bwf-interop.py`).
- Ardour pulls in libjack-jackd2-0, so it must not go in the runtime image.

## Pointers
- `brain/research/bwf-interop.md`, `brain/specs/bwf-broadcast-wave.md`
