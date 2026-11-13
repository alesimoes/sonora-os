# ADR-0156 — .ptx read-only import via libptformat, behind a neutral document

- Status: Proposed
- Date: 2026-11-12

## Context

Engineers arrive with Pro Tools sessions. ADR-0004 allows interoperability
through published formats and reverse-engineering results already published
under a free licence. `libptformat` decodes `.ptx`, ships under the GPL inside
Ardour, and was not produced by disassembling Pro Tools by us.

## Decision

- `.ptx` is read through libptformat, confined to `interchange/ptx/` behind
  the `PtxReader` interface and a plain `PtxDocument` (sample rate, audio
  sources, tracks, regions with source, start, offset, length, gain, mute,
  and a list of unsupported features). `PtxImporter` converts the document to
  a session; it is an `Importer` and read-only: there is no `.ptx` export.
- It runs only inside the `sonora-convert` process (ADR-0007 4.5), so a
  hostile file or the GPL code never reaches the engine or the app.
- Supported versions are those libptformat decodes; any other fails loudly
  naming the version.
- Plugins, automation, MIDI and inactive playlists go to
  `ConversionReport.dropped`.
- Audio files are copied into the session folder; a missing one is an error
  naming it.
- A null reader means `can_read` is false (a build without libptformat).

## Rationale

A neutral document keeps the decoder replaceable and testable with a fake,
and keeps third-party code out of every process that matters during a take.

## Consequences

- Wired: `add_subdirectory(ptx)` is in `interchange/CMakeLists.txt`;
  `PtxImporter::read` maps through `map_ptx_document`; `sonora-convert` has
  `ptx-import` (and `import` by `.ptx`), copying media into `Audio Files/`.
  Without `SONORA_WITH_LIBPTFORMAT` the command exits 1 with "no .ptx reader
  in this build". Decoding a real `.ptx` still needs a redistributable fixture.
- Licence: accepted by the owner on 2026-11-13 (see the amendment below).
  libptformat stays an optional, off-by-default adapter, found on the system
  and never fetched at configure time.

## Alternatives rejected

- Our own `.ptx` parser: reverse-engineering we would own (ADR-0004).
- Linking libptformat into the engine: GPL code and hostile input in the
  real-time process.
- `.ptx` export: no published basis for an encoder; outside ADR-0004.
- AAF only: kept as the documented fallback, not a replacement.

## Unverified

- Debian packaging of libptformat (none exists; we build it ourselves).
- Which Pro Tools versions it reads.
- Availability of `.ptx` fixtures we may redistribute.
- That its output maps onto `PtxDocument` without loss beyond the list above.

## Amendment 2026-11-13 — licence accepted, packaging obligations

- **Owner ruling, 2026-11-13: the libptformat licence is accepted** for how
  we distribute. The licence stated at the pinned revision is
  LGPL-2.1-or-later (`COPYING.LESSER`, header of `ptformat.cc`), a
  GPL-family licence compatible with our GPL-3.0-or-later; this ADR's
  earlier "GPL" meant that family.
- Built from source, never fetched by CMake: `tools/Dockerfile.ptformat`
  builds upstream https://github.com/zamaudio/ptformat at commit
  `af15ee90825f59b84b9977591862cce96bb1d781` (checked after the fetch) into
  `libptformat.so.0` with a `ptformat.pc`; `tools/build-ptformat.sh`
  configures with `-DSONORA_WITH_LIBPTFORMAT=ON` and builds and runs
  `test_ptx_libptformat`. A bump changes the commit there and in
  `licences/libptformat.md` together.
- Linked only into the `sonora-convert` process, as a shared library so it
  can be replaced; never into the engine or the app.
- Packaging obligations for any image that ships it:
  - the licence file `licences/libptformat.md` and the licence text
    (`COPYING.LESSER`) shipped under `/usr/share/doc/libptformat/`;
  - the corresponding source in the image (the archive of the pinned tree
    the binary was built from), plus the written offer of source in
    `licences/libptformat.md`.
