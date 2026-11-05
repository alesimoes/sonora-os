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

## Bounce Mix never runs on the audio thread

File > Bounce Mix writes the mix to one stereo BWF in `Bounced Files/`
(`mix.bounce`, protocol 49; `contracts/api/include/sonora/api/bounce.h`).
Offline, a worker thread plays a private `Session` and renders as fast as the
disk reader serves it; it hosts no plug-in inserts, so a session with an
active insert is refused and the operator is told to bounce in real time. In
real time the live audio thread pushes the master into a `MasterTap` ring
(only while rolling, no allocation) and a worker drains it to disk. A full
ring, a block the disk reader missed, or a non-finite sample under normalise
fails the bounce and removes the file: a file with a hole is never kept. How
it ended is a held `Bounce` event, shown as a notice until dismissed.

The file is a BWF, WAV, AIFF or FLAC at 16, 24 or 32-bit float, at the
session's rate or another, with TPDF or shaped dither when the depth is cut
(`mix.bounce`, protocol 61, ADR-0054). The mix is rendered once to a float
file and converted by the worker when the loudness gain is known.

## Render in place rolls the live chain

Track > Freeze, Commit and Process Selection (`track.freeze`, `track.commit`,
`track.process_selection`, protocol 56, ADR-0051) print one track through its
own plug-in inserts. The hosts are other people's code in processes of their
own and run only at the audio callback, so the print is made in real time: the
audio thread pushes the chain node's output into a `TrackTap` ring (no
allocation, only while rolling) and a worker drains it to a Float32 file,
dropping the chain's latency so the file lands where the live track played. A
block the chain did not play, a host miss, a full ring or an edit during the
render fails it and removes the file. The result is one undo step; a freeze
keeps a record of what it replaced and Unfreeze puts it back.

## Failing at start-up rather than during a take

`main` refuses to start when the server sample rate differs from the session
rate. Recording at the wrong rate produces files that play at the wrong pitch
and are discovered hours later. A clear refusal at launch is cheaper than a
ruined session, so the engine prefers not to run over running wrongly.

## What is not built yet

No playback, no mixing, no monitoring path, no timeline, no undo, no plugins,
no session file. Phase 1 proves the capture path; see `planning/ROADMAP.md`.
