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
ADR-0095 (reserved), written in the same PR. Target shape: boot steps inside nightly.yml's `usb-writer` job, sharing its ISO build. INTERIM (this PR): a new `.github/workflows/nightly-boot.yml` (schedule + `workflow_dispatch`) that builds the ISO and runs `tools/test-boot-qemu.sh` unchanged, because the daemon found `nightly.yml` held by active feature architecture.adr0007.m1_docs and `tools/test-boot-qemu.sh` by pr-442. Cost: a second ISO build (30-50 runner min) per night until a follow-up BACKLOG line folds the steps into usb-writer and deletes the file, after those two land. Rejected: ISO as cross-job artifact (~1 GB/night); every PR (ADR-0061); waiting for both features (the test stays unrun).
Failing loudly: boot steps not `continue-on-error`; without `/dev/kvm` only the default boot runs (TCG) and a `::warning` names the skipped `--gui*` boots. A red nightly opens a fix task; it never blocks a PR.
Reserved numbers: ADR 95, protocol none, format none.

## 4. DAW core
N/A.

## 5. Plugins, DSP, host
N/A.

## 6. UI
N/A.

## 7. Tests
The deliverable is the CI step. `tools/test-boot-qemu.sh` is reused UNCHANGED (pr-442 owns it); the workflow finds the serial logs by glob. Run `shellcheck` and `tools/pre-pr.sh`.

## 8. QA
Trigger the nightly with `workflow_dispatch` is not available (schedule only): if the workflow has no dispatch trigger, add `workflow_dispatch:` so the first run can be exercised by hand. Check timings of the first real run.

## 9. Checks and risks
Unverified assumptions (hand to researcher): (1) GitHub `ubuntu-24.04` runners expose `/dev/kvm` to docker `--device`; measure with a one-off job `ls -l /dev/kvm`. (2) The default boot under TCG fits the job's `timeout-minutes: 120` after a 30-50 min ISO build (3 boots x 300 s x 8 worst case = 2 h, too long): if KVM is absent, run only the default boot; time it. (3) The ISO build is reproducible enough on the runner (snapshot.debian.org availability) - already a risk of `usb-writer`.
Open question, default: raise the job timeout to 150 min if the measurement says so.
