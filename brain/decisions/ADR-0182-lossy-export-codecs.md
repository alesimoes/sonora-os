# ADR-0182 - Lossy export: an encoder seam, Opus and Ogg Vorbis, no MP3 or AAC

- Status: Accepted (recommended defaults, taken by the implementer with no
  owner available, 2026-11-13)
- Date: 2026-11-13
- Item: `interchange.audio.export`
- Amends: ADR-0054 §1 (it lists the lossless files; this adds the compressed
  ones as one more conversion of the same float render)
- Relates to: ADR-0007 (modules), ADR-0008 (portable code), ADR-0119 (RF64)

## Summary

*Navigation only; the sections below are the decision.*

- §1: `common/audiofile/include/sonora/lossy_encoder.h` is the seam: an
  encoder (`open`, `write`, `finish`) behind a byte sink, and a registry of
  the codecs this build carries. The bounce converter drives it on the
  worker thread, never `Session::process()`.
- §2: Opus (libopus + libogg) and Ogg Vorbis (libvorbis + libogg), both
  BSD-3-Clause, are accepted. MP3 (LAME, LGPL) and AAC (patent pool, FDK
  licence) are rejected for the image; the registry refuses to register them.
- §3: a codec that is not in the build is refused when the bounce starts,
  with an error naming it. No fallback to another format.
- §4: no wire change in this ADR; protocol stays.

## Context

ADR-0054 ships BWF/WAV/RF64, AIFF and FLAC with dither and rate conversion.
The item's last open word is "compressed": a reference mix to send by mail
or a streaming proof. The format choice is a licence choice first, because
each codec is a library linked into a GPL-3.0-or-later engine and shipped
in a Debian-based image and, later, an MSVC build (ADR-0008).

## §1 The seam

Loudness normalise already forces one float render per bounce (ADR-0054
§1); a lossy file is one more conversion of it. The converter reads blocks
of the render, applies the gain and, where the codec needs it, the rate, and
hands interleaved float to `LossyEncoder::write`. `open(rate, channels,
bitrate)` comes first and `finish()` last (the Ogg end-of-stream page). Each
call returns false with a reason; after one the converter stops and removes
the file, as for the lossless writers.

The encoder writes through `LossyByteSink`, not a path, so the export's own
file (exclusive create, removal on failure) stays the converter's and a
test holds the bytes in memory. The registry maps `LossyCodec` to a factory;
it is filled once at startup and only read during a bounce. `start()`
checks the request (1 to 8 channels, a rate, 6 to 512 kb/s), creates the
encoder and opens it, so every refusal happens before the render.

Everything here allocates and may take seconds: worker thread only. The
header takes no platform header (ADR-0008 §1); the real encoders, which do
link a library, register from the module that builds them.

## §2 Licences

| Codec | Library | Licence | Image |
|---|---|---|---|
| Opus (`.opus`) | libopus + libogg | BSD-3-Clause; the Opus patent grants are royalty-free | accepted |
| Ogg Vorbis (`.ogg`) | libvorbis + libogg | BSD-3-Clause; no known patents asserted | accepted |
| MP3 (`.mp3`) | LAME | LGPL-2.0-or-later | rejected |
| AAC (`.m4a`) | FDK AAC (or any) | FDK AAC licence; AAC patent pool licence | rejected |

- Opus and Vorbis are permissive, packaged in Debian main, and build on
  MSVC from source without terms beyond keeping the notice. Opus is the
  better codec at every bitrate; Vorbis stays for players that predate it.
- MP3: the patents have expired, so the reason is the library. LAME's LGPL
  is compatible with our GPL, but it brings relinking and source-offer
  obligations to the static and MSVC builds that the permissive codecs do
  not, for a format Opus surpasses. Rejected for the image; a later ADR may
  revisit it if an operator need appears that Opus cannot meet.
- AAC: the encoder that sounds good (FDK AAC) carries a licence Debian keeps
  out of main and the FSF calls GPL-incompatible, and AAC encoders sit under
  an active patent pool licence. Rejected.

The record is in code too: `lossy_codec_info()` names library, licence and
verdict, and `LossyRegistry::add` refuses a rejected codec, so a build that
links one by mistake fails its own startup check instead of shipping it.

## §3 Refusal, never fallback

A build without libopus has no Opus factory. `start(Opus, ...)` then
returns null with "Opus export is not available in this build (libopus +
libogg not installed)"; the bounce does not begin and nothing is written.
Writing a WAV instead would hand the operator a file they did not ask for
under a name they may not check. The error is shown the way other bounce
refusals are (a held notice, not the status line).

## §4 Wire

None in this ADR. The values `file=opus` and `file=ogg` of the existing
`mix.bounce` `file` field (ADR-0054 §5) are added to the contract with the
encoders that use them; the protocol stays. An older engine refuses the
unknown word by name when it parses the argument ("bounce file is bwf, wav,
aiff or flac, not \"opus\""), which is the same named refusal as §3.

## Consequences

- Real encoders (libopus, libvorbis) are their own slice; each registers a
  factory and its build adds the package to the build images.
- `parse_lossy_codec` gives the contract and the UI one spelling of the
  words.
- MP3 or AAC later means a new ADR that supersedes §2 for that codec.
