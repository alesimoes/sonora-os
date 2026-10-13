# Handoff: feature/usb-and-quantum-followups

**Goal.** (1) Pin the PipeWire quantum/rate so no client can move it (an M2 take dies on a change). (2) write-usb.sh review follow-ups a-f.
**State.** Both done and verified in container + QEMU. Committed locally, not pushed. Nothing on real hardware.

**Commits** (on origin/main 1031fa1): `678b9b0` quantum pin + test + CI job; then write-usb hardening; then planning/handoff.

**Verify**
- `tools/test-quantum-pin.sh` -> `QUANTUM RESULT pass` (17 ok, 2 `QUANTUM gap` lines expected)
- `WRITE_USB_BOOT=0 tools/test-write-usb.sh` -> `WRITE-USB TEST PASS (container half only...` (82 ok); full run -> `WRITE-USB TEST PASS` (94 ok)
- `tools/test-boot-qemu.sh` -> `BOOT TEST PASS`, 20 SMOKETEST ok, `pipewire quantum/min/max 128/128/128, rate|allowed 48000|[ 48000 ]`

**Done, with evidence (all 2026-10-12/13, Docker Desktop WSL2, PipeWire 1.4.2, dummy driver)**
- Old drop-in: 11 of 16 client routes moved the graph (64/256 frames, 96 kHz). New: min=max=default=128, allowed-rates [48000] (96000 removed on purpose), quantum-floor 128, settings.check-quantum/check-rate, server `node.rules` deleting node.force-quantum/force-rate. All 16 hold. Table: brain/research/quantum-pin.md.
- Decision: `clock.force-quantum` via pw-metadata is NOT an operator override any more (check-quantum refuses anything but 128). ADR-0003 addendum.
- test-engine-docker.sh on new conf: `buffer=128 xruns=0 overruns=0`, 18 tracks. ISO rebuilt; QEMU smoke test now queries the live server.
- write-usb.sh: (a) wipe = perl on /proc/self/fd/3 O_RDWR|O_EXCL, zeroes first+last MiB (seeded tail pattern gone, backup GPT gone); (b) after find_partition: fd 5 checked vs sysfs, diskseq recheck, mkfs/mount via /proc/self/fd/5; (c) fd 4 opened `<` (no O_CREAT), fd 3 = /proc/self/fd/4 (strace: no O_CREAT, no write-mode open of the disk by name); (d) ro=1 refused before prompt, `$((16#))` guarded (closed/regular fd tested); (e) probe: EBUSY = in use, ENOENT/ENXIO reported by name (ENOMEDIUM untestable); (f) `flock -x` on fd 3 from before wipe to after sfdisk (strace order asserted; foreign lock -> refuses, nothing written). sfdisk also goes through /proc/self/fd/3.
- shellcheck clean under 0.9.0 (ubuntu:24.04) and 0.10.0 (debian:trixie). Exec bits 100755.

**Open**
- Known gaps (measured, reported not failed): pw-metadata clock.max-quantum 256 then force 256; allowed-rates [48000 96000] then force-rate 96000. quantum-limit=128 would close the first; not shipped (sizes ALSA/resampler buffers; needs hardware). BACKLOG.
- Repeat quantum routes with the UMC1820 as driver. Physical stick still never written. udev honouring flock is documented systemd behaviour, not observed (no udev in container).

**Do not redo**
- min=max alone does not pin: forced quantum bypasses min/max (context.c); only floor/limit clamp it. Client-side jack.rules cannot block PIPEWIRE_QUANTUM (applied after them).
- Run containers by image ID: sonora-runtime:trixie tag is shared across worktrees and was rebuilt from another checkout mid-run (first measurements here were silently on the wrong config).
- `yes | head` and `dd | grep -q` under pipefail give false results; harness uses process substitution / grep -c.

**Pointers** audio-stack/pipewire-sonora.conf, tools/test-quantum-pin.sh, tools/test-quantum-entrypoint.sh, tools/jack-quantum-probe.py, tools/write-usb.sh, tools/test-write-usb-entrypoint.sh, brain/research/quantum-pin.md, brain/research/usb-stick-layout.md ("Writing only through verified descriptors"), logs in build-quantum/ (gitignored).
