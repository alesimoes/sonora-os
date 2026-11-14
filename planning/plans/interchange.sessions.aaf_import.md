# Plan: interchange.sessions.aaf_import - AAF import via libaaf (closure)

Status: approved (closure record; no new code)
Written by: eng-47 (docs close-out)   Dispatched by: lead   Architect sign-off: pending
Scope: documentation only. No source, test or build change.

## 1. Goal and user experience
Close out the AAF import item as far as code can take it. The operator can
import an AAF file through `sonora-convert` once a libaaf-ON build exists on a
host. Until then the import path is declared, mapped and tested with a fake
reader, and the rest is recorded as unverified.

Done on main:
- Contract: `AafDocument`, `AafReader`, `AafImporter` declaration and its error
  paths (ADR-0154 decisions 1-3, slice 1; PR #677 merge of the contract slice).
- Mapper: rational edit-rate conversion, `ConversionReport.approximated` and
  `dropped`, generated UMID reporting, media copy into the session folder
  (ADR-0154 decisions 4-7; `aaf_map.cpp` plus `aaf_map_util.{h,cpp}` after
  #719).
- libaaf seam: `SONORA_WITH_LIBAAF` option, default OFF, never fetched
  (`interchange/CMakeLists.txt`); `aaf_libaaf_reader.cpp` behind it.
- Fixture test: `test_aaf_fixture` skips visibly (exit 77) while
  `tests/fixtures/aaf/minimal.aaf` is absent (#719).

Out of scope for this item: AAF export (owner decision 2026-11-13, import
only); Logic `.logicx` import (no published specification, ADR-0004).

Acceptance:
- `grep -n "Unverified" brain/decisions/ADR-0154-*.md` lists the libaaf
  licence, build and fixture items (ADR-0154 Unverified 4 and 5, and the
  2026-11-13 amendment: licence not confirmed, ON path not compiled, fixture
  not committed).

## 2. Design
N/A - no screen or faceplate; the importer has no UI in this item.

## 3. Contracts and integrations
Contract merged (#677). ADR-0154 is the decision record; its amendment
(2026-11-13) is on main. No new ADR number is reserved by this closure.

## 4. DAW core
N/A - no engine or real-time change. The importer runs only in
`sonora-convert` (ADR-0154 decision 2), never in `Session::process()`.

## 5. Plugins, DSP, host
N/A - no plugin or DSP change.

## 6. UI
N/A - no UI change.

## 7. Tests
N/A for this PR - no test added or changed. The existing tests stay as they are:
the fake-reader mapper tests run in the micro build; `test_aaf_libaaf` and
`test_aaf_fixture` run only when `SONORA_WITH_LIBAAF=ON`.

## 8. QA
N/A - nothing to exercise end to end until a libaaf-ON host and a real AAF
file exist (see "Still open" below).

## 9. Quality checks
N/A - docs only. `tools/pre-pr.sh` is run before push.

## 10. Code standards
N/A - no code.

## 11. Dependencies and order
Closed by this plan. The remaining work depends on hardware or a host, not on
code:

| Slice | Agent | Difficulty | Risk | Tier | Status |
|---|---|---|---|---|---|
| libaaf-ON build on a host, licence read from LICENSE file | owner or hardware host | S | low | - | open, unverified |
| Real AAF fixture committed, `test_aaf_fixture` passing | owner or host with a DAW | S | low | - | open, unverified |
| Fade and gain round trip against another DAW | owner or host | M | med | - | open, unverified |

Integrator: N/A. Single PR title:
`[docs] interchange.sessions.aaf_import: close-out, libaaf build, fixture and licence stay unverified`

## 12. Risks and open questions
| Question | Recommended default |
|---|---|
| Is libaaf LGPL-2.1+ or another GPL-family licence? | Treat as unconfirmed; the importer links it only in `sonora-convert`. Read LICENSE on a host before any ON build ships. |
| Does libaaf expose every `AafDocument` field (gain, markers, timecode)? | Unverified; checked only against a real file. |
| Fade representation: which one libaaf reads and others emit | Unverified; keep `SourceClip` optional fades until a real file says otherwise. |
| Video-rate rounding acceptable to users | Unverified; rounding is reported in `ConversionReport.approximated`. |
| Embedded essence through `AafMob.media_path` | Unverified; assumed sufficient. |

## 13. Done =
- [x] contract PR merged (#677)
- [x] mapper and libaaf seam merged (#677, mapper split and fixture test in #719)
- [x] ADR-0154 lists the libaaf licence, build and fixture as unverified (Unverified 4-5, amendment 2026-11-13)
- [ ] libaaf-ON build compiled on a host - unverified, needs a host
- [ ] real AAF fixture committed and `test_aaf_fixture` passing - unverified, needs a fixture (owner or a DAW)
- [ ] libaaf licence read from its LICENSE file - unverified, needs the file
- [x] `tools/pre-pr.sh` clean for this docs-only PR - run before push

Note for the coordinator: the BACKLOG line stays open ("partial"). Ticking it
needs the three unverified items above, which are not closable from code.
