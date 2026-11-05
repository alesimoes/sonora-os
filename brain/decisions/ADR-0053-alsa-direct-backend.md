# ADR-0053 — A direct ALSA backend beside PipeWire, for the lowest latency

- Status: Accepted (recommended default, taken by the coordinator with no
  owner available, 2026-11-04)
- Date: 2026-11-04
- Amends: ADR-0003 (adds a second backend; PipeWire stays the default)

## Summary

`drivers/alsa` implements the `AudioBackend` contract straight on
libasound: mmap interleaved I/O on the card, the card's period as the
engine's block, one `SCHED_FIFO` thread, no server. It is selected with
`--backend alsa` and is **opt-in**. PipeWire through the JACK API
(ADR-0003) stays the default and the only backend the image starts with.

Choose ALSA when the machine is a recorder and nothing else needs the
card: the installed system tracking at 64 to 128 frames, where PipeWire's
graph adds a period of its own. Choose PipeWire when anything else must
share the card (desktop sound, a second client, a plugin host that talks
to the graph) or when the interface may be unplugged and replugged during
a session. The two never run on one card at once: ALSA gets `EBUSY` while
PipeWire holds it, and says so.

No latency figure is claimed here. The expected gain is unmeasured; the
steps that measure it are under "Unverified".

## Context

ADR-0003 rejected ALSA directly because we would own enumeration, the
resampler and the mixing of every other sound. That reasoning is about the
desktop. On the installed appliance (project memory: "installed system is
the target, optimised for low plugin latency") the engine is the only
audio client that matters, there is no resampler to own (the session rate
is the card's rate, and `open()` refuses any other), and nothing needs
mixing. What is left of ADR-0003's argument is enumeration, which is a
page of code here (`alsa_system.cpp`).

The class-compliance rule holds: `snd-usb-audio` needs no driver of ours,
and this backend adds none. It adds a way to use the card the kernel
already exposes.

## Decision

1. **Direct, not `plughw`.** The PCMs opened are `hw:CARD=<id>,DEV=0`. No
   rate, format or channel conversion happens in libasound; a card that
   cannot do what the session asks fails with the ALSA error. The backend
   converts interleaved integer samples (S32, S24_3LE, S24_LE, S16, tried
   in that order) to and from planar float itself (`alsa_convert.h`).
2. **mmap interleaved, one period at a time.** `snd_pcm_mmap_begin` on
   capture and playback, convert, call `process()`, commit. The block size
   is the period the card granted, which can differ from the one asked for;
   the backend reports what was granted (`buffer_frames()`), and
   `PeriodGuard` makes a later change fatal as for JACK.
3. **Period and buffer negotiation.** Defaults 128 frames x 2 periods;
   `--alsa-period` and `--alsa-periods` change them. The playback ring is
   prefilled with `buffer - period` frames of silence and both directions
   are linked (`snd_pcm_link`) and started on one trigger, so capture and
   playback stay one period apart for the life of the stream. Two periods
   is the least that streams; a USB card usually needs more than two to
   run without xruns, which is why `periods` is an option and not a
   constant.
4. **Xruns are counted, never waited on.** `-EPIPE` (or a short mmap
   region, or a bad state) increments `xruns()`, restarts the stream with
   silence queued and carries on. `frame_time` jumps by at least one
   period over the lost frames; the real number is not knowable from
   userspace. Only a restart that fails is fatal.
5. **Real-time thread.** `SCHED_FIFO` at 70 (below the 95 that
   `audio-stack/limits-audio.conf` grants; `--alsa-rtprio`), named
   `sonora-alsa`. A refusal is not fatal: it is printed once with the
   likely cause and `rt_priority_granted()` reports 0. Memory locking
   stays the engine's (`memory_lock.cpp`). The thread waits at most
   100 ms per poll so `deactivate()` is bounded, and the one-second stall
   rule of the contract is `PeriodGuard`'s, unchanged.
6. **Device selection by name.** `--device` takes a card id, name, index
   or `hw:` form, case-insensitively (`resolve_card`). With no name, the
   contract's default policy applies (the one USB card). Ports are named
   `<id>:capture_<n>` and numbered by the card's channel order, so
   channel k is input k with no sorting involved. Channel counts of a USB
   card come from `/proc/asound/cardN/stream0`, because opening the PCM to
   ask fails while another program holds the card and would make "held"
   look like "absent".
7. **No server means no graph.** `connect_inputs_in_order()` is where the
   card is opened when it was not chosen or present at `activate()`, so
   the engine's existing polling loop works unchanged; a held card is
   reported there, once, on stderr. A card unplugged mid-take is fatal
   (`DeviceGone`) exactly as under JACK.
8. **Seam for testing.** libasound is behind `alsa::System` and
   `alsa::Duplex`; only `alsa_system.cpp` includes an ALSA header. The
   backend runs the shared conformance suite against a fake card, in
   ctest, without libasound. A build without libasound headers links a
   stub and `--backend alsa` refuses with a message.

## Consequences

- One more backend to keep conformant: `test_alsa_backend` runs the same
  suite as the fake and JACK backends. Faults a card cannot have (clock
  step, quantum or rate change under the stream, server loss) are skipped
  by the rig, as the contract allows.
- The engine owns the card exclusively. The desktop has no sound while it
  runs. The image must not start PipeWire's ALSA monitor on that card for
  this mode (WirePlumber's `api.alsa.*` rule for the interface, see
  `audio-stack/wireplumber-sonora.conf`); this ADR does not change that
  file and the switch is a deployment step to be written once the
  measurement below shows the gain is worth it.
- `Dockerfile.build` gains `libasound2-dev`. The runtime needs only
  `libasound2`, which `alsa-utils` already pulls in.
- `reported_latency()` returns `period + ring` frames: what the backend
  adds, excluding converters and the USB transport. It is the fallback for
  a measured round trip (ADR-0019), never a claim about the interface.

## Unverified (needs the UMC1820)

Nothing in this ADR has run against a physical card. Written from the
alsa-lib documentation and the kernel's `sound/usb` sources, tested against
a fake; `drivers/alsa/src/alsa_system.cpp` in particular has compiled but
not streamed. On the interface, on the installed system with PipeWire
stopped, record and write into `brain/research/` with model, kernel, host:

1. `/proc/asound/cardN/stream0`: channel counts per altsetting and rate
   (`parse_stream0` assumes the layout in `sound/usb/proc.c`); whether 18
   in / 20 out is offered at 96 kHz or only at 44.1 / 48.
2. Which format the UMC1820 negotiates (the code expects S24_3LE) and
   whether `snd_pcm_link` succeeds between its capture and playback.
3. `sonora-alsa-passthrough --device UMC1820 --seconds 600` at
   `--period 64/128/256 --periods 2/3`: xruns, `callback_max_ns` jitter,
   `rtprio`. The lowest setting with zero xruns over ten minutes is the
   answer, the same rule as `tools/umc-measure.sh`'s quantum sweep.
4. The round trip with a cable from an output to an input
   (`--measure-latency`, ADR-0019), compared with the same setting on
   PipeWire at quantum 128. Only that comparison says whether this
   backend earns its place.
5. Whether `htimestamp`'s `callback_ns` is steady (jitter above), and
   whether unplugging the card mid-take ends the take cleanly.
6. `tools/test-alsa-loopback.sh` on a host with `snd-aloop`: not available
   in the container this was written in.

## Not done

- `plughw` / rate conversion: out of scope by decision 1.
- Hot-replug without ending the take: the contract says a lost device is
  fatal; a session can be resumed.
- Device-specific mixer control (the UMC1820's routing is hardware-side
  and not touched).
