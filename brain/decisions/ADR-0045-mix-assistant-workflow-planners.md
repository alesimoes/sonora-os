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

## Consequences

- The thresholds above are starting rules, not tuned against mixes; they are
  options in `GainStagingOptions` and `BusStructureOptions`, and the
  parallel figures are constants to move when a listening pass says so.
- Bus routing sends a track's whole output to its bus; a track already
  routed away from the Master is left alone and noted.
- Buses created by the plan have no inserts, no colour and unity gain.
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
