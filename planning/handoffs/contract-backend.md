# Handoff: contract/backend (ADR-0007 M2)

**Goal.** M2: `contracts/backend` (AudioBackend, ProcessCallback, AudioBlock), `SONORA_RT`, JACK only in `drivers/jack/src`, drivers/jack → engine/core edge removed, then the reviewers' changes (driver-engineer co-owner, auditor, code-reviewer).
**State.** All requested changes are in. Local on `contract/backend`, not pushed, ready for re-review. Base `aa4fbec`.

**Commits** (oldest first): `ce0a73e` pin Session::process behaviour (test first) · `f3b7d52` M2 contract ·
`81d8eaa` STATUS: M1 landed · `c262482` natural_less + PeriodGuard → drivers/support · `299eb87` devices, DeviceGone/Stalled,
per-period rate, fatal_reason by value, frame_time widening, NULL buffers, xrun_cb SONORA_RT, fatal check before take ·
`5721b9b` lock-free static_asserts · `1a50806` mlockall, refuse to start without unlimited memlock ·
`75f70cd`, `53d3dfe` test_memory_lock under sanitizers/clang · `a6013ba` ADR-0007 note + measurements.

**Verify.** `tools/build-engine.sh`, which should end with `100% tests passed, 0 tests failed out of 12`, then
`tools/test-engine-docker.sh`, which should print `RESULT files=18 connected=18 ... buffer=128 xruns=0 overruns=0` and `PASS`.

**Done, with evidence** (logs in `build-container/logs/`):
- GCC: 0 warnings, 12/12 tests. Clean Release, TSan, ASan+UBSan and `-D_FORTIFY_SOURCE=2 -O2` builds: 0 warnings, 12/12 each.
- clang-22 `-fsanitize=realtime`: 12/12. The 4 `-Wfunction-effects` warnings are all libjack C calls.
- RTSan engine with 18 tracks under PipeWire: no report, xruns=0, 18 correct tones. Also quiet with quantum 64 and rate 96000 forced mid-take.
- Mid-take scratch tests, 18 tracks: each exits 1 naming the cause, and all files are the same length.
  - quantum 64: "128 -> 64 frames".
  - rate 96000: "48000 -> 96000 Hz"; no 96 kHz period landed.
  - Source node destroyed: DeviceGone, naming the device.
  - PipeWire SIGSTOPped: Stalled.
- Engine without unlimited memlock exits 1 naming RLIMIT_MEMLOCK.

**Open**
- Stall: the take closes, but the exit waits in `jack_deactivate()` until the server resumes.
- The container cannot show that the per-period rate check is needed: the code before it also wrote no 96 kHz period in 3 runs. Only `test_period_guard` proves it.
- `fatal()` text says "a port an input was connected from", not "physical", so the container can test it. The owners decide.
- Not edited (not mine): `.claude/skills/rt-audit/SKILL.md` and `.claude/agents/rt-safety-auditor.md` still say "start at `JackBackend::process_cb`" (now `JackBackend::Impl::process_cb`, plus every `SONORA_RT` symbol).
- Unknown: the code-reviewer's "one comment fix" was not described to me.
- CI's native runner has a finite memlock limit, so the success half of `test_memory_lock` skips there and says so.
- With unlimited memlock in the container, PipeWire 1.4.2 still runs. The engine needs `--ulimit memlock=-1:-1`; both scripts pass it.

**Do not redo**
- The `sonora-runtime:trixie` tag is shared between worktrees. Scratch rate tests need an image whose `allowed-rates` includes 96000; with an image from the min-quantum branch, `force-rate` is ignored and the test passes vacuously. Use a private tag (`sonora-runtime:contract-backend`).
- ASan and TSan make `mlockall` a no-op, which is why that test skips its VmLck half there.

**Pointers.** Contract: `contracts/backend/include/sonora/{process_callback,audio_backend}.h`. Helpers: `drivers/support/`.
Decisions: ADR-0007 "Update, 2026-10-12: the backend contract as landed (M2)". Measurements: `brain/research/adr-0007-measurements.md` §1.
Scratch harness (not in repo): `fatal.sh` with MODE=quantum|rate|unplug|freeze, in the agent's scratchpad.
