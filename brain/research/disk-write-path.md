# Disk write path: direct I/O and a signalled disk thread, measured

BACKLOG `engine.record.direct_io` asked for a measurement before any change:
the disk thread as it is against O_DIRECT with aligned buffers, and its 10 ms
poll against a wake the callback signals. Result: keep both as they are.
Neither alternative wins anything the engine needs, and each one costs
something it relies on.

## What the disk thread does today

- `WavWriter` writes through stdio made unbuffered (`setvbuf(_IONBF)`, since
  #16), so every `write()` is one `write(2)` into the page cache. The
  BACKLOG line said "buffered stdio"; the stream has not been buffered since
  #16. The page cache does the buffering.
- `DiskThread::run()` drains every capture ring, sleeps 10 ms, and
  checkpoints every open take every 2 s (`WavWriter::checkpoint()`:
  header patch and `fdatasync`).
- Each capture ring holds `SessionConfig::ring_seconds` = 4 s.

## What was run

`measure_disk_write DIR [tracks] [seconds] [throughput_mib]`
(`engine/core/tests/posix/measure_disk_write.cpp`, built on Linux with the
tests, never run by ctest). Four parts:

1. **paced**: 18 mono 24-bit 48 kHz tracks (the UMC1820's inputs). One
   pass of 480 frames per track every 10 ms and a sync every 2 s, for 20 s,
   as `DiskThread::run()` does. Once through the real `WavWriter`, once
   through `O_DIRECT`. The direct sink packs into a 4 KiB-aligned buffer,
   writes whole 4 KiB blocks, and keeps the tail for the next pass.
2. **throughput**: the same 18 files written as fast as the medium takes
   them, 64 Ki samples per write (the disk thread's chunk), 512 MiB in all.
3. **wake, poll**: a producer thread on a 128-frame period (2.67 ms)
   advances a fill count; the consumer sleeps 10 ms and records the
   high-water fill.
4. **wake, signalled**: the same, but the producer `sem_post`s each period
   and the consumer waits on the semaphore. The post is timed on the
   producer, which stands in for the callback.

Machine: the owner's Windows laptop, 16 threads, Docker Desktop, kernel
6.6.87.2-microsoft-standard-WSL2. `DIR` was a labelled Docker volume (ext4
on the WSL2 virtual disk, `/dev/sde`), with `sonora-build:trixie` and a
Release build. Two runs on 2026-11-03:

    measure_disk_write /data 18 20 512

| | run 1 | run 2 |
|---|---|---|
| paced stdio: pass p50 / p99 / max | 0.20 / 0.61 / 4.4 ms | 0.15 / 0.35 / 1.8 ms |
| paced direct: pass p50 / p99 / max | 0.06 / 20.2 / 152.5 ms | 0.04 / 9.4 / 23.7 ms |
| paced stdio: longest sync (9 syncs) | 328 ms | 97 ms |
| paced direct: longest sync | 15 ms | 17 ms |
| paced CPU, stdio / direct (% of one core) | 3.1 / 7.2 | 2.3 / 6.1 |
| audio held in process memory, direct (18 tracks) | 73,152 B | 73,152 B |
| throughput stdio / direct (MiB/s) | 133 / 202 | 242 / 85 |
| poll 10 ms: ring high water | 640 frames (13.3 ms) | 640 frames (13.3 ms) |
| signalled: ring high water | 384 frames (8.0 ms) | 128 frames (2.7 ms) |
| signalled: disk thread wakes/s | 374 | 375 |
| signalled: `sem_post` on the producer, p50 / p99 / max | 35 / 101 / 994 µs | 44 / 61 / 121 µs |

## Findings

1. **Direct I/O moves the device wait onto every write.** Each O_DIRECT
   `write(2)` waits for the device, so the disk thread's pass p99 rises
   from under 1 ms to 9–20 ms, with a worst pass of 24–153 ms. The page
   cache path pays the device once per checkpoint instead (97–328 ms),
   and that cost is already covered: the ring holds 4 s.
2. **It does not reduce CPU at recording pace.** At 18 tracks the direct
   path used 2–3 times the CPU of the stdio path, because it makes more
   system calls and each one waits. Double buffering through the page
   cache costs nothing measurable at 2.5 MiB/s.
3. **Throughput is not the limit, and this disk could not rank them.**
   The two runs reversed the order (133 vs 202 MiB/s, then 242 vs 85).
   Even the slowest figure is about 600 times the 18-track recording
   rate.
4. **Direct I/O weakens kill -9 safety.** `WavWriter::checkpoint()`
   relies on the page cache holding every frame written: after kill -9
   the kernel still writes them out (`test_take_kill`). With O_DIRECT, the
   part block that is not a whole 4 KiB block stays in the process, up to
   4,095 B per track (73 KB over 18 tracks), about 28 ms of audio per
   track. It dies with the process. Keeping that guarantee would mean
   padding blocks and rewriting them, which is more I/O, not less.
5. **A signalled wake puts a system call in the callback for 10 ms of
   ring.** Polling every 10 ms kept the high water at 13.3 ms of audio,
   0.3 % of a 4 s ring. Signalling brought it down to 3–8 ms. The price
   was a `sem_post` on every callback, a futex wake into the kernel,
   with a median of 35–44 µs and a worst case near 1 ms, 37 % of a
   2.67 ms period. The producer here is a SCHED_OTHER thread in a VM, so
   these times include preemption. Even so, a system call that wakes
   another thread is outside what CLAUDE.md allows in `Session::process()`.
   The disk thread also woke 3.8 times as often.

## Decision

The poll and the unbuffered stdio stay. The BACKLOG lines on direct I/O and
the condition variable are answered by this note. Reopen only if
measurements on the target medium disagree.

## Open questions

- **Unverified: the target medium.** These figures come from a virtual
  disk on an NVMe laptop. A USB stick, the live medium, and the SSD of the
  installed system are not measured. Run
  `measure_disk_write <session folder> 18 60 512` on the appliance with the
  UMC1820's session folder on each. That medium matters most for finding 1:
  a slow stick could make a single sync longer than the ring. That is
  ADR-0009 Unverified 8, and `DiskThread::longest_checkpoint_ns()` already
  reports the sync time at exit.
- The `sem_post` cost from a SCHED_FIFO thread on bare metal is not
  measured. That figure is lower. It does not change finding 5, which
  rests on the system call, not on its time.
