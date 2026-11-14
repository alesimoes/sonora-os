# ADR-0225 - MTC chase never stops or relocates a recording

Status: accepted (amends ADR-0122 section 5)

## Summary
When the MTC master goes silent, `MtcChaseDriver` stops a rolling engine
(ADR-0122). That includes Recording, so a cable pull ends a take. Decision:
while the transport is Recording the chase never sends `transport.stop`; it
counts the event in `MtcSyncStats::silence_while_recording`. Only the
operator stops a recording.

## Context
ADR-0122 treats Rolling and Recording alike as "rolling". The Locate path
already refuses to move a recording (drift is counted); the Stop path does
not. Losing a take is the worst outcome in the product, and master silence
(unplugged cable, deck power-off, USB MIDI glitch) is a plausible event.

## Decision
`MtcChaseDriver::act(Stop, t)`: if `t.state == Recording`, send nothing,
increment `silence_while_recording`. `ChaseRule` stays pure and unchanged;
the guard sits beside the existing Locate guard. Play is unaffected (engine
is already running).

## Rationale
Same reasoning as the Locate guard: a recording is never moved by an
external clock. A fail-loud counter is visible to the operator; a stopped
take is not recoverable.

## Consequences
- A recording continues past master loss, with its own clock; the operator
  stops it. A UI warning on the counter is a later item.
- No change to `contracts/` or the protocol.

## Alternatives rejected
- Stop anyway and rely on crash-safe recovery: still ends the take early.
- Put `recording` into `ChaseSnapshot` and decide in `ChaseRule`: widens a
  merged header for no gain; the driver already holds the state.

## Unverified
Behaviour with a real master dropping out is not measured; parked with the
hardware validations in ADR-0122.
