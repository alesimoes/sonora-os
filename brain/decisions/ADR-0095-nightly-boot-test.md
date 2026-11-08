# ADR-0095: Nightly QEMU boot test

Status: accepted (2026-11-08)

## Summary

The QEMU boot test (`tools/test-boot-qemu.sh`) runs every night against the
real ISO. Target shape: boot steps inside `nightly.yml`'s `usb-writer` job,
sharing its ISO build. Interim: `.github/workflows/nightly-boot.yml`, a
separate workflow.

## Context

Only a booted image shows systemd, the running kernel and the engine unit. No
CI job booted the ISO, so a broken image was found by hand. ADR-0061 keeps full
builds out of PRs, so the nightly is the place.

## Decision

- Boot test in the nightly, not per PR (ADR-0061).
- Target: add the boot steps to the `usb-writer` job, which already builds
  `iso/sonora.iso`; one ISO build serves both.
- INTERIM: `nightly-boot.yml` (cron 05:00 UTC, after nightly's 03:00, plus
  `workflow_dispatch`) builds the ISO itself and runs the script unchanged,
  because `nightly.yml` (feature architecture.adr0007.m1_docs) and
  `test-boot-qemu.sh` (PR 442) are held by other active features. Cost: a
  second ISO build, 30-50 runner minutes per night, until the follow-up.
- Default boot always runs. `--gui` and `--gui-broken` run only when
  `/dev/kvm` exists; otherwise a `::warning` names the skipped boots. Boot
  steps are not `continue-on-error` and carry explicit timeouts.
- Serial logs upload as an artifact. The script removes its log at each start,
  so the workflow sets `SONORA_QEMU_DIR` and copies `serial.log` after each boot.

## Rejected

- ISO as an artifact across jobs: about 1 GB per night.
- Boot per PR: contradicts ADR-0061.

## Unverified

- That the hosted runner exposes `/dev/kvm` (without it only the default boot runs).
- TCG (no KVM) boot time against the script's timeout and the 20-minute step limit.
- Availability of snapshot.debian.org during the ISO build.
- The log copy and artifact upload: not run on a runner yet.

## Follow-up

Fold the boot steps into `usb-writer` in `nightly.yml` once its owner and
PR 442 release their files; delete `nightly-boot.yml`.
