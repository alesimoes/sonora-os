# ADR-0155 — Claude assistant contract (claude.assistant)

Status: accepted.

## Summary

An opt-in layer, off by default, lets the mix assistant ask Claude for a
plan with the operator's own API key. It sends measurements and session
structure, never audio; the reply becomes an ordinary `model::Plan`
(ADR-0038) reviewed as a diff and applied as one undo step. The contract is
header-only in `assistant/include/sonora/assistant/`; the pure core and the
single network implementation build on it.

## Context

ADR-0011 §3 lists an optional Claude layer beside the native mix assistant.
Owner, 2026-11-13: opt-in, with the user's own key. The appliance must not
send anything off the machine unless the operator asked, and must never
send audio. The native planner (`plugins/analyze`, `common/model/plan.h`)
is already on main.

## Decision

1. **Off by default.** `AssistantSettings::enabled` is false. While it is
   false no transport is constructed or called. `Unavailable` names every
   reason Claude cannot answer (off, no key, offline, bad key, API error,
   too large, bad reply) and `unavailable_text()` gives the line the UI
   shows next to the native suggestions, so a missing Claude plan never
   reads as "nothing to suggest".
2. **No audio.** `FeatureBundle` (feature_bundle.h) holds only scalars,
   names, ids and a fixed `std::array<double, kBands>` of band levels per
   strip. It has no sample buffer, frame count or media path. Adding a field
   is a privacy decision taken in this ADR.
3. **Bounded request.** `build_request(bundle, max_bytes, out, error)` takes
   the bundle and a byte cap and nothing else (the contract test pins the
   signature). Over the cap it drops spectra, then insert names, and refuses
   (`TooLarge`) rather than describe a different session. Default cap 64 KiB.
4. **Untrusted reply.** `parse_reply` accepts only `kReplyOps`
   (`set_track_gain`, `set_track_pan`, `set_send_level`, `set_bus_gain`),
   each one `plan_revertible` member, and only ids present in the bundle,
   with values range-checked. Any violation refuses the whole reply. Track
   names and the intent are data: a name that reads like an instruction can
   at most produce a plan the operator reviews, never an unlisted op.
5. **The key.** `KeyStore` hides storage (on the appliance, a file readable
   only by the engine's user). The key is passed to the transport beside
   the request, never in a body, never logged; Settings shows `mask_key()`.
6. **One network seam.** `HttpTransport::post(request, key, response)` is
   the only call that leaves the machine; `FakeTransport` scripts it for
   tests. Nothing here runs on the audio thread.

Module: `assistant/`, one module (edge `assistant -> common/model`), label
`assistant`. Nothing under `engine/` or `drivers/` links it.

## Rationale

Keeping the request builder's input to a typed bundle makes "never audio"
a compile-time property instead of a review habit. Mapping replies onto
existing plan members reuses the diff preview and the single-undo path the
native assistant already has, so a Claude plan cannot do anything the
operator could not undo in one step.

## Consequences

- The UI shows the source ("Claude") on the diff and the unavailable reason.
- Tests: switch off opens no connection; request has no audio and respects
  the cap; key never in body or log; unknown op refused; failure falls back.
- `tools/check-deps.sh` scans fixed top directories; `assistant/` needs adding
  there (follow-up) for the allow edge to be enforced.

## Alternatives rejected

- Sending audio excerpts for richer advice: out of scope (privacy, owner).
- Letting the reply name arbitrary model operations: not all are undoable
  through History, so "one undo reverts it all" would be false.
- Putting the key in the request struct: one log line would leak it.

## Unverified

Real API latency and request size with a user key (researcher, plan §9).
