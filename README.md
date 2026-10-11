# SonoraOS

A Debian-based Linux distribution that boots straight into a multitrack
recording engine. No desktop, no login, no launching an application: power on
and the machine is a recorder. Runs from a USB stick.

The engine is written from scratch in C++17 (ADR-0001).

> **Status: early. Nothing here has been booted or compiled yet.**
> The engine code and the image configuration are written; the owner machine
> cannot run either build at the moment. Treat every claim about behaviour as
> unverified until CI is green. See [`planning/STATUS.md`](planning/STATUS.md).

## On Pro Tools and Logic Pro

The goal is to feel familiar to engineers who work in those tools. That means
cloning the **workflow** — key bindings, edit modes, comping, mixer topology —
and interoperating through **published formats** (BWF, AAF, MIDI) plus
reverse-engineering work others have already published under a free licence,
such as `libptformat` for reading `.ptx`.

It does not mean decompiling anyone else, and we do not. The boundary is
written down in
[ADR-0004](brain/decisions/ADR-0004-no-proprietary-reverse-engineering.md) and
it binds contributors and agents alike. In particular: `.logicx` has no
published specification, so we do not claim Logic project import.

## What works today

The engine captures multitrack audio to Broadcast Wave with the timecode origin
in a `bext` chunk, so takes land at the right place when imported elsewhere.
Architecture:

- Lock-free single-producer / single-consumer ring buffer per track.
- An audio callback that never allocates, locks or blocks.
- Disk I/O on its own thread, with overruns counted rather than hidden.
- A terminal display showing meters, xruns and overruns separately — they have
  different causes and conflating them sends you after the wrong bug.

## Build

Both builds need Linux; on Windows or macOS they run in a container.

    tools/build-engine.sh        # cmake + ctest
    tools/build-iso.sh           # produces iso/sonora.iso
    sudo tools/write-usb.sh /dev/sdX   # ERASES the target device

## Repository layout

| Path | Contents |
|---|---|
| `engine/` | The recording engine. C++17, CMake. |
| `iso/` | `live-build` configuration for the bootable image. |
| `audio-stack/` | PipeWire, RT limits and kernel tuning. |
| `keymaps/` | Swappable command maps. |
| `brain/` | Decisions, format specs, research, postmortems. |
| `planning/` | Roadmap, backlog, current status. |
| `.claude/` | Agent definitions and skills that drive the build. |
| `tools/` | Build and USB scripts. |

## Reading order for a newcomer

1. [`CLAUDE.md`](CLAUDE.md) — the rules, particularly for the real-time path.
2. [`brain/INDEX.md`](brain/INDEX.md) — why things are the way they are.
3. [`planning/ROADMAP.md`](planning/ROADMAP.md) — where this is going.
4. [`.claude/OPERATIONS.md`](.claude/OPERATIONS.md) — how the work gets done.

## Licence

GPL-3.0-or-later. See [`LICENSE`](LICENSE).
