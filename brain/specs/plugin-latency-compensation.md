# Real-time plugin playback and latency compensation

The owner's requirement, stated 2026-10-11: play every recorded track with its
plugin chain inserted, in real time, synchronised for latency, to produce the
mixed bundle. This note records what that actually demands, because it is the
hardest problem in the project and the naive version of it does not work.

## Why this is hard, in numbers

At quantum 128 and 48 kHz the callback budget is **2.67 ms**. Everything has to
fit: reading 18 tracks from disk, running every plugin on every track, summing
the buses, and handing the result back. Miss it once and there is a hole in the
monitoring; miss it during a take and the recording has a hole.

With 18 tracks and, say, four plugins each, that is 72 plugin instances inside
2.67 ms. Serially at 30 µs per instance it already does not fit, and a
convolution reverb is not 30 µs. So this is not an optimisation to apply later:
the processing graph has to be parallel from the start, and plugins must not be
able to block each other.

## Plugin delay compensation

Every plugin reports a latency: a look-ahead limiter, a linear-phase EQ or an
oversampling saturator does not return the sample it was given, it returns one
from N samples ago. If track 1 has 2048 samples of plugin latency and track 2
has none, they arrive at the bus 42 ms apart. A drum kit mixed that way is
unusable, and the error is not obvious on a meter — it sounds like bad playing.

The rule: the engine delays every other path so all of them arrive aligned on
the longest one. That is PDC, and three properties matter.

**It must be sample-accurate.** Not "close", not interpolated. A one-sample
error between a kick's close mic and its overhead is a comb filter.

**It must be recomputed when the graph changes, without a glitch.** Adding a
plugin changes the whole alignment. The recomputation happens off the audio
thread, and the new plan is swapped in atomically — the callback never computes
a delay graph.

**It must be reported honestly.** The operator needs to see the latency a track
carries, because at some point they will want to bypass the plugin causing it
rather than accept the delay. A number the system knows and does not show is a
number the engineer discovers during a session.

Recording is unaffected: capture is pre-fader and pre-insert by design (see
`engine/src/session.cpp`), so what lands on disk is what came in. PDC is a
playback and monitoring problem only. That separation is worth protecting.

## The conflict with out-of-process hosting

`.claude/agents/plugin-engineer.md` requires plugins to run out of process,
because a plugin is third-party code inside a recorder and an in-process crash
loses a take. That is the right call and it collides with a 2.67 ms budget.

What the collision forces:

- The audio thread may **not** wait on another process. It hands work over
  through shared memory with the same shape as `RingBuffer`, and if a plugin
  has not returned by its deadline the engine **drops that plugin for that
  block** and carries on. A dropped plugin for one block is an audible
  artefact; a missed callback is a hole in every track.
- A plugin that misses repeatedly gets taken out of the graph and the operator
  is told which one. Silently limping is worse than failing clearly.
- The IPC cost is per block, not per plugin, so the boundary is crossed once
  with all the audio for a block rather than 72 times.

## The scheduler this needs

A dependency graph executed in parallel, with as many worker threads as there
are usable cores, all at real-time priority. Nodes become runnable when their
inputs are ready. No locks on the hot path: a work-stealing queue over
pre-allocated nodes, with completion signalled by atomics.

Per-node timing has to be measured, because the only way to tell an operator
which plugin is costing them the session is to know. Worst-case per block, not
average — an average hides exactly the spike that causes the xrun.

## Open questions, none of them answered yet

- Does PipeWire's JACK implementation let us own our own worker threads at RT
  priority, or does it expect to schedule the graph? This determines whether
  the parallel scheduler is ours or theirs.
- What is a realistic per-plugin budget on the target hardware? There is no
  measurement of anything on real hardware in this project yet.
- Does a shared-memory handoff plus a deadline actually beat in-process hosting
  for total latency, or does it only move the risk? This needs measuring before
  it is committed to, and it is an ADR when it is.
- Freeze and bounce: a track whose plugins are too expensive for real time can
  be rendered offline. That is the escape hatch every professional DAW has, and
  it should exist before the scheduler is pushed to its limit.

## Status

Nothing here is implemented. The engine has no playback, no mixing and no
plugin host. This note exists so the requirement is recorded precisely while
it is fresh, and so the design is not discovered late — PDC in particular is
the kind of thing that is cheap to design for and extremely expensive to
retrofit, because it touches the shape of the whole processing graph.
