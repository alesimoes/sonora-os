# ADR-0189: plugins.host.state closure; what stays open and why

Status: accepted (architect, 2026-11-14)

## Context
ADR-0151 and PR #649 landed the state-safety work: one State deadline
(`kStateReplyTimeoutMs`), one copying GetState at a time, Plugin Files
verified at open, FIFO-safe and inode-keeping copies, brace-initialised
wire structs, a counted `Fault{Plugin}` notice (`paths_noticed`). The
BACKLOG item still lists open lines. Re-planning found that none is
code that can ship now.

## Decision
No code in this feature. The open lines are classified:
1. Free-space margin across hosts: covered by the single copying GetState
   at a time (ADR-0151). A take consuming the margin while a save runs
   stays a risk; the researcher measures it (below).
2. "Collect plugin files" needs a new API message, so a protocol number the
   coordinator has not reserved. Deferred to its own contract-first item.
3. Throughput on a USB 3 stick (`kMaxStateCopyBytes`, `kStateCopyBudgetMs`)
   and read-only (0444) behaviour on FAT/exFAT: hardware measurements,
   handed to the researcher; parked with the hardware validations.
4. Splitting `plugin_hosting.cpp` (1770 lines) is a refactor and never
   shares a PR with a feature (code-structure rule): its own refactor item.
5. "Engine follows the session folder" changes where takes land; own item.

## Rationale
A PR for half of an unreachable line would break "one PR per complete
feature". Recording the classification stops the item being re-planned.

## Consequences
The PR is docs only and titled partial. Unverified: throughput and FAT
behaviour remain assumptions until measured on the reference stick.

## Alternatives rejected
- Implement Collect with an unreserved protocol number: collides with
  other contracts. - Fold the file split into this PR: refactor and feature
  must not share one.
