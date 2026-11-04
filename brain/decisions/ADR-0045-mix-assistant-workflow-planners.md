# ADR-0045 — Mix-assistant workflow planners: gain staging, bus structure, parallel sends

- Status: Accepted (recommended default, taken with no owner available,
  2026-11-04)
- Date: 2026-11-04
- Item: `mixassist.workflow` (mix assistant V1: analyse and correct the mix)
- Relates to: ADR-0038 (plans), ADR-0011 (§4.4, N5), ADR-0022 (aux buses),
  ADR-0007 (module DAG)

## Summary

Three rule-based `PlanSource`s in `engine/control` (`mix_planner.h`) and one
offline analysis pass. **Gain staging** trims faders (`SetMix`) so each track
sits at -23 LUFS after its fader, with a -3 dBTP ceiling per track and 6 dB
of estimated master headroom. **Bus structure** guesses a role per track
(drums, bass, guitars, keys, vocals) from its name, its inserts and its
colour, adds one aux per role with two or more tracks (`AddBus`) and routes
the tracks to it (`SetOutput`). **Parallel sends** adds a "Drums Parallel"
aux when the kit's crest factor is 10 dB or more and a "Bass Saturation"
aux for the bass, with a post-fader send per track (`AddSend`) and the
native processor's settings. Every step has a title, a reason naming the
rule and the measurement, and `needs`. No model, no randomness, no wire
change: `kProtocolVersion` stays. `sonora-mixplan SESSION.sonora
[gain|buses|parallel|all]` prints the plan and its diff.

## Context

ADR-0038 fixed the contract: a plan is model Members with words, previewed
as a diff, accepted in part, applied as one Compound. This item is its first
producer. ADR-0011 §4.4 asks for rules first, and for the assistant to use
our own processors only.

## Decision

1. **Analysis is offline and separate.** `analyse_session()` reads each
   audio track's active playlist through `WavReader`, region gain applied,
   muted regions left out, and measures sample peak, RMS, `LoudnessMeter`
   integrated LUFS and true peak (common/dsp). Fades, pan and the fader are
   not in the figures: the fader is read from the mix and added by the
   planner, so a re-run after the plan finds the same numbers. It runs on
   the control thread or a worker, polls a cancel flag and reports progress
   once per 4096-frame block. A file that cannot be read costs that track
   (`TrackAnalysis::problem`, a plan note), not the pass.
2. **Planners hold the analysis by value.** `PlanSource::propose` takes only
   the snapshot, so the constructor takes the measurements. The same snapshot
   and analysis give the same plan.
3. **Gain staging.** delta = target - (LUFS + fader). A boost is capped so
   the track's true peak stays under the ceiling; a cut never is. Deltas are
   rounded to 0.5 dB; under 1 dB there is no step. The master estimate adds
   the powers of the tracks' peaks (uncorrelated); over the headroom every
   track takes the same extra trim, so the balance the trims made holds. A
   muted, silent or unreadable track is left alone with a note. A plan with
   no step fails with "within 1 dB" rather than returning an empty plan.
4. **Roles.** Keyword tables on name tokens (short abbreviations match whole
   tokens only: "oh" is not "ohm"; "Bass Guitar" is a bass). Then inserts
   (`deesser` is vocals, `ampcab` guitars, a plugin name by the same table).
   Then colour: new tracks take the palette in turn (`auto_colour`), so only a
   colour the operator moved off its place's own counts; such a track borrows
   only when every classified track of that colour agrees. The
   reason quotes the evidence.
5. **Parallel sends and the missing aux insert.** The model has no insert
   on an aux bus (BACKLOG "Aux and Master inserts"; `AddInsert` takes a
   track). A plan can hold only what the model applies, so the processor is
   not a step: it is a `PlanNote` on the bus and `ParallelPlanner::
   suggestions()` (`ProcessorSetting`: LV2 URI `urn:sonora:native:compressor`
   or `...:saturator`, and the parameter keys of plugins/native). When aux
   inserts land, each becomes an `AddInsert` step that `needs` the bus, with
   no change to the rules. Settings: compressor threshold at the kit's RMS
   minus 3 dB, ratio 8, knee 3 dB, attack 5 ms, release 80 ms, auto make-up;
   send -6 dB, lowered 0.25 dB per dB of crest above 10, to -15. Saturator
   drive 12 dB, warm shape, saturate mode, focus 800 Hz, output -6 dB; send
   -12 dB.
6. **Joining.** `propose_mix_plan(All)` runs gain, buses, parallel on the
   same snapshot with the id counter carried on, re-indexes `needs` and
   labels the compound "Mix Assistant". A planner with nothing to say is a
   note in "All" and an error alone.
7. **Where.** `engine/control` already links model, core (so dsp) and
   audiofile, and `consolidate` is the precedent for reading sources off the
   audio thread. No new module edge. The command line is a target of the
   same directory, not part of `sonora-engine`.

