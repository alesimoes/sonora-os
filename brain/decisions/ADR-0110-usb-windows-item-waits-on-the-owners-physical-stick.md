# ADR-0110 - `system.usb.windows` is code-complete; the last part is the owner's physical stick

- Status: Accepted
- Date: 2026-11-08
- Item: `system.usb.windows`
- Relates to: ADR-0090 (disk layout)

## Context

The BACKLOG line "A verified way to write the stick from Windows" is open. Main
has `tools/write-usb.ps1` (1026 lines: USB-only, system-disk refusals, typed
token, read-back, `-FakeDisks`, `-SelfTest`) and `tools/test-write-usb.ps1`
(368 lines), which runs in the nightly `portable` job
(`.github/workflows/nightly.yml`, windows-latest), not on the PR gate. The
line's own text names the remaining part: the owner writes a physical stick
with it. The history is why this matters: the first Windows writer opened
`\PhysicalDrive` with one backslash and erased the wrong place while writing
nothing. Fake disks prove the logic, not that Windows accepts the
locked-and-dismounted raw handle on a real stick.

## Decision

1. No new code. Nothing an agent can run replaces the physical write: the
   item stays open and is not ticked by reference to the Windows tests.
2. The evidence that closes it is one owner run on a real stick, recorded as
   text in the closing PR or the BACKLOG chore: the `-List` output, the
   `-DryRun` output, the real run printing its `read back ok` lines and the
   `Done:` line, exit 0, and "booted on the UMC1820 machine, session survived
   a reboot" (or the failure). Commands are the ones in `iso/README.md`.
3. If the owner run fails, the failure becomes a regression case in
   `tools/test-write-usb.ps1` first (fake disk reproducing it), then the fix.
4. Until then the plan's single PR is partial, titled as waiting on hardware
   (`.claude/rules/branches-and-prs.md`).

## Rationale

Ticking by assertion is what cost us before (green without compiling, the
erased-wrong-disk writer). A stick write is the only check that touches the
real Win32 path; a cheap honest record costs nothing.

## Consequences

- The coordinator lists the item under "waits on the owner" and moves on.
- Unverified: that CreateFileW on a locked, dismounted USB disk succeeds on
  the owner's Windows 11 build; that the written stick boots. Measurement: the
  run in Decision 2 (researcher may script the checklist; nobody else can run it).

## Alternatives rejected

- Close by reference to the nightly Windows tests: they use regular files as
  disks, so they cannot show the real-device path works.
- Rewrite the writer around Rufus/dd for Windows: adds a dependency and
  discards a tested script to avoid one hardware run.
- Drop the item: the owner works on Windows; no other way to get a stick.
