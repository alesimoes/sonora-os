# ADR-0154 - AAF import via libaaf behind a neutral document

- Status: Proposed
- Date: 2026-11-12
- Item: `interchange.sessions.aaf_import` (contract slice)
- Relates to: ADR-0004, ADR-0007 §4.5; `brain/specs/session-format.md` "Interchange constraints"

## Context

Phase 4 needs AAF import. libaaf reads AAF structured storage; its types and ABI
are third-party, and a hostile file must not be able to end a session.

## Decision

1. libaaf is confined to `interchange/` behind the abstract `AafReader`, which
   fills a neutral `AafDocument` (plain structs: mobs, slots with an exact
   rational edit rate, `SourceClip`, `Filler`, `Transition`, timecode, markers).
2. It runs only in `sonora-convert` (ADR-0007 §4.5), never in the engine.
3. `AafImporter` (an `Importer`) takes an `AafReader*` by injection; with null,
   `can_read` is false. Mapping follows the table in `session-format.md`
   "Interchange constraints".
4. A non-native edit rate is converted by exact rational arithmetic; any
   rounding is listed in `ConversionReport.approximated`.
5. Unmappable content (automation, effects other than gain, inactive playlists,
   video) goes in `ConversionReport.dropped`.
6. A source without a UMID gets a generated one, and the report says so.
7. Media is copied into the session folder, never referenced outside it.
8. This slice adds the document, the reader interface, the importer declaration
   with its error paths, and a header-only `FakeAafReader`; the mapping and the
   libaaf reader follow in later slices.

## Rationale

A neutral document makes the mapping testable without libaaf and keeps its ABI
out of the rest of the tree. Process isolation makes a parser crash cost one
conversion.

## Consequences

Two layers to maintain (reader, mapper). The document must grow when a real
file shows a construct it cannot hold.

## Alternatives rejected

- Parsing AAF structured storage ourselves: large, error-prone, no gain.
- Linking libaaf into the engine: ADR-0007, a crash kills a session.
- Mapping straight from libaaf types: untestable without the library, and ties
  us to its ABI.

## Unverified

1. The fade representation (`SourceClip` optional fades) is an assumption: which
   one libaaf reads, and what other DAWs emit, is not checked.
2. That libaaf exposes every field of `AafDocument` (gain via `OperationGroup`,
   markers, timecode) is assumed.
3. That exact rational conversion plus reported rounding is acceptable to users
   for video-rate files is assumed.
4. libaaf's licence compatibility and build on our toolchain are not checked.
5. No real-world AAF file (from any DAW) has been opened; the document shape is
   from the spec table only.
6. Whether `AafMob.media_path` suffices for embedded essence is assumed.

## Amendment 2026-11-13: libaaf provenance, packaging, fixture

- **What it is.** LibAAF (Adrien Gesta-Fline, github.com/agfline/LibAAF), a C
  library that only reads AAF; it reports v1.0 and Ardour vendors it
  (commit "Update libaaf to v1.0-1-gdef35bf", 2024-03-10), so it is a
  published, freely licensed route as ADR-0004 requires.
- **Licence: not confirmed.** The CMake comment says LGPL-2.1+; the sources
  I could reach (Ardour's copy carries an FSF header) point to a GPL family
  licence but do not state which. The LICENSE file was not read. Until it is,
  treat "LGPL-2.1+" as an assumption; either way the importer links it only in
  the short-lived `sonora-convert` process, never the engine.
- **Debian packaging.** A packages.debian.org name search and a
  sources.debian.org search for `libaaf` both returned no result: there is no
  Debian package. libaaf is therefore built from source (or vendored like
  Ardour does) and `SONORA_WITH_LIBAAF` stays OFF by default; the image does
  not get it from apt.
- **Compile status.** `aaf_libaaf_reader.cpp` has NOT been compiled: libaaf is
  absent here, so the ON path (that file, `test_aaf_libaaf`, `test_aaf_fixture`)
  is unverified. Only the OFF path and the mapper were built.
- **Mapper split.** `aaf_map.cpp` (524 lines) is now `aaf_map.cpp` plus
  `aaf_map_util.{h,cpp}` (text/UMID/path helpers); behaviour unchanged.
- **Fixture.** `tests/fixtures/aaf/minimal.aaf` is not committed (cannot be
  authored without libaaf or a DAW; a hand-made binary would be unverifiable).
  `test_aaf_fixture` runs when libaaf is on and reports SKIPPED (exit 77)
  while the file is missing.
