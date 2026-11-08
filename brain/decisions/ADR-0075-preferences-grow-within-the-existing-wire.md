# ADR-0075: Preferences grow within the existing wire

Status: Proposed (architect review pending).
Builds on: ADR-0020 (transport commands), ADR-0004 (workflow, not binaries).

## Summary

Preferences goes from version 1 to 2 with two booleans, `click_at_start` and `count_off_at_start`. They are applied through the existing toggle commands by comparing with `transport_flags`. Setup shows the engine's facts read-only. No protocol bump: engine-side settings wait for a protocol reservation.

## Context

Setup > Preferences (`ui/qt/src/preferences.h`) holds only what the interface can honour. Operators also expect Click and Count Off to be as they left them. On the wire these are toggles (`transport.toggle_click`, `transport.toggle_count_off`), not setters, and the engine reports them in `transport_flags` (`kTransportClick`, `kTransportCountOff`).

## Decision

1. **No protocol bump.** Autosave interval, backup count, AutoMatch time, audio device and buffer need new wire messages. They stay out; the window shows them read-only when the state block carries them and omits the row otherwise. Never a control that would not act.
2. **Preferences version 2** adds `click_at_start` and `count_off_at_start` (default false). A version-1 file loads, missing keys keep defaults. Version 0, above 2, a non-boolean value or an unknown enum value is refused with the reason, `out` untouched.
3. **Applying is a plan, not a toggle.** `plan_transport_toggles(prefs, transport_flags)` (`ui/qt/src/transport_prefs.h`) returns which toggles to send: only those where the flag differs from the preference. With no engine connected the preference waits and applies on the first state block. A reconnect therefore never inverts the setting.
4. **Setup sections:** Playback Engine (format; buffer and device only when the state block carries them) and Click / Count Off.

## Rationale

Toggles cannot be set blindly: sending one per start would flip the engine on every reconnect. The state block is the single truth, so we read it first. Keeping the plan pure makes it testable without an engine.

## Consequences

- Older builds refuse a version-2 file with a reason (they read version 1 only); newer ones read both.
- The first state block after connect may cause up to two toggles; the window must not show the preference as applied before that.
- Engine-side settings remain unreachable until the follow-up below.

## Follow-up (needs a protocol reservation)

Wire messages for autosave interval, backup count, AutoMatch time, audio device and buffer, as a contract PR owned jointly with rt-engineer.

## Alternatives rejected

- **Send a toggle on every start:** inverts state on reconnect.
- **Add set-click/set-count-off commands now:** a protocol bump for something the state block comparison already solves.
- **Show engine settings as editable controls:** they would not act (a control that lies).

## Unverified

- Behaviour of the UI wiring (applying on first state block, Setup sections): not part of this contract, covered by the implementation PR.
- Whether the engine ever delivers a state block with a flag change between our read and our toggle; the next state block would correct it by the same plan.
