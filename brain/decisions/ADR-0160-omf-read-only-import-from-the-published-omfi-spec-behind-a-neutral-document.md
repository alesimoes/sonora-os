# ADR-0160 - OMF read-only import from the published OMFI spec behind a neutral document

## Context

Sessions from older Avid-family tools still travel as OMF (OMFI 1.0 and 2.0),
an interchange format Avid published. AAF (ADR-0154) is its successor and
covers newer sessions, but legacy projects exist only as OMF. ADR-0004 limits
us to published formats; the OMFI specification is one. Parsing a file
written by other software is hostile input (ADR-0007 section 4.5).

## Decision

1. OMF is read by our own small parser, written from the published
   specification only. No third-party code, no disassembly, no copying.
2. The parser lives in `interchange/omf/` behind `OmfReader`, which fills a
   neutral `OmfDocument` (sample rate, media sources, tracks, clips with
   source, start, source offset, length, gain, mute, fades, and a list of
   unsupported items). `OmfImporter : Importer` maps the document to a
   session. It takes an `OmfReader*`; null means `can_read` is false.
3. It runs only inside `sonora-convert`, never in the engine, so a hostile
   file can crash a converter and not a session.
4. The reader bounds-checks every read, limits file and object sizes, and
   caps the depth of nested objects.
5. Embedded media (AIFC/WAVE) is extracted into the session folder. Missing
   external media is an error that names the file.
6. Effects, automation, MIDI and unsupported versions: dropped into
   `ConversionReport.dropped`, or, for an unsupported version, a failure that
   names the version.
7. Import only. There is no OMF export; interchange out goes through AAF.

## Rationale

The format is small and published, so a bounded parser we own is cheaper to
audit than a dependency, and the neutral document keeps the mapper testable
with a fake reader (the shape ADR-0154 already uses).

## Consequences

- A new module `sonora_omf`; its `add_subdirectory` in `interchange/` lands
  after aaf_import merges, so until then the micro build does not reach it.
- This contract adds the headers, a fake reader and one test; the parser and
  the mapper are later slices.

## Alternatives rejected

- AAF only: remains the documented fallback, but OMF is how legacy sessions
  exist.
- Linking a third-party OMF library into the engine: puts hostile-input
  parsing in the real-time process and adds a dependency.
- OMF export: legacy format, no demand, AAF covers it.

## Unverified

- The specification's availability and redistribution terms (researcher to
  confirm).
- Real-world file fixtures; none have been gathered.
- Which OMF variants other DAWs actually wrote, and whether they follow the
  specification.
- That the size and depth limits are adequate for real sessions.

## Amendment 2026-11-13: wired into sonora-convert

`sonora-convert import x.omf` and `omf-import` now run the importer
(`interchange/src/convert.cpp`), copying media into `Audio Files/` through the
same essence lookup as AAF. `interchange/tests/test_omf_convert.cpp` covers a
synthetic round trip and truncated or missing files (exit 1, message). Real-world
OMF files remain unverified, as listed above.
