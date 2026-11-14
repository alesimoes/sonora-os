# ADR-0178: Read-only root close-out and the full-overlay follow-up

Status: accepted

## Context
ADR-0074 fixed the scope and evidence of the read-only root; `tools/test-ro-root.sh` and #351, #398 and #519 are on main. What is unverified is behaviour when the writable overlay on `/` fills while recording.

## Decision
The item closes on ADR-0074's evidence. Takes land on `/home` (persistence or internal disk), never on `/`. The overlay-full case becomes a follow-up: a QEMU test that fills the overlay and asserts the engine either keeps bounded buffers or fails loudly at start, never silently while recording. It needs KVM and goes to the researcher and tester.

## Rationale
Losing a take is the worst outcome; a loud start-up failure beats silent degradation. The KVM check cannot run in the PR gate, so it must not block the close-out.

## Consequences
The running-mount and full-overlay checks stay listed as unverified until the QEMU test exists. No code changes.

## Alternatives rejected
- Hold the item open until the QEMU test exists: blocks a done item on hardware-bound work.
- Reopen ADR-0074: it is accepted; a new ADR records the amendment.
