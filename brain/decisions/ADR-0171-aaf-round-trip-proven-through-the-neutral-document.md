# ADR-0171: AAF round trip is proven through the neutral document

Status: accepted (2026-11-13)

## Summary

The exit criterion "export then import gives the same session" is tested at
the `AafDocument` seam, not through a file, because libaaf has no writer we
can run here and the owner decided (2026-11-13) on import only.

## Context

ADR-0154 puts the importer behind `AafReader`; ADR-0161 puts the exporter
behind `AafWriter`. Both speak `AafDocument`. A file round trip through libaaf
needs a libaaf writer, which libaaf lacks, and the owner decided there is no
AAF export for users: import only.

## Decision

`interchange/aaf_export/tests/test_aaf_roundtrip.cpp` (ctest `aaf_roundtrip`,
label `interchange`) maps a Snapshot to a document with the exporter's `map_to_aaf` (`AafExporter::write` also copies source audio from disk, so the test stops before that) and gives it to a
`FakeAafReader`, imports it
with `AafImporter`, and compares. Edits that must survive: region position,
length, source offset, clip gain, fade lengths, a crossfade, a gap, markers,
sample rate (rational rates stay exact). What AAF cannot carry (track
automation) must appear in `ConversionReport.dropped` on export, and the test
pins that it does not come back on import.

## Rationale

The mapping logic is where round-trip bugs live; the file layer is libaaf's.
The seam test runs everywhere, with no external dependency and no wall clock.

## Consequences

- The backlog wording "automation intact" is not achievable with AAF, which
  carries no Sonora automation. The test pins the loss as documented
  behaviour; the owner decides whether the wording changes.
- A mapping regression on either side fails this test.

## Alternatives rejected

- A libaaf file round trip: needs a writer libaaf lacks.
- Exporting automation as vendor-private AAF data: no other DAW reads it.

## Unverified

- That a real DAW opens the exported structure and shows the same edits.
- That libaaf serialises/parses the document without changing it (the
  document-to-file layer is outside this test).
- Fade shapes: the format default is used; only lengths are compared.
