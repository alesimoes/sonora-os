# ADR-0069: Capture cards and the capture manifest

Status: Accepted (dsp-engineer, 2026-11-07; recommended defaults, nothing here needs the owner).

Builds on: ADR-0011 §4.6 (captures and IRs we ship: the card, provenance, size bounds, no fallback) and §5 (names), ADR-0004 (no black-box cloning), ADR-0066 (`plugins/native/models/manifest.json`, owned by native.ai_analysis).

## Summary

Every amp capture and cabinet IR we ship has a **capture card**, `brain/models/<id>.md`, whose machine part is exactly one fenced ` ```card-json ` block; the prose around it is free. `sonora::ampcab::parse_card` reads the block, `validate_card` applies the policy and returns every refusal (it never throws), `check_capture_file` checks the file's size and SHA-256 against the card. Captures are pinned in their own manifest, `plugins/native/ampcab/captures/manifest.json`, with the field names of the models manifest so the two can merge later. A card is a precondition: no card, a missing card or a hash that differs fails the build check and disables that capture with a message, never a fallback.

## Context

ADR-0011 §4.6 requires a card for each capture and IR: the unit (type, serial, owned or rented, and for a rented unit that the terms allow recording), its settings, the rig and date, and for IRs the cabinet, speaker, microphone, position and room. It forbids third-party captures without a written licence, captures of a plugin, and brand or artist names. Those rules are only as good as the check that enforces them: prose cards cannot be checked by a build. The models manifest (ADR-0066) belongs to native.ai_analysis and is being written in parallel; the amp-and-cab track must not edit it.

## Decision

1. **Card file.** `brain/models/<id>.md`. Its machine part is ONE fenced block opened by a line that is exactly ` ```card-json ` (surrounding blanks and a CR allowed) and closed by a line ` ``` `. No block, an unclosed block or two blocks is a refusal. Everything outside the block is free prose (provenance notes, photos of the rig, listening notes).

2. **Fields** (all required unless stated; unknown keys are refused, since a misspelt `esr_heldout` must not read as "no ESR"):

   | Field | Type | Notes |
   |---|---|---|
   | `id` | string | `^[a-z0-9-]+$`, the card's file stem |
   | `name` | string | the shipped, user-facing name, named by its sound (§4.6) |
   | `kind` | `capture` \| `ir` | |
   | `source` | `hardware` \| `plugin` | only `hardware` passes; `plugin` is parsed so it is refused by name |
   | `unit` | `{type, serial, ownership: owned\|rented, rental_terms_allow_recording}` | the flag may be absent when owned, must be `true` when rented |
   | `settings` | object | control name to string, number or boolean |
   | `rig`, `date` | string | `date` is `YYYY-MM-DD` |
   | `third_party` | `null` or `{licence_path: string\|null}` | null or absent: our own work |
   | `ir` | `{cabinet, speaker, microphone, position, room}` | only when `kind` is `ir` |
   | `capture` | `{architecture, sample_rate, esr_held_out, train_commit}` | only when `kind` is `capture`; `esr_held_out` a number |
   | `file` | `{name, sha256, size}` | `size` in bytes |

   `name` is the one field beyond §4.6's list: the deny check needs the name a user sees, and §4.6 says captures are named by sound and circuit class.

3. **`validate_card` returns a list of refusals, never throws,** and refuses: a unit that is neither owned nor rented (a parse refusal), a rented unit without `rental_terms_allow_recording: true`, a `plugin` source, `third_party` without a `licence_path`, a `sha256` that is not 64 lower-case hex digits, a `size` above 25 MB (25,000,000 B), a capture without `esr_held_out` (or a negative or non-finite one), an `id` outside the pattern, a malformed date, empty provenance strings, and shipped names (`id`, `name`, `file.name`) that contain a term of `plugins/native/ampcab/tools/ampcab_name_deny.txt` (brands, model numbers, artists; ADR-0011 §4.6, §5). Matching compares lower-case ASCII alphanumerics only, so `ZX-900`, `zx 900` and `zx900` match alike. `unit.type` is not checked: it is provenance and must name the real unit. The deny list is passed in (`parse_name_deny` reads the file's text), so tests use their own.

4. **The capture manifest is its own file,** `plugins/native/ampcab/captures/manifest.json`: an array of `{id, version, sha256, size, card}`, `card` being the card's repository path. It is NOT `plugins/native/models/manifest.json`, which ADR-0066 gives to native.ai_analysis. The field names are the same, so the two merge into one later without rewriting entries. Capture files are not in git (§4.6); the manifest pins them.

5. **A card is a precondition.** The build check (`plugins/native/ampcab/cmake/cards.cmake`, included from the module's `CMakeLists.txt` when it exists, so later slices never edit that file) fails when a capture file has no card, or a manifest entry's card is missing, fails `parse_card` or `validate_card`, or its `sha256`/`size` differ from the card's or the file's. At load, the same mismatch disables that capture with a held message; nothing falls back to another capture. The **100 MB total** of §4.6 covers both manifests together; until they merge, each manifest is held to 100 MB on its own (`kMaxManifestTotalBytes`), and the merge is where the joint bound starts being enforced.

The contract: `plugins/native/ampcab/include/sonora/ampcab/capture_card.h` (structs, enums, `parse_card`, `parse_name_deny`, `validate_card`, `check_capture_file`, `format_card`), `capture_card_fake.h` (`valid_capture_card()`, `valid_ir_card()`), the static library `sonora_native_ampcab_cards`, and the test `native_ampcab_card_contract`.

## Rationale

- One fenced block keeps the card a readable Markdown page for the researcher and a checkable record for the build, with no second file to drift. A tagged fence (`card-json`, not `json`) means an example in the prose is never mistaken for the card.
- Returning every refusal, not the first, lets one build run tell the author all that is wrong with a card. Not throwing keeps the loader's failure path a message, which §4.6 requires.
- Refusing unknown keys and the wrong kind's block catches copy-paste and typos, the commonest way a provenance record silently loses a field.
- Normalised substring matching catches the near-variants §5 forbids (spacing, hyphens, case) with a short list; a false positive costs a rename, a false negative a trademark problem.
- A separate manifest avoids two tracks editing one file at once, and identical field names make the later merge mechanical.

## Consequences

- The amp-and-cab slices after this one (the card check in `cmake/cards.cmake`, the capture manifest, the loader's disable-with-message path, the first real cards) build on this header and do not edit `plugins/native/ampcab/CMakeLists.txt` or `src/capture_card.cpp`.
- `ampcab_name_deny.txt` is a later slice's file; until it exists the build check has no deny list to apply and must fail rather than pass (a missing list is not an empty one).
- When the two manifests merge, the 100 MB bound becomes joint and this ADR's §5 per-manifest rule is retired by an amendment.
- The 25 MB and 100 MB figures stay ADR-0011's placeholders until the owner rules (ADR-0011 "For the owner", item 5); changing them is a constant and an amendment.

## Alternatives rejected

- **A separate `<id>.json` beside the Markdown card.** Two files per capture drift; the researcher edits the prose and forgets the JSON.
- **YAML front matter.** No YAML parser in the neutral modules, and front matter is easy to break with a stray `---` in prose; `common/json` is already there.
- **Adding captures to `plugins/native/models/manifest.json` now.** ADR-0066 owns it and its slice is in flight; two tracks on one file means conflicts and a contract change by side edit.
- **`validate_card` throwing on the first error.** One fix per build run, and an exception path in the loader where §4.6 wants a message.
- **Exact-match deny list.** Misses `ZX-900` against `zx900`, the near-variants ADR-0011 §5 names.
- **Checking `unit.type` against the deny list.** The card must record the real unit, brand and model included; it is provenance, never shipped as a name.
