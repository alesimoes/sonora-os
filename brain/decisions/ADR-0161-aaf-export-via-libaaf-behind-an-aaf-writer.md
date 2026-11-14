# ADR-0161 - AAF export via libaaf behind an AafWriter

- Status: Proposed
- Date: 2026-11-12
- Item: `interchange.sessions.aaf_export` (contract slice)
- Relates to: ADR-0004, ADR-0007 §4.5, ADR-0154 (AafDocument, AafReader)

## Context

Interchange with other DAWs goes through AAF (ADR-0004: published formats
only; there is no `.logicx` import). ADR-0154 added a neutral `AafDocument`
and an `AafReader` seam for import. Export needs the mirror image. libaaf's
types and ABI are third-party and cannot be exercised in a test without the
library.

## Decision

1. libaaf stays confined to `interchange/` behind the abstract `AafWriter`
   (`bool write(const AafDocument&, path, error)`), and runs only in
   `sonora-convert` (ADR-0007 §4.5), never in the engine.
2. `AafExporter` (an `Exporter`) maps a `model::Snapshot` to the neutral
   `AafDocument`, then hands it to the injected writer. With a null writer,
   or a session with no tracks, `write` fails with a clear error.
3. Mapping: tracks to slots; regions to SourceClips with exact rational edit
   rates; gaps to Filler; crossfades to Transition; gain, fades, timecode and
   markers carried across.
4. Essence is written as external WAV beside the `.aaf`. Embedding is a
   documented option; the default is external.
5. Unmappable content (automation, plugins, MIDI, buses, non-active takes)
   goes in `ConversionReport.dropped`; rounding in `approximated`.
6. Round trip with `aaf_import` must be lossless for everything the mapping
   table keeps.

Code lives in `interchange/aaf_export/` (library `sonora_aaf_export`, tests
labelled `interchange`); `AafDocument` is reused and not edited.

## Rationale

A neutral document keeps the mapping unit-testable with a fake writer and
keeps hostile or buggy library behaviour at one boundary, as in import.

## Consequences

- The mapper and the libaaf writer are separate slices.
- `add_subdirectory(aaf_export)` in `interchange/CMakeLists.txt` follows once
  aaf_import merges.
- A new test fake (`FakeAafWriter`) captures the document for assertions.

## Alternatives rejected

- Writing AAF structured storage ourselves: large, error-prone, no gain.
- Mapping Snapshot straight to libaaf types: untestable without the library.
- Embedding essence by default: huge files, slower, riskier for first
  versions.

## Unverified

Every item below is an assumption to be checked by the researcher:

- That libaaf supports writing, and that its write API is stable.
- That other DAWs open our files at all, and open them correctly.
- How fades are represented so other DAWs honour them.
- Whether external WAV essence is accepted by target DAWs, versus embedded.
- That the exact rational edit rates survive in libaaf's model.
- That the round trip is lossless for every kept mapping row.
