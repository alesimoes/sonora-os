# Plan: system.install.rootfs - Real root filesystem tuned for audio, swap sized for the machine

Status: approved
Written by: architect   Dispatched by: lead   Architect sign-off: architect 2026-11-08

## 1. Goal and user experience
`sonora-install` produces: ESP, 16 GiB ext4 root, swap partition sized from
RAM, and a separate `sonora-data` ext4 mounted at `/home/sonora/Sessions`.
A full session disk cannot break the OS; a missing data partition stops boot
loudly (ADR-0010 fallback), never records into a hidden directory. A disk too
small, or an image that does not fit the root, is refused before any write.
Out of scope: LUKS, migration of earlier installs, Secure Boot, read-only root
(`system.hardening.ro_root`), the installer screens.
Grouping: single open item (`system.install.rootfs`); layout, fstab, swap, data
mount, fit check and tests are one change to one script, so one PR.
Acceptance: `tools/test-install-layout.sh` and `tools/test-installer.sh` pass.

## 2. Design
N/A - no UI; the tty2/Qt front ends are `system.install.installer`.

## 3. Contracts and integrations
ADR: ADR-0090 (written, indexed). Protocol/format: none.
Reserved numbers: ADR 90, protocol none, formats none.
Contract = the function interface of `install-layout.sh`, merged first with its
test. Pure shell, no disk access: `layout_swap_mib RAM_MIB`,
`layout_plan DISK_MIB RAM_MIB EFI(0|1)` printing `key=value` lines (`esp_mib`,
`root_mib`, `swap_mib`, `data_mib`, plus the sfdisk script) or returning 4 with
a reason on stderr, `layout_fits_root SQUASH_MIB ROOT_MIB`.
Acceptance: `bash tools/test-install-layout.sh` passes with fake sizes.

## 4. DAW core
N/A - no engine work.

## 5. Plugins, DSP, host
N/A - none.

## 6. UI
N/A - none.

## 7. Tests
`tools/test-install-layout.sh` (pure, no Docker; RAM 2/4/16/64 GiB, smallest
disk, BIOS vs EFI). `tools/test-installer.sh` (privileged loop-device run):
four UUIDs in fstab, swap signature, data mounted, refusal exit 4.
new `tools/test-install-layout-qemu.sh` (test-install-qemu.sh is owned by open PR 442): `swapon --show` and the Sessions mount in the
installed boot. No new ctest target; these are the existing os-image tests.

## 8. QA
Install to a blank 40 GiB QEMU disk (UEFI and BIOS); wipe the data partition
and boot: expect the fallback, not a recording. Real hardware is unverified.

## 9. Quality checks
`tools/pre-pr.sh`; shellcheck as CI runs it. `sonora-install` stays under about
500 lines because the arithmetic lives in the library. Review focus: every step
checked, no silent `|| true`, install onto a previously used disk.

## 10. Code standards
`.claude/rules/*`. `.claude/context/os-image.md` is read-only here (owned by
`system.hardening.ro_root`).

## 11. Dependencies and order
layout (contract) -> wire -> tests + docs. Single PR.
| Slice | Agent | Difficulty | Risk | Tier | Sub-branch | Starts from | Files |
|---|---|---|---|---|---|---|---|
| layout | distro-engineer | M | high | senior | feat/system.install.rootfs-layout | main | install-layout.sh, tools/test-install-layout.sh |
| wire | distro-engineer | M | high | senior | feat/system.install.rootfs-wire | layout | sonora-install |
| tests | distro-engineer | M | med | pleno | feat/system.install.rootfs-tests | wire | tools/test-installer.sh, tools/test-install-layout-qemu.sh |
| docs | distro-engineer | S | low | junior | feat/system.install.rootfs-docs | wire | iso/README.md |
Integrator: the lead names one.
Single PR title: `installer: swap, separate data partition, fit check (ADR-0090)`

## 12. Risks and open questions
| Question | Recommended default |
|---|---|
| Root size 16 GiB | Fixed; the fit check refuses an image that outgrows it |
| Swap size | clamp(RAM/2, 2, 8 GiB), 256 MiB steps |
| Data mount without `nofail` | Yes, fail loudly; engine drop-in `RequiresMountsFor` written into the target |
| Throughput on the target disk, engine `mlockall` | Researcher measures (ADR-0090 Unverified) |

## 13. Done =
- [ ] layout contract and test merged into the feature branch first
- [ ] slices merged by the integrator
- [ ] tests of section 7 pass (commands and output in the PR)
- [ ] `tools/pre-pr.sh` clean; no file past ~500 lines
