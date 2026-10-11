# 2026-10-11 — debootstrap tar failure on the first ISO build

## Symptom

`tools/build-iso.sh` ran for several minutes, extracted most of the Debian
base, then stopped:

    I: Extracting libpam-runtime...
    E: Tried to extract package, but tar failed. Exit...
    E: An unexpected failure occurred, exiting...

No ISO was produced.

## Root cause

The container had the repository bind-mounted at `/src` and `live-build` ran
with that as its working directory, so `debootstrap` built the chroot under
`iso/chroot` — on the Windows host filesystem, reached through the Docker
Desktop bind mount.

`debootstrap` unpacks packages with `tar` and needs real ownership, hardlinks,
device nodes and extended attributes. A Windows bind mount provides none of
those, so `tar` fails partway through, and the error says only that tar failed
rather than naming the filesystem.

## Fix

The build now happens in a Docker named volume mounted at `/work`, which is
ext4 inside the Linux VM. The repository is bind-mounted read-only at `/src`,
the configuration is copied from it on every run, and only the finished ISO is
copied back to the host. `cache/` lives in the volume, so a rebuild does not
re-download the Debian base.

## Second defect found while investigating

The script reported success. The background invocation was
`bash tools/build-iso.sh > log 2>&1; echo "exit=$?"; tail -30`, and the exit
code of a `;`-separated list is the exit code of the last command — `tail`,
which always succeeds. The real `exit=1` was in the output but the harness
reported the task as exit code 0.

The lesson is about reporting, not about the script: when a long build runs in
the background, read the log for `^E:` or `error`, and do not take the task
exit code as the build result unless the build is the last command in the
chain.

## Third defect found while investigating

Nothing installed the engine into the image. `sonora-session.service` runs
`/usr/local/bin/sonora-engine`, and no package list, hook or include ever put
a binary there. The image would have booted into a `Restart=always` loop
against a missing file — and because the first build failed earlier, for an
unrelated reason, this would not have surfaced until someone booted a stick.

`tools/build-iso.sh` now builds the engine and installs it into
`config/includes.chroot/usr/local/bin/` before calling `lb build`. The binary
is gitignored: it is an artefact, not source.

The engine is built in the Debian trixie container and runs on a Debian trixie
image, so it links against the same `libjack-jackd2-0`. If the image base and
the build base ever diverge, this breaks at boot with a missing-symbol error.

## Guard against recurrence

There is no unit test for an ISO build. What replaces one:

- `tools/build-iso.sh` fails loudly if no engine binary is found to stage.
- The ISO build runs on the host filesystem never again — the volume is in the
  script, not in a command someone has to remember.
- Open item for the backlog: a boot smoke test in QEMU that asserts the
  engine process is running, so a missing binary fails in CI rather than on
  someone hardware.
