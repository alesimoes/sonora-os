---
name: test-loop
description: Pick the cheapest test that can prove the change, and keep test output out of the context. Use before running any test or build in this repository.
---

# Test with the fast loop, prove with the full one

Iterate on the fast loop. Run the full loop once, at the end, before handing
back or opening a PR. Never iterate on the full loop.

| Area changed | Fast loop (iterate) | Full loop (once, at the end) |
|---|---|---|
| `engine/` | `tools/build-engine.sh` (incremental, cmake + ctest) | + `tools/test-engine-docker.sh` (seconds) |
| `iso/`, `audio-stack/` | inspect the built ISO with `xorriso` in the livebuild container | `tools/build-iso.sh` (about 10 min with cache), then `tools/test-boot-qemu.sh` (about 2 min) |
| `tools/write-usb.sh` | `WRITE_USB_BOOT=0 tools/test-write-usb.sh` (about 2 min, container only) | `tools/test-write-usb.sh` (about 12 min, five QEMU boots) |
| any `tools/*.sh` | `bash -n` on the file | shellcheck as CI runs it (below) |

Shellcheck as CI runs it:

    docker run --rm -v "$(pwd -W):/mnt:ro" -w /mnt koalaman/shellcheck:stable \
      tools/*.sh iso/auto/config iso/config/hooks/normal/*.hook.chroot

Timings were measured on the owner's Windows host with Docker Desktop and
QEMU/WHPX (2026-10-12). Use them to choose; do not quote them as results.

## Output stays out of the context

- Redirect every long run to a file under the ignored `build-*/` directory
  and read back only the verdict:

      tools/test-write-usb.sh > build-usb/run.log 2>&1; echo "exit=$?" >> build-usb/run.log
      grep -E "FAIL|RESULT|TEST (PASS|FAIL)|^exit=" build-usb/run.log

  Open more of the log only when something failed, and then only around it.
- Long runs go in the background; wait with an until-loop on the `exit=` line,
  not with repeated reads.
- Do not edit a file a running test reads. Bash reads scripts as it goes, so
  an edit corrupts the run, and the run has to be thrown away. Finish the
  edits, then run.
- Read big files by line range. `grep -n` first to find the range.

## A result you can report

The verdict line, the count, and the log path, for example
`tools/test-write-usb.sh: 81 ok, 0 FAIL, build-usb/run.log`. If a test could
not run, say so and call the code unverified.
