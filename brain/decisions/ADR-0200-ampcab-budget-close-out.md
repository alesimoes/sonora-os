# ADR-0200 - Close-out of the class-E budget: quiet-machine rerun

- Status: Accepted (records a rerun; no decision changes)
- Amends the measurement record of ADR-0152. ADR-0152 is not edited: its
  figures and verdicts stand as measured, on a machine that was not quiet.
- Item: `ampcab.budget` (close-out; code and ADR-0152 merged in #669)
- Wire: no protocol change, no format change, no code change.

## Summary

ADR-0152 measured the class-E cost of the amp networks with the 200 ms cabinet
IR included, but on a Windows host running Docker Desktop and other agents'
builds, and it said so. This ADR is the place for the quiet-machine rerun: the
same `ampcab-budget` tool, run through `tools/build-slot.sh` while the host has
no other heavy job. Its Results section states each verdict and the machine
state of the run. The verdicts are not re-decided here.

## Procedure

1. `tools/build-slot.sh --list` must show no holder and no waiter.
2. Build the narrowest target in the `sonora-build:trixie` container, through
   the lock: `cmake --build <dir> --target ampcab-budget`.
3. Run `ampcab-budget 20` (20 s per size, 200 untimed blocks, Release, q128,
   48 kHz, IR included) through the same lock, so no other build starts while
   it times.
4. Paste the output under Results, one row per architecture, both kernel
   tables (`generic` and `avx2`).
5. Re-read the verdict rule of ADR-0152 (class E: p99.9 <= 250 us and
   max <= 500 us, IR included) and state for each row whether it moves.

## Results

**Status: not run. Verdicts are unverified.**

Attempt, 2026-11-14 (local time about 04:15, Windows 11 host, Docker Desktop
running):

- `tools/build-slot.sh --list` showed a holder every time it was asked: a
  `docker run` of another worktree (`drivers.sync.ltc.impl`), first pid 107,
  then pid 239 a few seconds later. The lock was re-taken as soon as it freed,
  so the host was not quiet and the run could not hold it alone.
- The `--detach` and `tools/build-wait.sh` helpers the harness rules name are
  not on this branch (the branch is based on `origin/main` at adb1289c), so a
  queued run could only be a foreground call. Foreground queueing behind
  another agent's build ran into the tool's time limit and was not started.
- `docker ps` listed no containers at the time of the check, so the only
  competing work was the lock holder and the other agents' processes that the
  lock does not serialise.

No rows are pasted because no run completed under the lock. Per the task, no
numbers are invented and no verdict moves. The ADR-0152 verdicts remain the
last measured figures, and they are the figures of a host that was not quiet.

Per row, the verdict is therefore **unverified** for this rerun:

| Architecture | generic | avx2 |
|---|---|---|
| WaveNet nano | unverified (ADR-0152: fits) | unverified (ADR-0152: fits) |
| WaveNet feather | unverified (ADR-0152: fits, margin 27 us) | unverified (ADR-0152: fits) |
| WaveNet lite | unverified (ADR-0152: does not fit) | unverified (ADR-0152: fits) |
| WaveNet standard | unverified (ADR-0152: does not fit) | unverified (ADR-0152: fits) |
| LSTM 1x16 | unverified (ADR-0152: fits) | unverified (ADR-0152: fits) |
| LSTM 2x8 | unverified (ADR-0152: fits, margin 248 us) | unverified (ADR-0152: fits) |
| LSTM 1x24 | unverified (ADR-0152: does not fit) | unverified (ADR-0152: fits) |
| LSTM 2x32 | unverified (ADR-0152: does not fit) | unverified (ADR-0152: fits) |
| LSTM 4x64 | unverified (ADR-0152: does not fit) | unverified (ADR-0152: does not fit) |
| LSTM 1x128 | unverified (ADR-0152: does not fit) | unverified (ADR-0152: does not fit) |

Machine state of the attempt: not quiet (another agent's build held or
queued for the lock throughout the check). Owner-facing consequence: the
eco-mode decision for a generic-only host (ADR-0152 Reading) stays on the
ADR-0152 figures until a quiet run replaces them.

## Rerun trigger

Run the Procedure when `tools/build-slot.sh --list` is empty at step 1 and
stays empty through step 3. Record the state of the lock at the start and the
end of the run in this section.

## Consequences

- No processor, kernel or eco-mode decision changes until a quiet run is
  pasted here.
- A future quiet run that moves a verdict amends this ADR's Results, not
  ADR-0152.

## Unverified

- Every figure for the quiet machine (all rows above).
- Whether the lock's other holders stayed out during a run that did start.
