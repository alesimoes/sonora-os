# ADR-0063 — Parallel processing graph: levels in the plan, help-first workers, nodes dropped on deadline

Status: Accepted (architect, recommended defaults; plugins.graph.parallel)

## Summary
- The builder assigns each plan node a **level** (longest path from a source); nodes of one level are independent. The plan stays in topological order, so the serial walk is still correct.
- `Session` runs levels through a `GraphScheduler` interface. The default is the serial scheduler: identical output to today, `workers = 0`.
- The parallel scheduler has N pre-created worker threads (N fixed at session creation, default 0). The audio thread **helps first**: it claims nodes itself from an atomic cursor and never sleeps. It only spins (bounded, no syscall) on a node a worker has claimed and not finished.
- At the **deadline** (a fraction of the period, default 70 %) an unfinished claimed node is **dropped**: its output is silence ramped over one block (as a missed chain, ADR-0012 §3), the drop is counted per node, and the worker's late result is discarded by a generation check.
- No wire or format change. The drop counters are `Session` accessors; surfacing them in the state block is a later protocol bump, not here.

## Context
`Session::process()` walks the node list serially (ADR-0012 §7.2; ADR-0024 §3 keeps the graph acyclic and topological). Plugin chains already run in other processes; what stays on the audio thread is source decode, region gain and fades, faders, sums and PDC delays. At 64+ strips with aux buses that serial walk is the cost that grows with the session. The backlog item (DAW 4) asks for a parallel graph "with nodes dropped on deadline". Rules: the audio thread cannot allocate, lock or block; losing a take is the worst outcome; the image must boot the same way everywhere.

## Decision
1. **Levels in the plan.** `PlanNode::level` (uint16) and `PlaybackPlan::level_first[]` / `level_count` give contiguous node ranges per level (nodes are re-sorted topologically with level as the primary key, ties by stable id so builds stay deterministic; PDC and delay-line identity are by stable id and unaffected). Edges go only from a lower level to a higher one.
2. **Capture is untouched.** Capture reads backend inputs and feeds the ring before any node runs. Parallelism touches only the playback/mix nodes. The disk thread's ring path is unchanged; a dropped node never drops a recorded sample.
3. **Help-first, no waiting primitive.** Per level the audio thread publishes `{level, epoch}` in one atomic, workers and the audio thread claim nodes with `fetch_add` on a cursor. The audio thread runs claimed work until the cursor is exhausted, then checks the level's `done` count. If a worker still holds a node it polls the count with a CPU pause until the level deadline. No mutex, no condvar, no futex wait on the audio thread.
4. **Dropping.** Past the deadline the audio thread marks each unfinished node of the level `dropped` for this block: silence for that node's output scratch, ramped from the previous block's last samples over the block. Downstream nodes run normally on the silent input. Workers write only into the node's own scratch; the audio thread reads it only if the node's `done_epoch == epoch`, so a late finish is ignored. Eight consecutive drops of one node mark it `kSilentChannelSlot`-style down: it is skipped and counted until a plan swap resets it (same rule as ADR-0012 §3).
5. **Worker wake.** Workers are timer-paced like hosts (ADR-0012 §1: futex gate off by default), `SCHED_FIFO` one below the audio thread. A `kGraphWakeEnabled` constant stays off until the wake cost is measured (Unverified 1).
6. **Fail loudly at start.** Workers, their affinity/priority and their buffers are created in `Session` creation. If the requested pool cannot start (thread, priority, memory lock), creation **fails with an error**; it never silently falls back to serial while recording.
7. **Workers never touch** the filesystem, log, allocate, throw, or call into plugin hosts. They run only node kinds marked `parallel_safe` (TrackSource, TrackFader, AuxFader fused work); `InsertChain` (ring/channel protocol, ADR-0012 §7.2), `BusSum`, `AuxSum`, `HardwareOut`, `PathOut` and delay-line writes stay on the audio thread, in plan order, between levels. This keeps PDC and the host protocol exactly as accepted.
8. **Determinism.** A level's nodes write to distinct scratch buffers and are summed later by the audio thread in plan order, so the float sum order is identical for any N. Output with N workers equals serial output bit for bit when nothing is dropped (a test).

