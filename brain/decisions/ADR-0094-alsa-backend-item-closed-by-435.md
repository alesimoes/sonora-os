# ADR-0094 — drivers.backends.alsa closed by reference to #435

Status: accepted (2026-11-08)

## Context
The backlog item "Direct ALSA backend" was open, but `drivers/alsa/`
(`AlsaBackend`, fake card, convert/proc/backend tests, `--alsa-*` flags in
`engine/app/src/main.cpp`) landed in #435 and is decided in ADR-0053.

## Decision
Do not build it again. Close the item by reference to ADR-0053 and #435. ADR-0003
(PipeWire plus the JACK API) stays the default backend; ALSA is the opt-in
`alsa` backend.

## Rationale
A second implementation would duplicate tested code and fork the backend API.

## Consequences
- No code change. Hardware behaviour (xruns on a real UMC1820) stays unverified
  until measured on the target; the researcher owns that measurement.

## Alternatives rejected
- Re-implement behind PipeWire's ALSA plugin: adds a server hop, which is what
  ADR-0053 avoids.
- Leave the item open: it would be dispatched again and duplicate work.
