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