## Contract (as built, contract slice)
- `playback_plan.h`: `PlanNode::level` (uint16, 0 until the level pass); `PlaybackPlan::level_first` (uint32 array of `node_count + 1` entries, allocated and zeroed by `create_plan()`) and `level_count` (0: no level pass, the plan is one level). `plan_level_count()` / `plan_level_range()` are what a scheduler walks.
- `graph_schedule.h`: `GraphNodeWork` (`run(node)` on any thread into the node's own scratch, `drop(node)` on the audio thread only), `GraphLevel` (plan node indices plus the absolute `deadline_ns` from `graph_deadline_ns()`), `GraphScheduler` (`workers()`, `run_level()` runs or drops each node exactly once, `reset_drops()` at a plan swap, `drop_counts()` from any thread), `GraphDropBook` (per-node streak, down after `kGraphDownAfterDrops` = 8, atomic totals `dropped` and `skipped`), `GraphSchedulerConfig` (workers, max_nodes, deadline fraction), `make_serial_graph_scheduler()`.
- The session filters a level's parallel-safe nodes (§7) into an index array sized at creation; the scheduler never sees a node kind.
- Tests inject stalls with `engine/core/tests/graph_schedule_fake.h` (serial plus `set_stalled(node)`); `engine/core` globs `src/graph_*.cpp` and `tests/test_graph_*.cpp` (ctest `graph_<name>`, label `core`).

## Rationale
Help-first means the audio thread is never worse than serial by more than the deadline poll, and with N = 0 it is serial. Bounded spin with a drop is the only way to meet a deadline without a blocking wait; the cost is a spin on the RT core, bounded by the deadline and ruled on by the auditor (Unverified 3). Keeping sums on the audio thread preserves bit-exactness and the ADR-0012/0024 behaviour.

## Consequences
- Plan gets two header fields; the plan builder gains a level pass (control thread). `session.cpp` (3.2k lines) gets only a scheduler call; new code lives in `graph_*` files.
- A dropped node is audible (a click-free dip) but never loses a take; counters make it visible. Presets of N: default 0 until measured.
- Memory: N threads and one stack each, locked at creation.

## Alternatives rejected
- **Wait on the workers (barrier with condvar/futex).** Blocks the audio thread; an unlucky wake misses the whole period. Violates the RT rule.
- **Depth-1 pipelining of nodes (host style).** Adds a block of latency to every parallel node and breaks PDC arithmetic; hosts accept it because they are out of process.
- **Parallel summing with atomic float adds.** Order-dependent output; not bit-exact across N.
- **One thread per track.** Scheduling cost and wake storms at 64 strips; levels bound it.
- **Fall back to serial silently when workers fail.** Degrades while recording; rejected by the fail-loud rule.

## Unverified (measurement, owner: researcher)
1. Worker wake cost and dropped-block rate on bare metal `rt-amd64`, quantum 128, 18 and 64 strips, N = 0, 2, 3, 4 workers, one hour with `stress-ng --futex`. Pass: zero drops at the default deadline, p99.9 callback under 70 % of the period.
2. Deadline fraction (70 % assumed) against real decode and fade cost per strip; settle with the same run's histogram.
3. Whether a CPU-pause spin on a core shared with a worker (4-core machine, UMC1820 target) hurts the worker it waits on; measure with affinity pinned and unpinned.
4. A node's `done_epoch` publication ordering on ARM (acquire/release suffices by reasoning, not run); TSan and a CI run on x86 only.

## Delivery
One feature, one PR: contract and fake -> levels builder and workers (parallel) -> session integration -> RT audit. Tasks in `planning/plans/plugins.graph.parallel.md`.
