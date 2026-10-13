# Architecture

## Threads

Three, and the boundary between them is the whole design.

    ┌─────────────────────────────────────────────────────────────┐
    │ audio thread            (PipeWire, RT priority 88)          │
    │                                                             │
    │  JackBackend::Impl::process_cb                              │
    │    └── Session::process        (via ProcessCallback)        │
    │          ├── peak metering          → atomic per track      │
    │          └── RingBuffer::write      → lock-free handoff     │
    │                                                             │
    │  never: allocate, lock, block, log, do I/O, throw           │
    └──────────────────────────┬──────────────────────────────────┘
                               │  RingBuffer<float>, one per track
                               │  SPSC, power-of-two, cache-line split
    ┌──────────────────────────▼──────────────────────────────────┐
    │ disk thread             (normal priority)                   │
    │                                                             │
    │  DiskThread::drain_once                                     │
    │    └── WavWriter::write    → buffered stdio, may block      │
    └─────────────────────────────────────────────────────────────┘
    ┌─────────────────────────────────────────────────────────────┐
    │ main thread             (normal priority)                   │
    │  keyboard input, meters, transport commands via atomics     │
    └─────────────────────────────────────────────────────────────┘

The audio thread is the only writer of the playhead and the only producer into
the capture rings. The disk thread is the only consumer. Nothing else is
shared except atomics.

## Why a ring buffer and not a queue

A lock-free queue would allocate per message. The ring is allocated once, when
the session is created, sized from `SessionConfig::ring_seconds` — four seconds
by default, which is how long the disk thread may stall before a take is
damaged. Write and read indices are plain `size_t` counters that are allowed to
overflow; the index into storage is a mask, which is why capacity is rounded up
to a power of two.

The two indices sit on separate cache lines. Without that padding, the producer
and the consumer invalidate each other on every block, which costs more than
the copy does.

## Overruns are counted, never waited on

When a ring has no room, `RingBuffer::write` writes what fits and returns a
short count. `Session::process` turns that into `Track::note_overrun` and
returns. The alternative — waiting for space — would turn a storage problem
into an xrun and a hole in every other track as well.

This is why the display shows xruns and overruns as separate numbers. An xrun
means the callback was late; an overrun means the disk could not keep up. They
lead to different fixes.

## Capture is pre-fader

`Session::process` writes the input to disk before applying gain. The fader is
a monitoring decision and must not be baked into the file — an engineer who
rides the monitor level during a take has not asked to alter the recording.

## Failing at start-up rather than during a take

`main` refuses to start when the server sample rate differs from the session
rate. Recording at the wrong rate produces files that play at the wrong pitch
and are discovered hours later. A clear refusal at launch is cheaper than a
ruined session, so the engine prefers not to run over running wrongly.

## What is not built yet

No playback, no mixing, no monitoring path, no timeline, no undo, no plugins,
no session file. Phase 1 proves the capture path; see `planning/ROADMAP.md`.
