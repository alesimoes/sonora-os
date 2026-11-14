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
- Licence note: still unconfirmed by the owner (see Unverified); libptformat
  stays an optional, off-by-default, separately linked adapter until then.

## Alternatives rejected

- Our own `.ptx` parser: reverse-engineering we would own (ADR-0004).
- Linking libptformat into the engine: GPL code and hostile input in the
  real-time process.
- `.ptx` export: no published basis for an encoder; outside ADR-0004.
- AAF only: kept as the documented fallback, not a replacement.

## Unverified

- Licence fit of libptformat (GPL) with how we distribute; kept in a separate
  process. **Owner to confirm.**
- Debian packaging of libptformat.
- Which Pro Tools versions it reads.
- Availability of `.ptx` fixtures we may redistribute. **Still open:** no
  free-licence `.ptx` fixture is in the tree; needs a human to source or
  create one.
- That its output maps onto `PtxDocument` without loss beyond the list above.
- **libptformat-ON host:** the reader has not been built and run on a host
  with `-DSONORA_WITH_LIBPTFORMAT=ON`. Only the OFF build is exercised; the
  ON path needs the library installed and a person or hardware to run it.
