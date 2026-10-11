# ADR-0003 — PipeWire as the server, JACK API as our client interface

- Status: Accepted
- Date: 2026-10-11

## Context

Our engine needs a way onto the hardware. Options: ALSA directly, JACK with
`jackd`, or PipeWire.

## Decision

Ship PipeWire as the only audio server. Our engine is a JACK *client*, linked
against the PipeWire JACK implementation (`pipewire-jack`). No `jackd` on the
image.

## Rationale

- Writing to ALSA directly means we own device enumeration, the resampler and
  the mixing of every other sound on the system. That is a project in itself.
- The JACK API gives us what a DAW actually needs: one callback, sample-
  accurate port buffers, xrun notification, and a graph other tools can patch
  into.
- PipeWire provides that API while also handling the desktop audio an
  appliance still needs (preview, browser, notifications), which `jackd` does
  not. Running both servers means fighting over the device.
- Keeping our side at the JACK API is a hedge: if PipeWire disappoints under
  load, swapping in `jackd` is a configuration change, not an engine rewrite.

## Consequences

- The PipeWire quantum is the buffer size. It must be pinned in configuration,
  not left to dynamic negotiation, or our block size changes mid-session.
- The sample rate belongs to the server. The engine refuses to start when the
  server rate differs from the session rate — recording at the wrong rate
  silently produces wrong-pitch files, which is worse than failing to start.
- We depend on the PipeWire JACK emulation being faithful. Any gap we hit goes
  in `brain/research/` with a measurement, not an impression.
