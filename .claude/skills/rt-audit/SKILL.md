---
name: rt-audit
description: Audit the real-time audio path for anything that can delay the callback. Use after changes to the callback call graph, when xruns appear, and before closing a phase.
---

# Real-time path audit

An xrun is a hole in the recording. The cause is almost always that something
on the audio thread waited.

## Scope of the audit

Start at every SONORA_RT function (grep SONORA_RT); today
`JackBackend::Impl::process_cb` and `xrun_cb` in
`drivers/jack/src/jack_backend.cpp`. Follow every call transitively. That is the audit surface. Anything on it must not
allocate, free, lock, wait, perform a syscall, or throw.

For the detailed method and the list of standard-library traps, use the
`rt-safety-auditor` agent — it exists for exactly this and it is more paranoid
than a general review.

## A first pass by hand

    grep -rn "new \|malloc\|push_back\|resize\|reserve\|lock\|mutex\|printf\|fopen\|fwrite" common contracts engine drivers ui

Then decide, for each hit, which thread reaches it. Hits inside
`disk_thread.cpp` and `wav_writer.cpp` are expected and correct: that thread is
allowed to block. Hits reachable from `process()` are defects.

## Measuring rather than reading

The engine reports both numbers that matter on its own display: JACK xruns and
ring-buffer overruns. They mean different things.

- **xruns** — the callback did not return in time. Audio thread problem.
- **overruns** — the ring filled because the disk thread fell behind. Storage
  or scheduling problem, not the callback.

Reporting one as the other sends the next person in the wrong direction. Say
which you observed.

A clean read-through is not a measurement. Record a real session and report
both counts with the buffer size and the track count.
