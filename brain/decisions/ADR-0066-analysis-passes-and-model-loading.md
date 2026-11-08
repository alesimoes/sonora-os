# ADR-0066: Analysis passes, model loading and the sonora-analyze subcommands

Status: Accepted (contract for `native.ai_analysis`, 2026-11-07; decided on the recommended defaults of `brain/research/onnx-runtime-and-analysis-models.md`).
Builds on: ADR-0004 (what we refuse), ADR-0011 §3.4, §4.1, §4.4, §4.6 (AI produces parameters, ONNX Runtime only in `sonora-analyze`, training data and cards), ADR-0046 (the mastering pass, the report shape), ADR-0061 (micro builds).

## Summary

`sonora-analyze` gains two passes, **key** (tonic and mode) and **timbre** (a third-octave curve and the EQ that moves a target towards a reference). Every pass has a **classical implementation that is always built and is the default**; a pretrained model is an opt-in alternative that never replaces it silently. **ONNX Runtime is linked only by `sonora-analyze`**, behind the CMake option `SONORA_WITH_ONNX` (`find_library`, off by default and never on Portable/MSVC). Models are described by `plugins/native/models/manifest.json`; the loader refuses a file whose sha256 differs and a `conditional` model unless the operator config sets `allow_conditional_models`, and it never downloads. A missing library or model disables that pass with a message and exit 1, never a fallback to another model. Reports are JSON of settings and measurements, never audio; a reference track is reduced to a derived curve and never copied. The interfaces are headers under `plugins/analyze/include/sonora/analyze/` with inline fakes; the slices add sources through CMake fragments and never edit the contract's files. No protocol or session-format change.

## Context

N6 (`native.ai_analysis`) asks for tempo, key, presence and timbre suggestions. The research note selected candidates and found that the only usable key model (MusicalKeyCNN `keynet`, MIT) carries a training-data caveat (GiantSteps-MTG, licence not determined), and that S-KEY and the Essentia models are excluded on their weights' licences. ADR-0011 §3.4 already confines general inference to `sonora-analyze`; this ADR fixes how passes, models and the command line fit together so the key, timbre and model-loading slices can be built in parallel without touching each other's files.

## Decision

### 1. Classical first, models opt-in

- Each pass has a classical method that needs no model and no ONNX Runtime: key by Krumhansl-Schmuckler profile correlation on chroma (C. L. Krumhansl, *Cognitive Foundations of Musical Pitch*, 1990; the Temperley-revised profiles are an allowed variant), timbre by Welch-averaged third-octave band power (the method `mix_analysis.h` already uses for octaves).
- The classical method is the default. A model is used only when the operator asks for it by name (`--method keynet`); its result says so (`"method": "keynet"`). If the asked-for model is unavailable, the pass fails with exit 1 and a message naming the cause. It does not quietly run the classical method instead, and it never substitutes another model: an operator who asked for a model and got something else would read a figure whose origin they do not know.
- `KeyMethod` and the report's `method` field name the method that produced the figure.

### 2. ONNX Runtime: one consumer, one option

- `option(SONORA_WITH_ONNX ... OFF)` in `plugins/analyze/CMakeLists.txt`. When ON, `find_library(onnxruntime)` and `find_path(onnxruntime_cxx_api.h)`; if either is missing CMake prints one status line and builds without it (`SONORA_ONNX_FOUND` false). The Debian package is `libonnxruntime-dev` 1.21 (research §1.1); the ISO build turns the option on.
- Only `sonora-analyze` and its library link it. `common/dsp`, `plugins/native` and every `process` stay free of it (ADR-0011 §3.4); the amp kernels in `common/dsp/neural` are unrelated and do not use ORT.
- The ORT-backed `ModelRunner` lives in its own source, compiled only when found, and reaches the app through `RunnerFactory` (`inference.h`). Without it the factory returns null with "built without ONNX Runtime"; the pass reports that and exits 1. No `#ifdef` in shared headers: the choice is made by which source CMake compiles.
- Portable (MSVC) does not build `sonora-analyze` at all (it needs `common/model`, Linux-only).

### 3. The manifest and the loader

`plugins/native/models/manifest.json` is `{"schema": 1, "models": [ ... ]}`. Each entry has the fields of research §3, all required:

| Field | Type | Rule |
|---|---|---|
| `name` | string | `[a-z0-9_-]+`, unique in the file |
| `version` | string | non-empty |
| `file` | string | relative to the models directory; not absolute, no `..`, no backslash |
| `url` | string | `https://` only; pinned (commit-hash or release asset). Read by the image build, never at run time |
| `sha256` | string | 64 lowercase hex digits |
| `size_bytes` | integer | > 0 |
| `licence` | string | SPDX identifier, non-empty |
| `licence_url` | string | non-empty |
| `weights_status` | string | `cleared` or `conditional` |
| `training_data_note` | string | may be empty for `cleared`; non-empty for `conditional` |
| `card` | string | `brain/models/<name>.md` |
| `onnx_opset` | integer | >= 1 |
| `input` | object | `sample_rate` > 0, `channels` >= 1, `layout` non-empty |
| `outputs` | array of strings | output tensor names, at least one |
| `ort_min_api` | integer | >= 1 (21 for ORT 1.21) |
| `attribution` | string | text for the third-party notices |

