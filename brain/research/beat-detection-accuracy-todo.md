# Beat detection: what is not yet measured

Item: `time.beat_detection`. Decision: ADR-0114 (offline `sonora-analyze beat`
pass, applied as a tempo edit). ADR-0194 closes the item and points here.

The contract, the classical detector, `tempo_from_beats` and the `tempo.edit
detect` UI are on main. The tests in `plugins/analyze/tests/test_beat*.cpp`
use synthetic click tracks (beats within 20 ms of the truth). Nothing below is
measured on real drums or vocals, so none of these figures is a claim about
real material.

## Open measurements

1. **Beat accuracy on real drums and vocals.**
   Unmeasured. Synthetic clicks prove the mechanics, not accuracy.
   Measure: a hand-annotated beat grid for each recording in a labelled set
   (our own recordings, or licensed and recorded on a card, ADR-0011 §4.6).
   Report the F-measure at ±70 ms and the median absolute offset in ms, per
   material class (drum loop, live drums, sung vocal, rubato), with the
   half/double-tempo error rate counted separately.

2. **The 0.4 confidence constant** (`kSaturatingAutocorrelation`,
   `classical_beat_detector.cpp`). Chosen by judgement ("a strong figure for
   real material"), not fitted.
   Measure: the distribution of the tempo autocorrelation strength on the
   labelled set. Then check that the confidence ranks correct grids above
   wrong ones (ROC against the F-measure from item 1). Change the saturation
   point only if the correct grids sit clearly below it.

3. **The 0.3 apply threshold** (`kBeatMinConfidence`, `beat_detector.h`).
   It is the cut below which the interface refuses a grid. Its value depends
   on item 2, so it is set only after that measurement.
   Measure: the precision and recall of "apply" at each candidate threshold on
   the labelled set. Pick the one that keeps wrong grids out of the tempo map
   without refusing the correct grids of the common material classes.

4. **The 4 % stability bound** (`kStableTolerance`, `tempo_track.cpp`). The
   local beat period (median of 5 intervals) must vary by no more than 4 % to
   be called stable. Unmeasured.
   Measure: the local-period variation on steady drum loops (should be stable)
   and on rubato or live vocal material (should be unstable). Report the
   false-unstable rate on steady material and the false-stable rate on
   drifting material, for bounds around 4 %.

5. **Half/double-tempo errors** (follow-on to item 1). Not covered by any
   test. Measure: how often the global bpm is off by a factor of two on each
   material class, and whether the confidence flags those cases.

## Owner of the measurements

The researcher clears provenance for every recording before it enters a
labelled set. The DSP side then runs the measurement and reports the numbers
with the machine and settings used.
