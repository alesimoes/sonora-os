# Keeping the graph at 128 frames and 48 kHz against every client

Established 2026-10-12 with `tools/test-quantum-pin.sh`. Every result below
was measured, in the Debian trixie runtime container (PipeWire 1.4.2,
WirePlumber 0.5.8, `pipewire-jack`, dummy driver, no hardware) on Docker
Desktop / WSL2 6.6.87.2, unless it says otherwise. PipeWire behaviour is
explained from the 1.4.2 sources (`src/pipewire/context.c`, `settings.c`,
`impl-node.c`, `pipewire-jack/src/pipewire-jack.c`), read after measuring.

## Why it matters now

ADR-0007 M2 makes a buffer-size or rate change during a take fatal to the
take. So "the default quantum is 128" is not a pin: any client that can move
the graph for one cycle ends the take.

## Method

An observer JACK client (`tools/jack-quantum-probe.py`, ctypes on the
`libjack.so.0` that `pipewire-jack` provides, as the engine uses) runs for 5 s
and records the size of every block it is handed and every buffer-size and
rate callback. One second in, an intruder tries one route to another
quantum or rate for 2 s. Any block other than 128 frames, or any rate
callback other than 48000, fails the scenario.

## Results

With the drop-in as it was (`min-quantum 64`, `max-quantum 128`,
`allowed-rates [ 48000 96000 ]`), 11 of 17 scenarios (16 routes plus a
baseline with no intruder) moved the graph:

| Route | Old drop-in | New drop-in | What closed it |
|---|---|---|---|
| `PIPEWIRE_LATENCY=64/48000` | held | held | `node.lock-quantum`, which pipewire-jack sets by default |
| `jack_set_buffer_size(64)` | **64-frame blocks** | held | `node.rules` deletes `node.force-quantum` |
| `jack_set_buffer_size(256)` | **256** | held | same |
| same, with `jack.global-buffer-size` | **64** | held | `settings.check-quantum` |
| `PIPEWIRE_QUANTUM=64/48000` | **64** | held | `node.rules` |
| `PIPEWIRE_QUANTUM=256/48000` | **256** | held | `node.rules` |
| `pw-metadata clock.force-quantum 64` | **64** | held | `settings.check-quantum` |
| `pw-metadata clock.force-quantum 256` | **256** | held | `settings.check-quantum` |
| `pw-metadata clock.min-quantum 64` + `clock.quantum 64` | held | held | lock-quantum, then min = 128 |
| `pw-metadata clock.max-quantum 64` | held | held | lock-quantum |
| `PIPEWIRE_RATE=1/96000` | held | held | not a force |
| `PIPEWIRE_QUANTUM=128/96000` | **96 kHz** | held | `node.rules` deletes `node.force-rate` |
| `node.force-rate=0` with `PIPEWIRE_RATE=1/96000` | **96 kHz** | held | same; 0 means "force my node.rate" |
| `jack_set_sample_rate(96000)` | **96 kHz** | held | same |
| `pw-metadata clock.force-rate 96000` | **96 kHz** | held | `settings.check-rate` + `allowed-rates [ 48000 ]` |
| `pw-metadata clock.rate 96000` | held | held | not a force |

New drop-in: 17 of 17 hold (`QUANTUM RESULT pass`). The engine's own
recording test (`tools/test-engine-docker.sh`, 18 tracks) still passes on it:
`buffer=128 xruns=0 overruns=0`.

Why min = max did not suffice on its own: in `context.c` a forced quantum
(node or metadata) replaces min and max for that recalculation. Only
`quantum-floor` and `quantum-limit` bound it. `allowed-rates` likewise does
not bound a node's `node.force-rate`.

`node.rules` in `pipewire.conf` are applied by the server in
`impl-node.c:check_properties()` every time a node's properties change, after
the client's update. A client cannot outrun them. Client-side `jack.rules`
would not do: pipewire-jack applies `PIPEWIRE_QUANTUM` after them, and
`jack_set_buffer_size()` changes properties at run time.

## Known gaps, measured

Reported by the test as `QUANTUM gap`, not failures:

- `pw-metadata clock.max-quantum 256`, then `clock.force-quantum 256`: the
  graph runs at 256. The metadata keys that set the limits are not checked,
  and `check-quantum` checks against the limits as they are now.
- `pw-metadata clock.allowed-rates '[ 48000 96000 ]'`, then
  `clock.force-rate 96000`: the graph runs at 96 kHz.

Both need a deliberate two-step edit of the settings metadata; no JACK call
makes it. Closing the first would take `default.clock.quantum-limit = 128`,
which also sizes server-side buffers, including the ALSA node's and the
resampler's for a device that cannot run at 48 kHz. Not shipped without
hardware to try it on. Nothing in 1.4.2's configuration closes the second.

## Decisions taken from this

- `clock.force-quantum` is no longer an operator override: `check-quantum`
  refuses every value except 128. Changing the block size is an edit to
  `audio-stack/pipewire-sonora.conf` and a PipeWire restart, between sessions.
- 96000 is out of `allowed-rates`. A device or stream at another rate is
  resampled; the graph does not move. Recording at 96 kHz becomes a
  configuration change, not a negotiation.

## In QEMU

`tools/test-boot-qemu.sh` on the rebuilt ISO (same day, `6.12.111+deb13-rt-amd64`,
QEMU with WHPX) now asks the running server rather than grepping the file:
`pipewire quantum/min/max 128/128/128, rate|allowed 48000|[ 48000 ]`, 20 of
20 checks. Stock `pipewire.conf` raises `min-quantum` to 1024 under a
hypervisor; `settings.c` clamps it to `max-quantum`, so it lands on 128. That
rule is also why no earlier QEMU boot could have shown the old 64 minimum.

## Not verified

- A real ALSA driver. The dummy driver has no period-size opinions; a USB
  interface may. First run on the UMC1820 should repeat this test with the
  interface as driver.
