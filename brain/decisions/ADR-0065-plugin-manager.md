# ADR-0065: The plug-in manager: "blocked by you" as a refusal prefix, no protocol bump

Status: Accepted (recommended defaults, 2026-11-07; BACKLOG `plugins.host.manager`).
Builds on: ADR-0012 (amendment of 2026-11-03: the operator's block list, `plugin.block` / `plugin.unblock`), ADR-0007 §4.1 (the control API).
Amends: ADR-0012's 2026-11-03 amendment, item 5 ("the plug-in manager's own field ... is the UI slice's protocol bump"): there is no field and no bump.

## Summary

The plug-in manager has to show two kinds of unusable plug-in apart: the
ones **the operator blocked** (which it can unblock) and the ones **refused by
their probe** (crashed, hung, or disagreed with what they declared, which it
cannot). Both already arrive as `PluginInfo::usable == false` with a reason in
`PluginInfo::refusal`. The difference is carried by **fixed words at the start
of that reason**: `kBlockedRefusalPrefix` (`"blocked by you"`), tested by
`is_blocked_refusal()`, both in the header-only
`contracts/api/include/sonora/api/plugin_refusal.h`. `PluginInfo` gets no new
field, `messages.h` does not change and `kProtocolVersion` stays as it is.
A blocked plug-in whose bundle is gone is still listed, unusable, with the
prefix, so it can be unblocked. An insert refused for being blocked is retried
by the UI adding it again; there is no `plugin.retry`. Favourites are the
interface's setting, in its preferences file, and never reach the engine.

## Context

ADR-0012's amendment of 2026-11-03 gave the engine the operator's block list
(`~/.config/sonora/plugin-blocklist.json`) and the `plugin.block` /
`plugin.unblock` commands. It masks every record it publishes for a blocked
id as `Blocked`, with the reason `kBlockedByOperator`
(`engine/control/include/sonora/plugin_block_list.h`: "it is blocked in the
plug-in manager"). The scanner copies that reason into `PluginInfo::refusal`
(`plugin_scanner.cpp`), and `catalog_refusal` refuses an `AddInsert` naming it
as `"<name> cannot be used: <refusal>"`.

That amendment left one piece to the UI slice: telling a block from a probe's
refusal, which it expected to be a new `PluginInfo` field and a protocol bump.
No protocol number is reserved for this item, and the information is one bit
that the engine already writes in words.

## Decision

1. **A prefix on the existing field.** A plug-in the operator blocked has a
   `PluginInfo::refusal` that is `kBlockedRefusalPrefix` alone or the prefix,
   `':'` and more words (`"blocked by you: it is blocked in the plug-in
   manager"`). `is_blocked_refusal(refusal)` is true exactly then; the prefix
   must end the text or be followed by `':'`, so a probe's reason that happens
   to begin with the same letters is not read as a block. The comparison is
   exact and case-sensitive. The words are wire-visible: changing them is a
   protocol change, so they do not change.
2. **The predicate reads the catalog entry, not an insert refusal.** An insert
   refusal wraps the reason in the plug-in's name; the UI that needs to know
   why an insert was refused finds the catalog entry by format and id and
   asks the predicate of its `refusal`.
3. **The engine slice makes its reason start with the prefix.**
   `kBlockedByOperator` becomes `"blocked by you: it is blocked in the plug-in
   manager"` (built from `kBlockedRefusalPrefix`). Trap for that slice: the
   scan cache stores the stand-in record of a blocked plug-in with that reason
   and `stand_in()` compares it for equality, so a cache written before the
   change must still be recognised (accept the old words as a stand-in, or
   bump the cache's format so it is rebuilt). A probe reason must never start
   with the prefix; the engine slice's test checks the reasons it can produce.
4. **A blocked plug-in whose bundle is gone stays listed.** The engine lists
   every id on the block list that no scanned bundle provides as a catalog
   entry with `usable = false`, the prefix as its refusal, the name the list
   last recorded and nothing else known (no channels, no parameters). The
   operator can then find it and unblock it; otherwise the block list holds
   entries nobody can see or remove except by editing the file.
5. **No `plugin.retry`.** An insert refused because its plug-in is blocked
   stays refused. After an unblock, the UI offers to add it again, which is an
   ordinary `AddInsert` (undoable, refused again with the same words if the
   unblock has not reached the catalog yet). A retry command would duplicate
   `AddInsert` with a second path through `catalog_refusal`.
6. **Favourites are the interface's.** Which plug-ins the operator marks as
   favourites is how it looks at the catalog, not what the engine may run.
   The UI keeps them in its preferences file (the interface-domain settings
   of ADR-0012), keyed by format and id, and never sends them to the engine.
   A favourite that is not in the catalog is kept and shown nowhere until it
   comes back.

## Rationale

The bit already exists in the engine's words; making those words fixed costs
one header and no wire change. A field would have been clearer on the wire,
but it costs a protocol number, a codec change on both sides and a version
check, for one bool that the reason already implies. The UI cannot mirror the
block list either: the engine's file is the authority, it is written by the
scan worker, and a second copy in the UI would disagree after an edit made
elsewhere (another client, the file edited by hand, an unreadable file renamed
away).

## Consequences

- The UI's plug-in manager shows "Blocked" (with Unblock) for
  `is_blocked_refusal(refusal)` and "Refused" (with the reason) otherwise.
- A reason the engine writes for any other refusal must not start with the
  prefix. The predicate's boundary rule narrows the risk; the engine slice
  tests its own reasons.
- If a later protocol bump touches `PluginInfo` for another reason, a
  `blocked` field may be added then and the prefix kept for older clients;
  that is a later ADR's choice, not a debt of this one.

## Alternatives rejected

- **`PluginInfo::blocked` and a protocol bump.** Clearer, but dearer: a
  reserved protocol number, both codecs, a version check, for one bool the
  reason already carries. No protocol number is reserved for this item.
- **The UI keeping its own mirror of the block list.** It drifts from the
  engine's file, which is the authority and is written only by the engine.
- **Matching the engine's current words** ("it is blocked in the plug-in
  manager") **in the UI.** They are prose, not a contract, and live in an
  engine header the UI must not include; a fixed prefix in `contracts/api`
  is the same idea made a contract.
- **A `plugin.retry` command.** See Decision 5.

## Contract

- `contracts/api/include/sonora/api/plugin_refusal.h`: `kBlockedRefusalPrefix`,
  `is_blocked_refusal(std::string_view)`, header-only, `constexpr`.
- `contracts/api/tests/test_plugin_refusal.cpp` (ctest `api_plugin_refusal`):
  the prefix's words, the predicate on blocks, on probe reasons and on near
  misses.
