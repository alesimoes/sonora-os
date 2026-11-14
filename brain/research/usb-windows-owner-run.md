# Owner-run evidence record: `tools/write-usb.ps1` on a physical stick

Fill-in record for ADR-0197, which confirms ADR-0110. Status: **not yet run**.
The item `system.usb.windows` stays open until every field below holds real
output.

Commands: `iso/README.md`, section "Writing the stick from Windows", lines
28-31 (`-SelfTest`, `-List`, `-DryRun`, the real write). Run them exactly as
written there, with `N` replaced by the stick's disk number from `-List`.
Paste the output as printed; do not summarise it.

## Setup

- Date of run:
- Owner:
- Windows version (`winver` or `Get-ComputerInfo`):
- Stick model and capacity (from the label or `Get-Disk`):
- Image used (`iso/sonora-stick.img` or `iso/sonora.iso`), and its `.sha256` line:
- Disk number used for `N`:

## 1. Self-test (`-SelfTest`, iso/README.md line 28)

Output:

```
(paste)
```

Exit code:

## 2. List (`-List`, iso/README.md line 29)

Output:

```
(paste)
```

## 3. Dry run (`-DiskNumber N -DryRun`, iso/README.md line 30)

Output:

```
(paste)
```

Exit code:

## 4. Real write (`-DiskNumber N`, iso/README.md line 31)

Output, with every `read back ok` line and the `Done:` line:

```
(paste)
```

Exit code (must be 0 for a pass):

## 5. Boot

- BIOS boot (legacy): pass / fail. Boot menu entry and keys used:
- UEFI boot: pass / fail. Boot menu entry and keys used:
- Machine (model, CPU, RAM):
- Audio interface attached (UMC1820 or other):

## 6. Reboot

- Session survived a reboot (the same stick, booted again): yes / no.
- If no: the last visible line or screen, and what was done instead.

## 7. Result

- Overall: pass / fail
- If fail: the failing step number above and the exact message. Per ADR-0110
  Decision 3, this becomes a regression case in `tools/test-write-usb.ps1`
  before any fix.

## Checklist before the item is ticked

- [ ] Windows version filled in
- [ ] Stick model filled in
- [ ] `-List` output pasted
- [ ] `-DryRun` output pasted
- [ ] Real run: every `read back ok` line and the `Done:` line pasted, exit code 0
- [ ] BIOS boot result filled in
- [ ] UEFI boot result filled in
- [ ] Reboot result filled in

A record with any box empty keeps `system.usb.windows` open (ADR-0197
Decision 6).
