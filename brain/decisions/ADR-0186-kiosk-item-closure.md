# ADR-0186 — Closure of the system.image.kiosk item

Status: accepted (closure record, 2026-11-14)

## Summary
The `system.image.kiosk` item closes. Its two deliverables under ADR-0118 are
on main: the smoketest asserts the kiosk really runs (#613, commit b7aebf69),
and `tools/test-image-config.sh` keeps those checks in place. The
default-on flip of the compositor is not made here; it waits for a KVM
`--gui` and `--gui-broken` QEMU run and stays parked (owner, 2026-11-13).

## Context
ADR-0118 split what was left of ADR-0010 into two parts: smoketest assertions
and a written gate for the default flip. Both are now in the tree:
`sonora-smoketest` checks the compositor and UI units under `sonora.gui`, and
the fallback path under `sonora.expect_fallback`; `iso/README` documents that
the kiosk is opt-in and what the default-flip gate requires. Verified with
`git log origin/main -S unit_running`, which lists #613.

## Decision
1. The `system.image.kiosk` item is closed for the work it covered: the
   smoketest assertions and their static check.
2. The default flip stays a separate, open item. It is not part of this
   closure and needs a recorded KVM run first (ADR-0118 decision 3).
3. No new code, contract or protocol change is made by this closure.

## Rationale
Leaving the item open would keep a closed deliverable on the board and hide
the one remaining open part behind it. Splitting the flip out keeps the open
work visible and honest.

## Consequences
- The backlog's kiosk line is ticked only for the split-out, closed part.
  The flip is tracked as its own open line.
- Nothing in the image changes: the default stays opt-in until the KVM gate
  is met.

## Unverified
- The default flip itself: not booted on KVM here. Owner-run, parked.
- This closure was made from static evidence (commit history and the static
  check); no image was built or booted by this change.

## Alternatives rejected
- Keep the item open until the flip: rejected, it holds a finished deliverable
  hostage to a hardware run the owner has parked.
- Flip the default now: rejected, ADR-0118 requires the KVM evidence first.
