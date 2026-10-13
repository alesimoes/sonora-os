# 2026-10-13 — loopback playback null: two blocks swapped by a skipped period

## Symptom

The PLAYBACK check in `tools/test-engine-entrypoint.sh` (play track 1's
take on the monitor bus, record `sonora:out_1` patched into `sonora:in_2`,
null the second take against the first) failed now and then on PR #31: 2
of 9 runs at 34e6945, with xruns, overruns and misses all 0. The take nulled
at two periods of lag everywhere except one spot.

## What it was not

It predates the review fixes of 477e3fe. A loop running only the playback
check reproduced it at 8412b5e (4 failures in 60) as well as at 34e6945
(1 in 60), and much more often with two or four containers sharing the
host (8 to 10 in 100). The 0-in-5 at 8412b5e was luck. So there was nothing
to bisect between the two commits.

## Root cause

The wrong spot is always two adjacent 128-frame blocks, swapped: one is
`out_1` from one period earlier than the rest of the take, the next from
three periods earlier. Track 1's tone is 250 Hz, a 192-frame cycle, so a
single-lag fit cannot see this. It needs a fit per block, or the sine
recurrence `x[n] = 2cos(w) x[n-1] - x[n-2]`, which every splice breaks.

A scratch build of `drivers/jack` (never committed) copied each period's
`out_1` after `process()` and `in_2` before it, with both buffer addresses
and `jack_last_frame_time`:

- PipeWire's JACK layer gives the self-linked `in_2` the same buffer as
  `out_1`, one of two in turn (A, B, A, B). So `in_2` reads what the engine
  wrote into that buffer two periods earlier. That is the loop latency.
- In every failure, `jack_last_frame_time` stepped by 256 or 384 frames in
  one period: PipeWire's dummy driver, woken late, skipped a cycle. The
  turn went A, A, B, so `in_2` read data one period old and then three
  periods old. No xrun callback came.
- The engine's `out_1`, as it left the callback, had no splice in any
  failure. The engine never reads `frame_time`.

Giving the container `rtprio 95` (PipeWire's data loops at SCHED_FIFO 88)
did not help: 7 failures in 100 with RT against 3 in 100 without, on the
same host at the same time. The late wakeups come from the Docker Desktop
VM, not from scheduling inside it.

## The engine's part

`FrameClock` counted only backward steps. The contract's own check
(`frame_time_never_decreases`: every gap excused by an xrun or a clock
step) fails on such a skip, and `RESULT` said `xruns=0 clock_steps=0` for a
run in which the server had not run for a period. `FrameClock::widen()`
now takes the period length and the server's xrun count. A forward step
longer than the last period, with no xrun reported, is a clock step:
reported, not failed (ADR-0007, update of 2026-10-13). `FakeBackend`
gained `inject_unreported_skip()`.

## The harness's part

A self-linked port cannot stay null across a skipped period, and on a busy
host a 3-second take can hold a dozen of them, so recording the take again
was not enough (one full run failed three attempts in a row). When the
engine counts clock steps, `verify-recording.py --check-playback
--clock-steps N` lets at most 2N blocks null exactly one period early or
late. Every other block must null at the loop latency, as before. A take
that still fails is recorded again, up to three times, only if steps were
counted. A null that fails with no clock step fails at once. Track 1's
tone repeats every 192 frames, so the excuse cannot tell a period from 64
frames. `engine/control`'s `test_playback_loopback` holds the engine's
own timing to every frame with no excuse (300 of 300 rounds exact under
jitter, stalls and three-way CPU contention).

## Lessons

- A null test over a periodic tone hides shifts of whole tone cycles, and
  a single best lag hides a local swap. Check continuity per block.
- When a loopback fails, record both ends inside the callback before
  blaming either. A tap through another client (pw-record) splices on its
  own under load and proves nothing.
- 0 failures in 5 runs against 2 in 9 is not a difference. Loop until the
  rates separate, and loop both commits at once, under the same load.
