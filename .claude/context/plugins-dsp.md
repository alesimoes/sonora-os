# Context: native plugins and DSP

Local CI context: `sonora-plugins`. Agent: dsp-engineer (editor art: visual-designer).

## Where
- `common/dsp` (neutral DSP), `plugins/native/<name>` (one dir per processor,
  plus `sdk`, `format`, `measure`), `plugins/analyze`.

## Bound by
- ADR-0011: native suite behind our `Processor` interface; measured quality
  bars, golden files, CPU classes at q128 (eco mode if over class); physical
  models validated against SPICE, never a commercial plugin; own names, no
  trademarks.
- Faceplates: `.claude/rules/design-faceplates.md`.

## Build and test
- `SONORA_DSP_QUICK=1` as CI does; a new suite stays under ~60 s in Release.
- Labels: `dsp`, `native`; the long suites carry `slow` (not in a PR run).
- Gate steps: tree, plugins, rtsan, windows (Portable MSVC). A new target goes
  in the `--target` list of the Portable job in `.github/workflows/ci.yml`.
- `tools/build-slot.sh tools/build-plugins.sh`.
- Tuner on real voices (ADR-0077): offline tool `native_tuner_eval` in
  `plugins/native/tuner/eval/`; datasets, usage and the results table (empty
  until a command printed a number) in `brain/research/native-tuner-eval.md`.

## Traps
- Timing label: `${_dsp_timing_tests}` in root `CMakeLists.txt`.
- No TSan on a PR for plugins; it runs on main.
- A processor over its CPU class is gated off until it has an eco mode.
