# ADR-0204: Preferences engine settings, close-out of ADR-0075

Status: Accepted (amends ADR-0075; does not edit it).

## Summary

ADR-0075 and its code merged in #597 (Preferences v2, click and count-off applied from `transport_flags`). What is left (autosave interval, backup count, AutoMatch time, audio device, buffer) needs wire messages. No protocol number is reserved, so this feature closes as partial and the rest waits for a protocol reservation.

## Context

The BACKLOG lines for autosave, backups and AutoMatch time are still open. Each needs an engine setter and a state-block field, which is a protocol bump owned jointly with rt-engineer.

## Decision

1. Ship nothing further in the UI: a control with no engine behind it would lie (ADR-0075 section 1).
2. When a protocol number is reserved, one contract PR defines a single `engine_settings` message (set and report) covering the five settings; the UI then shows them read-write.
3. The setters validate at engine start-up and refuse loudly; none may change on the audio thread.

## Rationale

One message for five settings avoids five bumps. Waiting costs nothing: operators already get click/count-off persistence.

## Consequences

- Autosave, backup and AutoMatch BACKLOG lines stay open.
- Unverified: whether the device/buffer change needs a PipeWire graph restart; the researcher measures the restart gap on target hardware.

## Alternatives rejected

- **Five separate messages:** five bumps for one window.
- **UI-only storage of the values:** the engine would ignore them.

## Amendment 2026-11-14: the contract, protocol 71

Protocol 71 was reserved (70 is another PR's), so decision 2 lands as
`contracts/api/include/sonora/api/engine_settings.h`. Decision 1 ends with
it for the UI slice that builds on this contract.

1. **Messages.** `EngineSettingsSet` (client to engine, wire type 11) carries
   all five values, unchanged ones included, so a set never depends on what
   the engine held. `EngineSettingsReport` (engine to client, wire type 12)
   answers it by `seq`, and with seq 0 tells a client what the engine holds
   after Hello or when the values change from elsewhere. It lands in
   `Inbox::settings`. `EngineControl::send_settings()` is virtual, not
   pure: a control with no settings path returns 0 and the interface shows
   a held notice instead of waiting.
2. **Ranges** are those of the engine's own flags, so the window and
   `engine/app --help` agree: autosave interval 0 (off) to 86400 s, default
   60; backup count 0 (none) to 1000, default 20; AutoMatch 10 to 5000 ms,
   default 250; device id at most 256 bytes with no control byte, empty for
   the system default; buffer 0 (the backend's default) or 16 to 8192
   frames.
3. **Refusals.** `validate()` checks a set whole and names the first value
   out of range, its value and its range. The engine stores all or
   nothing: one bad value refuses the request, the report carries the
   values it kept, and **nothing is clamped**, because a clamped value is
   one the operator did not choose and would not see. Codes, fixed for
   good on the wire: 1 autosave interval, 2 backup count, 3 AutoMatch time,
   4 device id, 5 buffer, 6 not stored (valid, but the preferences file
   could not be written). The decoder refuses a code with no reason or a
   reason with no code, so a refusal always reaches the operator in words;
   the UI shows it as a held notice. The decoder does not check ranges, so
   an out-of-range set is refused in words rather than by dropping the
   connection.
4. **When they apply, never on the audio thread.** The engine keeps the
   values on its control thread (`EngineSettingsStore`). Autosave interval
   and backup count apply there at once, at the next timer and the next
   save. AutoMatch is a session's construction-time setting
   (`SessionConfig::touch_return_ms`) and applies at the next session open.
   Device and buffer apply at the next engine start. The report's `pending`
   flags (`kSettingsPendingSessionOpen`, `kSettingsPendingRestart`) say
   which held values the running engine does not use yet, so the window
   can say so.
5. **Fake.** `contracts/api/testing/include/sonora/api/fake_engine_settings.h`
   is header-only and apart from `FakeEngine`. It validates with the
   contract's `validate()`, stores or refuses whole, counts refusals, and
   works out `pending` against what it was told is running. Engine and UI
   tests inject it.

Still unverified: whether a device or buffer change needs a PipeWire graph
restart, and how long the gap is on the UMC1820. That goes to the
researcher; until it is measured, "next engine start" is the only promise.
