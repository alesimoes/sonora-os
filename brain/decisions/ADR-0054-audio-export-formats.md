# ADR-0054 — Audio export: our own writers, FLAC included, conversion from a float render

- Status: Accepted (recommended defaults, taken by the implementer with no
  owner available, 2026-11-04)
- Date: 2026-11-04
- Progress item: `interchange.audio.export`

## Summary

*Navigation only; the sections below are the decision.*

- §1: a bounce renders to a float32 file beside the kept one; `finish()`
  converts it once, with the normalise gain, rate, depth and dither the
  operator chose (`common/audiofile/audio_export`).
- §2: the FLAC encoder is ours, not libFLAC.
- §3: dither is TPDF (+-1 LSB) or the same with a second-order error
  feedback; Auto means TPDF at 16 bits and nothing above.
- §4: rate conversion is a Kaiser-windowed sinc, offline, stretched by the
  ratio when the rate falls.
- §5: the wire (protocol 61): `file`, `rate`, `dither` and `int16` on
  `mix.bounce`.

## §1 One road for every file

Loudness normalise needs the whole file measured before its gain is known,
so the mix is rendered once, in float, and every output is a conversion of
that render: BWF, WAV, AIFF or FLAC, 16, 24 or 32-bit float (FLAC and AIFF
hold no float), the session's rate or another. The converter reads, applies
the gain, resamples, quantises and writes in blocks of 4096 frames on the
bounce's worker; nothing here is reachable from `Session::process()`. The
kept name is reserved with an exclusive create before the render starts, so
a second bounce never takes it whatever the extension, and a cancel or a
failure removes both the name and the render. A file over 4 GiB is refused
(RF64 is a separate item).

BWF carries bext version 2: Description, Originator, the date and time,
TimeReference (the start of the range at the file's rate) and LoudnessValue
(the integrated figure of the file as written, in hundredths of LU); the
other loudness fields read 0x7FFF, "not given".

## §2 Why not libFLAC

libFLAC is BSD and in Debian, but it would be one more package in every
image that builds the engine (build, UI, RT-sanitiser, plugins) and a
vendored copy for the MSVC job, to encode a format whose reference decoder
checks every frame's CRC and the MD5 of the samples. The encoder here is
the subset the format makes simple: fixed predictors of order 0 to 4,
partitioned Rice coding, constant and verbatim subframes, independent
channels, 4096-frame blocks. It gives up the last 5 to 10 % that LPC and
stereo decorrelation would take. The test decodes with an independent
decoder written from the specification, and `tools/test-export-interop.sh`
runs `flac -t`, libsndfile, sox and ffmpeg over the files. If a later item
wants LPC, it is a change inside `audio_export.cpp`.

## §3 Dither

TPDF (the difference of two uniforms, +-1 LSB) makes the quantisation error
independent of the signal; the test pins its mean, its variance of 1/4 LSB^2
and its absence of memory. Shaped adds a second-order error feedback with
NTF `1 - z^-1 + 0.5 z^-2`: less noise below about 10 kHz at 48 kHz, more in
the top octave, 2.25 times the total power (also pinned). 32-bit float is
never dithered. Auto is TPDF at 16 bits and none at 24, where it would sit
below any converter's noise.

## §4 Rate conversion

Sinc of 32 zero crossings, Kaiser beta 10, a 512-point table per crossing
with linear interpolation, cutoff at the lower of the two Nyquists. Stop
band about 100 dB; the test requires a tone above the new Nyquist to fall
below 0.001 and a 1 kHz tone to keep 0.1 % of its level. The length is
`round(frames * out / in)`.

## §5 Wire

`mix.bounce` takes `file=bwf|wav|aiff|flac`, `format=int16|int24|float32`,
`rate=<Hz>` (8000 to 384000, absent: the session's) and `dither=auto|none|
tpdf|shaped`, all optional. The protocol is 61: an older engine would
refuse the new words one by one, and a refusal at the connection is clearer
than a 24-bit BWF where a FLAC was asked for. FLAC or AIFF with float32 is
refused when parsed and again by the engine.
