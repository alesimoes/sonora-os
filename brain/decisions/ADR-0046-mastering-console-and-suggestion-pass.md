# ADR-0046 — The mastering console and its rule-based suggestion pass

- Status: Accepted (recommended default, taken with no owner available,
  2026-11-04)
- Date: 2026-11-04
- Item: `native.mastering` (ADR-0011 stage N4)
- Relates to: ADR-0011 (§3.4 `sonora-analyze`, §3.5 class D, §4.4), ADR-0038
  (plans), ADR-0007 (module DAG), ADR-0012 (inserts)

## Summary

1. **The console is a processor built from our other processors.**
   `plugins/native/mastering` owns an EQ (zero-latency mode: low shelf, two
   bells, high shelf), the compressor, the saturator (eco), a mid/side width
   stage, a gain stage and the true-peak limiter, in that order, and a
   BS.1770 loudness meter on its output. It forwards parameters to the
   stages as events, so each stage's own smoothing and bars apply; the
   console's tests show each stage inside it equals the standalone
   processor. Class D (400 us p99.9). **Auto-level** (off by default) moves a
   trim of at most +-12 dB, 0.9 dB per second, toward a target short-term
   loudness, after the first 3 s.
2. **The suggestion pass is rules over a whole-mix analysis**
   (`plugins/analyze`): loudness and true peak (BS.1770/EBU 3341), crest and
   peak-to-loudness ratio, stereo correlation and side level, and the
   long-term octave spectrum against a target curve. Same analysis, same
   settings; every setting has a title and a reason with the numbers.
   No model, no ML (N6 adds models later, behind the same plan).
3. **Its output is an ADR-0038 plan**: `MasteringPlanner` is a `PlanSource`.
   Step 0 adds (or replaces) the console insert with all its parameters;
   step 1 is a fader trim only when the mix is so loud that the console's
   -12 dB gain cannot reach the target. Other inserts are named in the notes.
4. **The offline run** is `sonora-analyze mastering <bounce.wav>`: blocks of
   16384 frames, polling a cancel flag (SIGINT/SIGTERM) between them,
   reporting progress, writing a JSON report (measurements, settings,
   findings). It runs off every audio thread.

## Decisions that need recording

- **Module edge**: `plugins/analyze` links `common/model` (for `PlanSource`)
  and `plugins/native/mastering` (the parameter ids and ranges, so the pass
  and the processor cannot disagree). ADR-0011 §3.4 listed only
  `common/dsp`, `common/audiofile` and `common/json`; ADR-0038 §7 already
  put the planner behind `PlanSource`, which requires the model. Linux-only
  because `common/model` is.
- **Where the console goes**: the model has no master or aux inserts yet
  (BACKLOG, "Aux and Master inserts"). Until it has, the plan puts the
  console on a stereo (or mono) audio strip, the one the bounce was made
  of, as its last insert (inserts are pre-fader). Moving it to the Master is
  a change of `MasteringPlanConfig::track` once that lands.
- **Rules as code, not yet as data**: ADR-0011 §4.4 says workflow rules are
  data under `plugins/analyze/rules/`. The thresholds are one struct
  (`MasteringConfig`) and one table (`kTargetDb`) with the literature cited;
  loading them from JSON is a follow-up, with the same tests.
- **The target curve** is an engineering target for the long-term octave
  spectrum of a finished popular mix (Pestana, Reiss and Barbosa, AES 135,
  2013; Katz, Mastering Audio, 3rd ed.). It is judged by shape (anchored on
  500 Hz - 2 kHz), a +-3 dB tolerance, half the excess corrected, 6 dB at
  most.
- **Fader trim** is the one step that is not an insert, and only exists for
  the case above; it is a `SetMix`, which a plan can take back.

## Unverified

- The target curve and thresholds are defaults from the literature and
  practice, not tuned on a corpus. The reasons the pass prints make each one
  checkable by the engineer; tuning is a data change.
- Auto-level is validated on stationary noise (target and +-12 dB clamp), not
  on programme with long silences or a changing section; it holds its trim
  through quiet passages below -70 LUFS short-term rather than chase them.