- `parse_manifest` refuses the whole file on the first violation, naming the entry's index and the field. An unknown field is refused too, so a typo does not pass as an omission. A schema other than 1 is refused.
- `verify_file` checks, in order: the policy (`conditional` refused unless `ModelPolicy::allow_conditional_models`), the file's existence and size, then its sha256. Hashing goes through the injected `FileDigest`, so tests need no file and the header no filesystem.
- `allow_conditional_models` comes from the operator config (`--allow-conditional-models` on the command line, which engine/control passes from the setting); the default is false. The researcher's caveat on `keynet` is the reason the switch exists.
- The loader never downloads. Model files are not in git; the image build fetches them by the manifest and checks the same sha256. An entry enters the manifest only after the researcher has cleared its provenance and its card is in `brain/models/`.

### 4. Reports

Both passes write one JSON object to stdout or `--out`, the shape of the mastering report (ADR-0046): measurements, settings, the method, never audio.

- `key`: `{"pass": "key", "method": "krumhansl"|"keynet", "tonic": 0..11, "tonic_name": "C".."B" (sharps), "mode": "major"|"minor", "confidence": 0..1, "duration_s": n}`.
- `timbre`: `{"pass": "timbre", "method": "third_octave", "bands_hz": [31 centres], "reference_db": [31], "target_db": [31], "curve_db": [31], "eq_bands": [{"freq_hz", "gain_db", "q"}...]}`. A band without valid data is `null`. The EQ bands are settings for the stock parametric EQ, which the operator previews and applies as an undoable session operation (ADR-0011 §4.1, §4.4).
- A reference track is read, measured and closed; only its derived curve may be stored or reported. No audio, excerpt or resampled copy of it is written anywhere.
- Exit codes as ADR-0046: 0 report written, 1 the pass failed (message on stderr), 2 usage, 130 cancelled.

### 5. Interfaces and file ownership

- `key_detector.h`: `KeyResult{tonic, mode, confidence, method}`, `KeyDetector::detect(AudioSource&, const RunOptions&, KeyResult&, error)`. `RunOptions::cancel` is the cancel flag.
- `timbre_match.h`: `TimbreCurve` (31 ISO 266 third-octave gains, 20 Hz to 20 kHz, with validity), `TimbreMatchResult{curve_db, eq_bands[]{freq_hz, gain_db, q}}`, `match_timbre(reference, target, params, result, error)`, and the `TimbreMeasurer`/`TimbreMatcher` interfaces the app is written against.
- `model_manifest.h`: `ManifestEntry`, `parse_manifest`, `verify_file`, `FileDigest`. The parser is inline in the header: the schema is the contract.
- `inference.h`: `Tensor`, `ModelRunner::run(inputs, outputs, error)`, `RunnerFactory`.
- `analyze_fakes.h`: one inline fake per interface, for the tests of every slice.
- `plugins/analyze/CMakeLists.txt` is the single owner of the build. It includes `cmake/models.cmake`, `cmake/key.cmake` and `cmake/timbre.cmake` when they exist; a slice adds its sources and tests there (`target_sources(sonora_analyze ...)`, its own `add_executable`/`add_test`) and never edits the owner. `app/main.cpp` dispatches subcommands through a table to `run_key_command`/`run_timbre_command` (`app/commands.h`). The owner compiles `app/cmd_key.cpp` when that file exists, else `app/cmd_key_stub.cpp` (exit 1, "not implemented"); likewise for timbre. A fragment does not add `app/cmd_*.cpp` itself.

### 6. Deferred

- **Artist-tone matching** (N6): matching a player's sound needs amp capture cards recorded on hardware the owner provides (ADR-0011 §3.3, §4.6), and no product, capture or preset name may carry a guitarist's or band's name (ADR-0004, ADR-0011 §5). Deferred until those captures exist.
- **Tuner follows the session key** (N6): there is no key field in the session format on main. It needs a session-format change under an ADR of its own. Deferred; the key pass reports, nothing stores it.
- Tempo and presence passes (Beat This!, YAMNet) will follow this contract; they are not part of this item's slices.

## Consequences

- The classical passes ship on every Linux build, with or without ORT; the micro build image needs no ONNX package.
- A conditional model is unusable until the operator opts in: that is the answer to the training-data caveat until it is resolved.
- The schema check is strict; a manifest change that adds a field amends this ADR and `model_manifest.h` together.
- `verify_file`'s real `FileDigest` (on `sonora_sha256`) needs the edge `plugins/analyze -> common/base` in `tools/module-deps.allow`; the models slice adds it with its source.

## Unverified

1. That ORT 1.21's C++ API loads `keynet.onnx` and matches the reference (research §5.3).
2. That `libonnxruntime-dev` installs in the build image within the 72 MiB measured.
