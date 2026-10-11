# Audio stack

Configuration shipped into the image. These files are the difference between a
machine that records and a machine that drops samples.

| File | Installed to | Purpose |
|---|---|---|
| `limits-audio.conf` | `/etc/security/limits.d/95-audio.conf` | RT priority and locked memory for the `audio` group |
| `pipewire-sonora.conf` | `/etc/pipewire/pipewire.conf.d/99-sonora.conf` | Pins the rate and the quantum |
| `sysctl-sonora.conf` | `/etc/sysctl.d/99-sonora.conf` | Kernel knobs that affect latency |

## Why each knob

**`rtprio 95`** — the audio thread must preempt everything else. Not 99, so a
runaway thread still leaves room for the kernel threads needed to recover.

**`memlock unlimited`** — a page fault inside the callback is a disk wait
inside the callback. Locking buffers into RAM is what makes the
no-blocking rule hold in practice rather than on paper.

**`threadirqs`** (a kernel parameter, set in `iso/auto/config`) — forces
interrupt handlers into threads that the scheduler can prioritise. Without it a
busy USB or network interrupt runs ahead of the audio thread.

**Pinned quantum** — the quantum is our block size. If PipeWire renegotiates it
mid-session, latency compensation shifts under a take in progress.

## Not configured here, and deliberately

CPU frequency scaling and `nohz_full` both change latency measurably, and both
depend on the machine. They belong in a tuning profile applied after we have
measured a given box, not in a default that ships blind. First measurements go
in `brain/research/`.
