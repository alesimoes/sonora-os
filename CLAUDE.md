# SonoraOS

A Debian-based Linux distribution that boots straight into a multitrack
recording engine written from scratch. Boots and runs from a USB stick.

## What this project is not

We do not decompile, disassemble or copy code from Pro Tools, Logic Pro or any
other proprietary DAW. What we *do* reproduce is the **workflow**: key bindings,
edit modes, comping, mixer topology. Interoperability work is limited to
published formats (BWF, AAF, MIDI) and to reverse-engineering results already
published under a free licence. Any task that asks for more than that is out of
scope — say so instead of doing it.

## Layout

| Path | What lives there |
|---|---|
| `engine/` | The recording engine. C++17, CMake. Real-time audio core. |
| `iso/` | `live-build` configuration producing the bootable USB image. |
| `audio-stack/` | PipeWire, RT limits and kernel tuning shipped into the image. |
| `keymaps/` | Swappable command maps (`protools.json`, `logic.json`). |
| `brain/` | Durable knowledge: decisions, research, format specs, postmortems. |
| `planning/` | Roadmap, backlog and the current state of the work. |
| `.claude/` | Agent definitions and skills that drive the build. |
| `tools/` | Build and test scripts. |

## Rules for the real-time path

`Session::process()` and everything it calls run on the audio thread. In that
call graph, never:

- allocate or free (no `new`, no `std::vector` growth, no `std::string`)
- lock a mutex or wait on a condition variable
- touch the filesystem, log, or call anything that might
- throw

Buffers are sized when the session is created. Hand data to the disk thread
through a `RingBuffer`. A short write means an overrun — count it, never block.

## Build

Both builds need Linux. From Windows, use the container wrappers:

    tools/build-engine.sh      # cmake + ctest
    tools/build-iso.sh         # produces iso/sonora.iso

## Working in sessions

Context is the main cost: short sessions, one agent per task and review round.
The SessionStart hook (`tools/session-start.sh`) prints a starting summary
from `planning/handoffs/`; run the branch handoff's Verify command before
relying on it. Starting and stopping: `handoff` skill. Testing and keeping
output out of the context: `test-loop` skill.
Never read `planning/BACKLOG.md`, `planning/progress.json`, `brain/specs/*.md`
or an ADR whole: read the ADR's Summary, grep the heading, Read with
offset/limit. Progress goes through `tools/progress.sh get|list|set`.

## Conventions

- C++17, 4-space indent, `snake_case` members with a trailing underscore.
- Comments explain *why*, never *what* the line already says. No comment that
  restates the code.
- Every bug fixed in the real-time path gets a regression test first.
- Decisions that shape the architecture go in `brain/decisions/` as an ADR
  before the code lands.
