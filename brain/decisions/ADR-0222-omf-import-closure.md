# ADR-0222 - Close out OMF import: synthetic fixtures now, real-world files later

## Summary

OMF read-only import (ADR-0160) is implemented on main: the contract, the
reader (`interchange/omf/src/omf_reader.cpp`, `omf_container.cpp`), the mapper
(`omf_map.cpp`) and the `sonora-convert omf-import` command (#679, #713).
This ADR closes the item without code. The item is done against the tests and
hand-built fixtures it has today. Real-world OMF files are not part of the
closure: they need files from a human, and until they arrive ADR-0160's
Unverified list stays as written.

## Context

- ADR-0160 decided a parser written from the published OMFI specification,
  behind `OmfReader`, with `OmfImporter` mapping into a native session.
- The reader and mapper are tested against hand-built fixtures only
  (`interchange/omf/tests/`). The `interchange/omf/README.md` says so.
- No real OMF from Pro Tools, Avid or another DAW has been opened. Whether
  those writers follow the specification is an open question (ADR-0160,
  Unverified).

## Decision

1. The OMF import item is closed as "implemented, verified on synthetic
   fixtures". No source change is part of the closure.
2. ADR-0160's `## Unverified` section is not edited. It keeps:
   - the specification's availability and redistribution terms (researcher to
     confirm);
   - real-world file fixtures, none gathered;
   - which OMF variants other DAWs wrote, and whether they follow the spec;
   - whether the size and depth limits suit real sessions.
   Those items remain unverified until a file exists to test them against.
3. Real-world OMF files are requested from the owner as test material. They
   are added as fixtures only with their licence and provenance recorded next
   to them. Nothing is fetched from a proprietary product or reverse-engineered
   to make one (ADR-0004).
4. When a real file arrives, the follow-up is: run `sonora-convert omf-import`
   on it, record what is dropped in the conversion report, and fix the reader
   against the file. That work is a new item; it does not reopen this one.

## Rationale

The code that can be verified without outside files is merged and tested. The
remaining gap is evidence, not code, so keeping the item open would hold a
finished feature on a human action with no deadline. Leaving ADR-0160's
Unverified list untouched keeps the gap visible where the decision lives.

## Consequences

- OMF import is shown as done, with the real-world gap named, not as verified
  against Pro Tools or Avid output.
- No OMF export is planned; AAF remains the way out (ADR-0160 decision 7).
- `interchange/omf/README.md` still says the wire-up is "a follow-up after
  `aaf_import` merges". That is stale: #713 wired `omf-import` into
  `sonora-convert`. Fixing the README is a follow-up, not part of this
  docs-only closure, because it is outside the claimed files.

## Alternatives rejected

- Keep the item open until real files arrive: holds a finished feature on an
  external dependency with no date.
- Edit ADR-0160 to mark its Unverified items closed: would hide the gap.
- Gather OMF samples by any means that involve a proprietary product's
  internals: refused under ADR-0004.

## Unverified

- That the closed item matches real-world writers' output (no real files).
- The `interchange/omf/README.md` wire-up sentence is stale (noted above; not
  edited here).
