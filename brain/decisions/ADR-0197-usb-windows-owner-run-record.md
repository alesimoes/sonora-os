# ADR-0197 - The owner's stick run is recorded in `brain/research/` under a fixed format

- Status: Accepted
- Date: 2026-11-14
- Item: `system.usb.windows` (parked, waits on hardware)
- Confirms: ADR-0110
- Relates to: ADR-0005 (USB persistence), ADR-0090 (disk layout)

## Context

ADR-0110 closes `system.usb.windows` only with one owner run on a real stick:
`tools/write-usb.ps1` writes the stick from Windows, the owner boots it and
records the result. The Decision names the evidence in prose. Nothing yet says
where the record lives, which fields it has, or what counts as a failure, so
the run could be recorded differently each time and the item could be ticked
on a partial record.

The owner's 2026-11-13 rule parks hardware validations to the end of the
project. Agents cannot run the stick write or the boot, so the item stays
open until the owner runs it. This ADR does not change that; it fixes the
format of the record the owner fills in.

## Decision

1. ADR-0110 stands. This ADR adds no code and changes no script or test.
2. The record is `brain/research/usb-windows-owner-run.md`, a fill-in form
   with one section per required field. The owner copies it, fills it in
   with the output as printed, and opens a PR that adds it under
   `brain/research/`.
3. The commands are the ones in `iso/README.md` ("Writing the stick from
   Windows"). The record references them by file and line and does not
   restate them. If those lines change, the record follows them, not the
   other way round.
4. The record must hold these fields, all of them, before the item can be
   ticked:
   - the `-List` output;
   - the `-DryRun` output;
   - the real write's output with every `read back ok` line and the `Done:`
     line, and the process exit code;
   - the boot result under BIOS and under UEFI (the two bootloaders the
     image carries);
   - whether the session survived a reboot;
   - the Windows version and the stick model.
5. A failed run is recorded as failed, with the same fields. It is not
   dropped. Per ADR-0110 Decision 3, the failure becomes a regression case in
   `tools/test-write-usb.ps1` (a fake disk that reproduces it) before the fix.
6. The BACKLOG line for `system.usb.windows` is ticked only by the
   coordinator's chore PR, and only when the record has every field above
   filled with real output. A record with a blank field keeps the item open.

## Rationale

ADR-0110 already decided that the evidence is the owner's run. Writing the
format down now means the first run, and any failed run after it, can be
checked against the same list. The fill-in form costs the owner a few minutes
and removes the chance of a "it worked" line with no exit code or no read-back
lines behind it. Pointing at `iso/README.md` keeps one copy of the commands,
so the record cannot drift from the script's usage text.

## Consequences

- The item stays open and listed under "waits on the owner" until the record
  exists with all fields filled.
- Agents do not tick the item, and do not write a result for it from the
  Windows nightly tests (they use regular files as disks).
- `iso/README.md` is the single source of the commands; a change there is
  reflected in the record by reference only.
- Unverified: that `CreateFileW` on a locked, dismounted USB disk succeeds on
  the owner's Windows 11 build, and that the written stick boots under BIOS and
  UEFI. Only the owner's run can measure these.

## Alternatives rejected

- Let the owner write the evidence freely in the PR body: the fields would
  vary from run to run and a missing exit code would pass unnoticed.
- Put the commands in the record again, with the owner's N and paths filled in:
  two copies of the commands would drift, and the record would stop matching
  what the script prints.
- Close the item on the nightly Windows tests: they use regular files as
  disks, so they cannot show the real-device path works (ADR-0110).
- Automate the stick run from an agent: an agent cannot reach a physical USB
  stick or a boot on the owner's machine, and the owner's 2026-11-13 rule parks
  these checks to the end.

## Unverified

- The plan `planning/plans/system.usb.windows.md` named in the assignment is
  not on `main`, on `feat/system.usb.windows`, or in this branch; this ADR was
  written from ADR-0110 and `iso/README.md` alone.
- Nothing in this ADR has been run on hardware. Every field in the record is
  empty until the owner's run.
