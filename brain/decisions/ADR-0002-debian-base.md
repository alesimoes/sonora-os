# ADR-0002 — Debian stable as the distribution base

- Status: Accepted
- Date: 2026-10-11

## Context

The image has to be predictable. A studio machine that boots differently after
an update is worse than one that lacks a feature. Candidates were Debian
stable, Ubuntu LTS and Arch.

## Decision

Debian stable (trixie), built with `live-build`.

## Rationale

- Frozen package set for the life of a release: the image we test is the image
  that ships. The Arch rolling model gives us a different kernel every month,
  which is the opposite of what an audio appliance wants.
- `live-build` is the native, well-documented way to produce a hybrid ISO, and
  it supports the persistence overlay we need (ADR-0005).
- `linux-image-rt-amd64` is packaged in Debian, so we do not maintain a kernel.
- Prior art: AV Linux and Ubuntu Studio both sit on this base.

## Consequences

Packages are old. Where a current PipeWire or plugin version matters, we pull
from `trixie-backports` or vendor the build — pinned explicitly, never by
tracking `unstable`.
