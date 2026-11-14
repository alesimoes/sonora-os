# OMF import (read-only)

`sonora-convert` can read OMF interchange files into a native session
(ADR-0160). There is no OMF export; use AAF (`interchange/aaf`) to go the
other way.

- Our own parser, written from the published OMFI specification, no
  third-party code. `OmfReader` (`src/omf_reader.cpp`) walks the container
  (`src/omf_container.cpp`): header, object index, composition mob, slots,
  source clips and media. Every read is bounds-checked; a truncated or
  corrupt file returns an error naming the problem and never loops.
- Embedded WAVE media is extracted into the session folder and checked with
  `WavReader`. Embedded AIFF/AIFC is extracted but not decoded and is listed
  in the report.
- Wire-up: `sonora-convert import x.omf out.sonora` (or `omf-import`) calls
  `make_omf_importer` on the output folder, then copies the media into
  `Audio Files/` the way `aaf-import` does; a missing file is an error naming
  it. Test: `ctest -R omf_convert` (synthetic fixture, truncations).

## What is dropped

Effects, automation, pan/volume curves, transitions beyond fades, MIDI and
any object class the reader does not model: each class is named in
`OmfDocument::unsupported` and shown in the conversion report.

## If an OMF will not import

Export AAF from the source application (nearly all of them write it) and use
the AAF importer, or bounce stems.

## Unverified

The container layout is implemented from our reading of the specification and
tested on hand-built fixtures only. No real-world OMF file (Pro Tools, Avid,
Logic) has been opened yet; expect to adjust the reader when one is.
