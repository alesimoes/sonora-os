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