8. **Reverb and delay (`mixassist.reverb_delay`).** `ReverbDelayPlanner`
   takes a `ReverbDelayReference` (tail, pre-delay, echo time, two wet
   levels, read off a reference record by the operator; the planner does not
   analyse the reference audio) and the session's first tempo. Times snap to
   the tempo: tail = a whole number of half beats (at least one; a note over
   4 bars), pre-delay = the 1/64..1/16 note nearest the reference, echo = the
   1/8, dotted 1/8, 1/4 or dotted 1/4 note nearest the reference's time
   (`note_l` of `urn:sonora:native:delay`, `sync` on, `tempo` set). Vocals
   get a "Vocal Reverb" and a "Vocal Delay" aux, at the reference's levels as
   post-fader sends; two or more drum tracks get a "Drum Room" aux whose
   tail stays under one beat, 4 dB under the vocal reverb send. Excess check:
   the two vocal returns are power-summed; over `max_wet_db` (-10 dB under
   the dry track) both sends drop by the same amount, with a note. Processors
   are `PlanNote`s and `suggestions()` as in 5; the wire is unchanged (no
   protocol bump). `propose_mix_plan(Space)` and `All` run it last;
   `sonora-mixplan FILE space` prints it.

   **Measuring the reference (`space_measure`).** `sonora-mixplan FILE space
   --reference REF.wav` fills the `ReverbDelayReference` from the audio, in
   `engine/control` (offline, never `process`; `plugins/analyze` was not on
   main). WAV/BWF only: FLAC is refused with a message to convert it. Mid
   signal at 1 ms resolution, at most 15 minutes. Each estimate is optional;
   one not found leaves the planner's default and the printout says so.
   - *Decay:* free decays (a level peak, its plateau end, then a fall of at
     least 30 dB before the next rise) are integrated backwards (Schroeder,
     JASA 37, 1965); T20 (-5..-25 dB, R^2 >= 0.95) x 3 is the RT60, EDT
     (0..-10 dB) x 6 the early time; the median of the decays. A simpler
     cousin of the blind estimators (Loellmann et al., IWAENC 2010), not
     their maximum-likelihood method.
   - *Pre-delay and wet level:* isolated transients (20 dB quiet before),
     a gap 10 dB down within 150 ms, then the tail rising 6 dB. Pre-delay =
     transient to tail onset; DRR = direct energy / tail energy (ACE
     Challenge definition, Eaton et al. 2016); the reverb send estimate is
     -DRR clamped to -40..0 dB. Needs such transients: a dense mix gives
     "not found", not a guess.
   - *Echo:* mean-removed autocorrelation of the onset strength (5 ms RMS
     steps) of the side signal when it holds over 10 % of the mid energy,
     else of the mid; first peak within 60 % of the largest between 60 ms and
     1.5 s, strength >= 0.2. The echo level is the first repeat's amplitude
     under its source over clean onsets. The planner snaps the time to a
     note; the strength tells the operator how sure to be, since a steady
     rhythm also correlates (the snap to the tempo grid hides most of that).
   - *Tempo:* the same autocorrelation, 75..160 bpm; printed, the session's
     tempo still decides the snap.
   Tests (`test_space_measure`) build signals of known RT60 (0.6, 1.2, 2.0 s),
   pre-delay (20, 45, 90 ms), DRR (exact from the generated energies), echo
   (375, 562.5 ms, -6 dB) and tempo (96, 120, 140 bpm) at 48 and 44.1 kHz.
   Tolerances: RT60 10 %, pre-delay 4 ms, DRR 2 dB, echo 10 ms, level 3 dB,
   tempo 2 bpm. Not tested: real recordings, where tails overlap and these
   estimates degrade.

## Consequences

- The thresholds above are starting rules, not tuned against mixes; they are
  options in `GainStagingOptions` and `BusStructureOptions`, and the
  parallel figures are constants to move when a listening pass says so.
- Bus routing sends a track's whole output to its bus; a track already
  routed away from the Master is left alone and noted.
- Buses created by the plan have no inserts, no colour and unity gain.
- The reverb/delay figures are starting rules, not tuned by ear; a tempo map
  with changes is followed at its first tempo only (noted).
- Gain staging ignores pan and fades, and counts every track as feeding the
  Master.

## Alternatives rejected

- **A planner that adds the processor as a track insert.** Wrong place:
  parallel compression is on the sum, not on each track.
- **A duplicate-track parallel path** (a track that copies the audio and
  holds the compressor): works today, but doubles the regions, breaks on edit
  and is not what the engineer wants on the aux.
- **LUFS-only trims.** A click or snare with a low integrated figure and a
  high peak would be raised into the ceiling; the true-peak cap is needed.

## Unverified

- Gain staging at -23 LUFS per track is a convention; whether 16 tracks then
  sum to a usable mix level was only checked through the headroom rule.
- Analysis time on a long session: one pass at disk speed, not measured.
- That the engine's `MemberCheck` accepts every step (bus count limit, send
  slots) was not exercised with the engine's hook.
