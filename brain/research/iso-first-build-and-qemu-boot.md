# First image build and QEMU boot (2026-11-03)

Measured, not hardware: a virtual machine. Nothing here says anything about
the UMC1820 or about xruns on a real interface.

## Build host

Windows 11 (10.0.26300), Docker Desktop 20.10.14, 16 CPUs and 31 GiB visible
to the VM; `tools/build-iso.sh` with `SONORA_WITH_UI=1`, the
`sonora-livebuild-work` cache volume already holding the bootstrap stage.

| Run | What | Wall time |
|---|---|---|
| 1 | engine build plus full ctest: stopped by `native_fet` (a 192 kHz worst-block CPU bar, 705 us against 667 us, under load from other agents' containers) | 2665 s, no image |
| 2 | stage-only engine and UI builds, then live-build | 1319 s |
| 3 | same, engine and UI trees warm | 835 s |
| 4 | same, rebased on main (display-fallback smoketest changes) | 1012 s |

The image is 978 MB. PipeWire `1.6.9-2~bpo13+1` and WirePlumber
`0.5.12-1~bpo13+1` were installed from the 20261101T000000Z snapshot; kernel
`6.12.115+deb13.1-rt-amd64`. C: went from 65 GB to 60 GB free.

## Boot

`tools/test-boot-qemu.sh` with no QEMU on the Windows host, so QEMU 10.0.13 ran in
the `sonora-qemu:trixie` container on the Docker VM's `/dev/kvm` (nested
virtualisation, KVM accelerated). 4 GiB, 2 vCPUs, intel-hda + hda-duplex.

- Kernel to `SMOKETEST RESULT`: 40.5 s guest time; 48 s wall time for the whole run.
- PipeWire quantum/min/max 128/128/128, rate 48000, allowed `[ 48000 ]`.
- Capture ports: 2 (the emulated codec, pro-audio profile applied).
- Engine pid stable over 10 s, 0 session restarts.
- No VT login prompts (logind `NAutoVTs` = 0), no serial getty.
- Xruns: not measured. The smoke test does not count them, and an
  emulated HDA codec under TCG/KVM timing would not be evidence anyway.

The first boot (run 2's image) failed one check, the sonora.gui conditions
check, which read a property systemd does not expose, and showed
`BUILD_ID unstamped`. Both were fixed before run 3, which passed all 27 checks.
Run 4's image, with main's added display-stack checks, passed all 31: 22.9 s
guest time to the verdict, 25 s wall for the boot. The figures above are from
run 3.

## Cold build through snapshot.debian.org (ADR-0002 amendment applied)

Same host. trixie, trixie-updates, trixie-security and trixie-backports all
from the `20261101T000000Z` snapshot, a fresh Docker volume (no
`cache/bootstrap`, no package cache), `tools/build-iso.sh` without the UI.

| Stage | Wall time |
|---|---|
| Engine build (stage-only) from a cold worktree, plus image copy-out | 906 s (total minus live-build) |
| `lb bootstrap` (debootstrap from the snapshot) | 5 min 30 s |
| `lb chroot` (RT kernel, firmware, PipeWire, hooks) | 11 min 53 s |
| live-build total, `lb clean` to ISO | 23 min 02 s |
| Whole script | 2288 s |

No rate-limit errors or retries from snapshot.debian.org in the log. The
image is 976 MB. Installed: kernel `6.12.113-1` (`6.12.113+deb13-rt-amd64`),
PipeWire `1.6.9-2~bpo13+1`, WirePlumber `0.5.12-1~bpo13+1`. The run-3 image
above, built from the live mirror, had `6.12.115+deb13.1`: the kernel in that
image depended on the build date, and now it depends on the timestamp.

In the ISO's squashfs: `/etc/apt/sources.list` names `deb.debian.org` and
`security.debian.org/debian-security`, and no file under `/etc/apt` sets
`Check-Valid-Until` (the `.conf.chroot` override was removed before packing).

`tools/test-boot-qemu.sh` on that image (KVM in the Docker VM, as above):
BOOT TEST PASS, 31 ok, kernel `-rt-`, 65 s wall. Xruns not measured.
