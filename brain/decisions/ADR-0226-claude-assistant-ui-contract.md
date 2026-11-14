# ADR-0226 — Settings and suggestion source for the UI (claude.assistant)

Status: accepted.

## Summary

The UI drives the Claude layer (ADR-0155) through one interface,
`AssistantService`: settings, switch, key, and an asynchronous
`request_claude` with a completion callback. A fake ships with a contract
test. Off by default.

## Context

ADR-0155 put the gate (`AssistantCore`), the `KeyStore` and the transport on
main. `ui/` may not depend on those implementations, and `suggest()` blocks
for the network, which the UI thread must never do.

## Decision

1. `assistant/include/sonora/assistant/assistant_service.h`: `settings()`,
   `set_enabled`, `set_key`, `clear_key`, `masked_key`, and
   `request_claude(bundle, snapshot, completion)`.
2. The completion receives a `ClaudeResult`: `why` (an `Unavailable`),
   `has_plan`, the `model::Plan` and a `detail` string. `why == None` means
   the plan is Claude's; anything else means native suggestions are shown
   with `unavailable_text(why)` as a held notice, never the status line.
3. A refusal that needs no network (SwitchedOff, NoKey) completes
   synchronously and opens no connection. Otherwise the completion runs on
   the service's own thread; the UI posts it to its thread.
4. `set_enabled` and `set_key` return false with an error when they cannot
   persist, and the switch then stays as it was.
5. A fresh profile is off. `masked_key()` is the only key text the UI sees.
6. `FakeAssistantService` (header-only, opens nothing) and
   `test_service_contract.cpp` pin this. `assistant/CMakeLists.txt` adds
   `src/service` and `tests/service` when they exist, so later tasks do not
   edit it.

## Rationale

A callback keeps the UI thread free without making the UI own threads. One
interface lets the UI be tested without a socket. Reusing `Unavailable`
keeps one vocabulary for the reason a plan is missing, so a control never
looks like Claude had nothing to say.

## Consequences

The real service (wrapping `AssistantCore`, `FileKeyStore`, the transport)
and the Settings screen are later tasks. The service reports Claude vs
native through `why`; `Source` stays in the core header, not duplicated here.

## Alternatives rejected

- A blocking `suggest()` from the UI: freezes the interface during a
  timeout.
- Futures: need a thread-owning caller; a callback fits the UI's event loop.
- The UI holding `KeyStore` directly: spreads the key to more code.

## Unverified

Behaviour of the real service and its threading; only the fake is tested.
Screenshots are not committed.
