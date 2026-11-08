# Plan: quality.ci.boot - Boot test in CI (nightly ISO build + QEMU)

Status: approved
Written by: architect   Dispatched by: lead   Architect sign-off: architect 2026-11-08

## 1. Goal and user experience  (architect)
The nightly (`nightly.yml`, once a day on main) boots the real ISO in QEMU and fails if the image does not come up as shipped (RT kernel running, session on tty1, GUI fallback). Today only the owner's machine runs `tools/test-boot-qemu.sh`. Out of scope: bootloader execution (the test boots `-kernel/-initrd`), Secure Boot, install/virtual-card QEMU tests.
Grouped items: only `quality.ci.boot` (BACKLOG "Run the boot test in CI"); no other open OS item touches the nightly's ISO job.
Acceptance: a nightly run shows the boot step(s) green or red with the serial log uploaded as an artifact.

## 2. Design
N/A - no UI.

## 3. Contracts and integrations
ADR-0095 (reserved), written in the same PR as the workflow change. Decision: do NOT add a second workflow that rebuilds the ISO (30-50 runner minutes twice). Add the QEMU boot steps to the existing `usb-writer` nightly job, which already builds `iso/sonora.iso`, renamed "ISO: USB writer and QEMU boots". Rejected: separate workflow with its own ISO build (double cost); ISO as a cross-job artifact (about 1 GB upload/download per night for no gain); running on every PR (violates ADR-0061, tens of minutes).
Failing loudly: boot steps are not `continue-on-error`; if `/dev/kvm` is absent the default boot still runs under TCG (multiplier x8 from `qemu-accel.sh`) and the `--gui*` boots run only when KVM exists, the skip printed as a `::warning`, never silent. A red nightly opens a fix task (CLAUDE.md), it never blocks a PR.
Reserved numbers: ADR 95, protocol none, format none.

## 4. DAW core
N/A.

## 5. Plugins, DSP, host
N/A.

## 6. UI
N/A.

## 7. Tests
The deliverable is the CI step. `tools/test-boot-qemu.sh` is reused unchanged unless the runner needs a small fix (artifact path for `serial.log`). Run `shellcheck` and `tools/pre-pr.sh`.

## 8. QA
Trigger the nightly with `workflow_dispatch` is not available (schedule only): if the workflow has no dispatch trigger, add `workflow_dispatch:` so the first run can be exercised by hand. Check timings of the first real run.

## 9. Checks and risks
Unverified assumptions (hand to researcher): (1) GitHub `ubuntu-24.04` runners expose `/dev/kvm` to docker `--device`; measure with a one-off job `ls -l /dev/kvm`. (2) The default boot under TCG fits the job's `timeout-minutes: 120` after a 30-50 min ISO build (3 boots x 300 s x 8 worst case = 2 h, too long): if KVM is absent, run only the default boot; time it. (3) The ISO build is reproducible enough on the runner (snapshot.debian.org availability) - already a risk of `usb-writer`.
Open question, default: raise the job timeout to 150 min if the measurement says so.
