# Audio stack

Configuration shipped into the image. These files are the difference between a
machine that records and a machine that drops samples.

| File | Installed to | Purpose |
|---|---|---|
| `limits-audio.conf` | `/etc/security/limits.d/95-audio.conf` | RT priority and locked memory for the `audio` group |
| `pipewire-sonora.conf` | `/etc/pipewire/pipewire.conf.d/99-sonora.conf` | Pins the rate and the quantum |
| `wireplumber-sonora.conf` | `/etc/wireplumber/wireplumber.conf.d/99-sonora.conf` | Pro Audio profile, no remembered state, and the UMC1820 rule (no suspend-on-idle, period 128); `brain/research/umc1820-profile.md`, `tools/test-umc-profile.sh` |
| `sysctl-sonora.conf` | `/etc/sysctl.d/99-sonora.conf` | Kernel knobs that affect latency |

The PipeWire these files configure is 1.6.9 with WirePlumber 0.5.12, from
trixie-backports at exact versions (`iso/README.md`, "Versions").

## Why each knob

**`rtprio 95`** — the audio thread must preempt everything else. Not 99, so a
runaway thread still leaves room for the kernel threads needed to recover.

**`memlock unlimited`** — a page fault inside the callback is a disk wait
inside the callback. Locking buffers into RAM is what makes the
no-blocking rule hold in practice rather than on paper.

**`threadirqs`** (a kernel parameter, set in `iso/auto/config`) — forces
interrupt handlers into threads that the scheduler can prioritise. Without it a
busy USB or network interrupt runs ahead of the audio thread.

**Pinned quantum and rate** — the quantum is our block size. If PipeWire
renegotiates it mid-session, latency compensation shifts under a take in
progress, and since ADR-0007 M2 the engine ends the take. min, max and default
are all 128 and the only allowed rate is 48000; `node.rules` and
`settings.check-*` stop clients from forcing anything else. Every route tried,
and the two that still get through, are in `brain/research/quantum-pin.md`;
`tools/test-quantum-pin.sh` re-runs them. After editing any file here, run
`tools/sync-audio-stack.sh` so the image gets the same copy.

**Pro Audio profile** — the one that decides whether a multichannel interface
exists. In the default profile a card is a stereo pair, so a Behringer UMC1820
offers two channels and the other sixteen are not visible to the engine at all:
not silent, not an error, absent. Pro Audio exposes every channel as its own
port with nothing in the signal path.

## Runtime tuning (installed system)

`sonora-rtirq` (xHCI IRQ threads to SCHED_FIFO 85), `sonora-cpu-latency`
(performance governor, PM QoS latency held while the engine runs) and the udev
rule that keeps USB audio devices out of autosuspend live in
`iso/config/includes.chroot`, with the reasoning and sources in
`brain/research/rt-tuning.md`. Test: `tools/test-rt-tuning.sh`.

## Not configured here, and deliberately

`isolcpus` / `nohz_full` depend on the core count of the machine; they are
documented as optional boot options in `brain/research/rt-tuning.md`, never
default.
