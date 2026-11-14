# ADR-0108: 24-hour soak: one continuous take, a script, a hardware verdict

Status: accepted (architect, 2026-11-08)

## Summary
`system.hardening.soak` ("24 hours continuous recording on the UMC1820, zero
xruns") is closed in two steps: `tools/soak-record.sh` (this ADR's protocol,
with a `--self-test` on a fake backend) lands first; the item is ticked only
when a real 24 h run on installed hardware reports PASS.

## Context
The engine already prints one `RESULT files=N ... frames=N ... xruns=N
clock_steps=N overruns=N misses=N dropped=N ...` line when a take ends
(`engine/app/src/main.cpp`), and exits 1 on any miss, drop, xrun or overrun.
It exits 1 when non-empty tracks disagree in frame count (empty tracks are skipped, so the script must count the 18 files itself). The xrun
counter is also published in the control state (`engine/control/src/state_publisher.cpp`,
`t.xruns`). `tools/verify-recording.py` checks the files and
`tools/umc-measure.sh` does short sweeps. Nothing runs a day-long take or watches it. A soak that only
reports at hour 24 wastes a day when hour 1 already failed.

## Decision
1. **One continuous take**, 86,400 s, 18 tracks, at a quantum passed
   explicitly (`--buffer`). The quantum is the output of backlog item
   `drivers.umc1820.latency` (todo, blocked on hardware); until that item
   reports, the operator picks it and the report records it. On the installed
   system (the target), not the live stick, with memlock on (never
   `--no-memlock`).
2. **Pass criteria**, all required: `xruns=0`, `overruns=0`, `dropped=0`,
   `misses=0`; all 18 files have the same frame count, equal to `frames` in
   RESULT (the engine fails on disagreeing non-empty tracks; the script counts
   all 18 files), and
   `verify-recording.py` passes; `clock_steps` is reported, not
   failed (existing rule); the engine PID never changed (no restart); no
   `xhci` or `snd_usb` error in the journal during the run.
3. **Fail loudly and early**: before starting, the script refuses (exit 2) if
   free space on the session volume is below the computed need plus 5 %, if
   the CPU governor is not `performance`, if memlock or the RT limits are
   missing, or if the card shows fewer than 18 capture channels. During the
   run it samples every 60 s (engine alive, free space, journal errors, the
   xrun count from the control state) into a CSV. The first non-zero
   xrun or overrun ends the run as FAIL at once and keeps the files.
4. **Disk need is computed, not assumed**: channels x rate x bytes per sample
   x seconds. 18 x 48,000 x 3 x 86,400 = 223.9 GB at 24-bit; 299 GB at 32-bit
   float. The script prints it before it starts.
5. **Output**: a markdown report (`--out DIR`) with the criteria, the numbers,
   the CSV path and the RESULT line, to paste in a PR or handoff. Exit 0 PASS,
   1 FAIL, 2 setup refused.
6. `--self-test` runs 30 s on the fake backend and proves both a PASS and a
   forced FAIL (injected xrun); it is the only part CI may run.

## Rationale
A single take is the user's worst day (one long session, no restart to clear
state); 24 x 1 h runs would hide leaks and drift. Early abort costs nothing
and saves the day. Failing at start-up on disk and RT preconditions follows
"fail loudly at start-up". Computing the size first avoids the likeliest
silent failure: a full disk at hour 20.

## Consequences
- The item stays open until a person reports a hardware PASS (waits on the
  owner; work moves on). The PR is partial and says so.
- A 4 GiB per-file limit matters at this size (about 12.4 GB per track at
  24-bit). The WAV writer switches to RF64 in place past the RIFF limit
  (`common/audiofile/include/sonora/wav_writer.h`), and its tests lower that
  limit to reach the switch. That the switch holds for a 12 GB track on the
  target disk is **unverified**; the self-test cannot show it. Before the
  hardware run, a short take at the lowered limit confirms the RF64 file
  reads back intact; if not, raise an item first.
- The live xrun counter is published (`t.xruns` in `state_publisher.cpp`, from
  the audio device). It is verified in code, not on hardware. If the counter
  is absent (no audio device in the state), the script falls back to journal
  sampling and says so in the report.
- Thermal and USB power behaviour over 24 h are found only by the run.

## Alternatives rejected
- *24 hourly segments*: easier on disk, but hides slow leaks and the
  continuity the item states.
- *Nightly CI run*: no UMC1820 in CI; a fake-backend 24 h run proves nothing
  about USB and ties up a runner for a day.
- *Report only at the end*: a day lost to an hour-1 failure.
- *Soak on the live USB*: stick throughput and the read-only root are not the
  target.
