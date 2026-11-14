# ADR-0190: Varispeed control is a catalog command

Status: accepted

## Context
Session::set_varispeed exists in the engine (relaxed atomic, ADR-0033). No wire path or UI sets it.

## Decision
`transport.varispeed` is a catalog command (CommandRequest.argument = ratio as decimal text, 0.5..2.0, 1 resets). Amends ADR-0033 section 4. No new message, so no protocol bump. Out of range, NaN or empty is refused with a reason and changes nothing. The ratio is not saved in the session.

## Rationale
The command catalog already carries a text argument; a new message would force a protocol bump for one float.

## Consequences
The engine parses text on the control thread only; the audio thread is unchanged. Unverified: whether replicas need the ratio pushed in state; measure by the UI test showing the field after reconnect.

## Alternatives rejected
A dedicated wire message (protocol bump, no benefit). A session-saved ratio (a take would replay at a shifted speed after reload).
