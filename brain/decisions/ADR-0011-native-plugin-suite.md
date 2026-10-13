# ADR-0011 — The native plugin suite

- Status: **Proposed**, 2026-10-13. Amendment 1 (coordinator, 2026-10-13):
  §2.2 edges and the N0 decisions, at the end. Amendment 2 (2026-10-13):
  the DPF patch instead of the §1.2 fallback, and block-accurate VST3
  automation under §2.4, at the end.
- Date: 2026-10-13
- Decided by: architect, for the owner. The points that are the owner's to
  settle are listed at the end, under "For the owner".
- Numbering: follows ADR-0010 (kiosk compositor). No other branch carries an
  ADR-0011 at the time of writing.
- Research: `brain/research/native-plugin-suite.md` is being written in
  parallel on `research/native-plugins` and is **forthcoming**. Where this ADR
  rests on a fact that note is expected to settle, the fact is listed under
  "Unverified" with the measurement that settles it.

## Context

**The request (owner, 2026-10-13).** The product ships its own plugin suite:

- EQ, reverb, delay, chorus, compressor, gate, limiter, tube drive, tape
  simulator, mix bus;
- a full AI mastering console that detects what is in excess and suggests
  what to cut;
- classic opto and FET compressors. The owner's references are two famous
  1960s hardware units as one commercial vendor emulates them;
- per-instrument AI reference timbre matching;
- an AI vocal tuner that follows the song's key.

**Owner addition, same day (given to the coordinator):**
1. **Physical modelling from the hardware literature** is the method for
   every analog-character processor: tubes, harmonic distortion,
   transformers, mains hum and noise, and tape hiss and noise floors.
2. **A guitar amp head and cabinet family.**
   - Classic amps and cabinets, modelled with neural-network DSP:
     capture-based, NAM-class models, plus cabinet impulse responses.
   - The aim is the tones of famous guitarists.
   - Captures are only of hardware we own or rent. No third-party captures
     or IRs without a licence. No artist names in product or preset names.
   - "Artist tone" comes from reference matching against audio the user
     supplies.
   - This ADR must decide where neural inference runs (the plugin host
     process), its CPU budget per instance, and the model format.
3. **An AI mixing and mastering assistant** that runs the workflow of top
   studios:
   - gain staging;
   - bus structure;
   - parallel compression and saturation;
   - shared reverb and delay sends;
   - the mix-bus chain;
   - mastering.

   It makes suggestions and sets up parallel processing with our plugins.
   Everything is applied as normal session edits the user can undo, through
   the model's apply and undo (ADR-0009). Never hidden processing.

**Owner example for the assistant, same day.** Reverb and delay on
instruments and vocals:
- the right time;
- the ideal time for the song's style;
- the ideal amount, so it is not excessive;
- matched to an artist's reference.

This is made the assistant's first concrete feature (§4.4).

**Owner question, same day.** "Can we plug Claude into these plugins to use
Claude's knowledge, or will it be native?" §4.5 records the proposed answer,
a hybrid. That subsection is Proposed separately, because its cost and
privacy terms are the owner's.

**The coordinator's constraint, which restates ADR-0004 for this case.**
- Avid's or the plugin vendor's algorithms are not copied, decompiled or
  cloned as a black box.
- The analog units are modelled from the original hardware: published
  schematics and the published literature.
- Console DSP comes from the published literature.
- Our own product names, and no third-party trademarks.

**What already constrains the shape.**
- **ADR-0007 §3 and §4.3.** Plugins run in a separate host process. The
  engine never links a plugin SDK and never calls plugin code. Audio crosses
  the boundary once per block per host process. A late block is dropped,
  never waited on. `latency_frames` includes any IPC pipelining delay. The
  M9 plugin ADR (after the bare-metal IPC measurements) still decides the
  wake mechanism, host granularity and worker threads.
- **`brain/specs/plugin-latency-compensation.md`.** PDC is sample-accurate
  or it is wrong. Recording is pre-insert, so what lands on disk is what came
  in. The budget at quantum 128 and 48 kHz is 2.67 ms for every track.
- **ADR-0008.** Neutral modules are standard C++17. Platform code sits at the
  edges. A Windows build of Sonora may come later, with ASIO, and its plugin
  format there would be VST3.
- **ADR-0010.** Third-party editors are X11 windows that float over the UI
  through a lazily started Xwayland. That works, but it is the least
  integrated part of the display stack.
- **ADR-0004.** It forbids copying DSP algorithms, presets, impulse
  responses, samples or UI artwork, and shipping third-party trademarks.
- **`docs/daw-mvp-plan`** (an open architect PR, owner-directed 2026-10-13)
  has already catalogued, under the Complete DAW milestone:
  - `plugins.stock`: a curated free LV2 set of stock effects;
  - `ui.mixer.loudness`: BS.1770 and R128 metering in the mixer;
  - `logic.stem_split`: mastering-assistant class tools, "ML models only
    under free licences".

  This ADR gives those items an implementation. It does not reopen their
  scope.
- **`feature/plugin-host-lv2`** exists locally (no commits beyond `main` at
  the time of writing). It is where `plugins/host` starts, LV2 first.

**What nothing in the tree yet says:**
- where our own DSP lives;
- whether it is a plugin or part of the engine;
- who owns it;
- what "good enough" means for an EQ curve or an aliasing figure;
- where machine-learning inference may run, and under what data policy.

Each of those is cheap to decide before the first processor exists. Each is
expensive after fifteen.

## Decision

### 1. Packaging: standard plugins from one codebase, not engine processors

**Our processors are ordinary plugins.**
- One DSP codebase. Each processor is built as an **LV2** bundle and a
  **VST3** bundle.
- Sonora's own plugin host loads them exactly as it loads third-party
  plugins: out of process, with deadlines, with PDC.

**1.1 Which build Sonora loads.**
- On Linux the host loads the **LV2** build. The host lands LV2 first
  (`feature/plugin-host-lv2`).
- The **VST3** build is produced and validated in CI from the first
  processor that ships (Steinberg's `validator`). An adapter added later to
  fifteen processors finds fifteen parameter-model mismatches at once. Added
  with the first one, it finds them one at a time.
- VST3 is what a future Windows Sonora would load (ADR-0008). It is also what
  any other DAW would load, if the owner ever wants that (see "For the
  owner").
- **VST2 is never built.** Its SDK licence is closed. The "clean room" VST2
  headers some frameworks carry are not something we ship (ADR-0004).
- **CLAP is not built now.** The format layer below makes it cheap to add
  later. Nothing requires it today.

**1.2 The format layer sits behind our own interface.**
- `plugins/native/sdk` defines `sonora::native::Processor`: `prepare`,
  `process`, `reset`, `latency`, timestamped parameter events, and
  state save and restore. It also defines the parameter table (§2.4).
- Each processor implements that interface and nothing else.
- The format adapters translate the interface to LV2 and VST3. The first
  implementation of the adapters uses **DPF** (DISTRHO Plugin Framework),
  pinned to a commit, with its UI layer disabled.
  - DPF is ISC-licensed.
  - It produces LV2 and VST3 from one source.
  - Its VST3 support is its own implementation, not Steinberg's SDK.
- Steinberg moved the VST3 SDK to the MIT licence with VST 3.8 (press
  release, 2025-10-29). That is reported, not checked here (Unverified 1).
- **Checkpoint.** The first processor (the EQ) must pass four checks through
  both adapters:
  - `lv2lint`;
  - Steinberg's `validator`;
  - our own host;
  - the golden tests run through the adapter, not only through the core.

  If DPF's VST3 fails `validator`, the VST3 adapter moves to the MIT SDK.
  Because no processor sees DPF, that is one adapter rewritten, not fifteen
  processors. (Amended 2026-10-13, Amendment 2 item 1: for the one failure
  N1 found, a local DPF patch was accepted instead.)

**1.3 Nothing of ours runs in the engine's audio path, except meters.**
- The engine's mixer stays what ADR-0007 and ADR-0009 say it is: gain, pan,
  mute, sum, sends and PDC delays in `engine/core`.
- The single exception is **metering**. A track or master meter is needed
  whether or not any plugin is loaded:
  - peak;
  - BS.1770 loudness (momentary, short-term, integrated);
  - true-peak.
- `engine/core` may link `common/dsp`'s meter primitives for those. Every
  call from the callback is `noexcept SONORA_RT` and audited like any RT
  callee.
- **The mix bus processor and the master chain** (console bus, bus
  compressor, mastering EQ, limiter) are inserts on the master like any other
  plugin, in the host process.

**1.4 Moving a processor into the engine path is a named, gated step.**
- It is not done now, and it is not done by drift. It needs all four of:
  1. an amendment to this ADR;
  2. the bare-metal IPC figures that feed the M9 plugin ADR (BACKLOG,
     "Blocked on hardware");
  3. a measured worst case for that processor at quantum 128;
  4. the rt-safety-auditor's sign-off.
- **The one foreseen candidate is the master chain.** Track inserts run in
  the host. The engine sums. The master inserts run in the host again. That
  is **two serial round trips per block**.
  - In the container, one round trip measured p99 210–245 µs
    (`brain/research/adr-0007-measurements.md`).
  - So two serial trips cost about 0.5 ms of a 2.67 ms period at p99.
  - If bare metal does not fit that, the master chain is the first thing
    promoted.
- Because every processor already sits on the neutral `Processor` interface
  over `common/dsp`, a promotion is a re-link plus an audit, not a rewrite.
  **That is the main reason for the library split in §2.**
- **No processor with an ML model or a neural capture is ever promoted.**
  That includes the amp family. Neither is a processor whose worst case was
  never measured.

**1.5 PDC contract for our processors.** Stricter than what we can demand of
third parties.
- Each processor reports an **exact** latency in samples for each
  configuration (sample rate, oversampling factor, look-ahead).
- Latency changes only on a **non-RT reconfiguration**. When it changes, the
  processor reports it, the host re-declares it, and the engine builds a new
  plan (ADR-0007 §2, plan swap). It never changes inside `process`.
- **Bypass is latency-compensated.** A bypassed processor still delays by
  its reported latency. Toggling bypass never shifts timing.
- A test proves that the **measured latency equals the reported latency**,
  to the sample, in every configuration (§3).

**1.6 Two boundaries that do not move.**
- **The capture path never passes through a processor of ours.** It is
  pre-insert, as the PDC spec says. No "print through the EQ" feature puts
  DSP between the interface and the take file.
- **A dropped block on a master insert during a real-time bounce fails the
  bounce.** It never produces a file with a hole or an unprocessed block in
  it. The bounce is reported as failed, and the plugin that missed is named.
  What a dropped block *substitutes* during playback (silence, or dry signal
  delayed by the reported latency) is M9's decision. This ADR records one
  input to it: for a limiter, dry is louder than processed.

### 2. Code layout, ownership and the parameter model

**2.1 Modules.** An additive amendment to ADR-0007 §1.

| Directory | Target | Owns | Owner |
|---|---|---|---|
| `common/dsp/` | `sonora_dsp` | Neutral DSP primitives: biquads and SVFs with matched designs, linear-phase FIR, oversampling (polyphase IIR half-band and FIR), dynamics detectors and gain computers, waveshapers and ADAA nonlinearities, delay lines with interpolation, LFOs, FDN reverb core, FFT, partitioned convolution, BS.1770 loudness, true-peak, pitch detection, parameter smoothing, physical-model building blocks (triode and pentode models, transformer saturation and hysteresis, tape hysteresis, seeded noise and hum generators), and `neural/`: fixed-architecture inference kernels (§4.3) | dsp-engineer |
| `plugins/native/sdk/` | `sonora_native_sdk` | The `Processor` interface, the parameter table, preset and state codecs, and the descriptor generator (§2.4) | dsp-engineer; plugin-engineer reviews |
| `plugins/native/format/` | `sonora_native_format` | The DPF-based LV2 and VST3 adapters (§1.2). DPF is vendored at a pinned commit | dsp-engineer; plugin-engineer reviews |
| `plugins/native/<name>/` | `sonora-<name>.lv2`, `sonora-<name>.vst3` | One processor: DSP graph, parameter table, presets, golden files | dsp-engineer |
| `plugins/native/measure/` | `sonora_dsp_measure`, `sonora-dsp-measure` (exe) | The measurement harness (§3) | dsp-engineer |
| `plugins/native/models/` | — | `manifest.json`: id, version, SHA-256, size and card path for every analysis model, amp capture and cabinet IR we ship. Never the bytes themselves (§4.6) | dsp-engineer; researcher checks provenance |
| `assistant/` *(N7, Proposed)* | `sonora-assistant` (exe) | The optional Claude layer (§4.5): a control-API client, and the only target with network code | ui-engineer; dsp-engineer for the features it sends |
| `plugins/analyze/` | `sonora-analyze` (exe) | Offline analysis passes: mastering suggestions, reference matching (timbre and amp tone), key detection, and the mix assistant's planner (§4.4). The only target that links a general inference runtime (§4) | dsp-engineer |

**2.2 Allowed edges.** Additions to ADR-0007 §1. Any edge not listed is
forbidden, as before.

```
layer 1   common/dsp ──> common/base
          plugins/native/sdk ──> common/base, common/json
layer 2   plugins/native/format ──> plugins/native/sdk           (DPF: format only)
          plugins/native/<name> ──> common/dsp, plugins/native/sdk, plugins/native/format,
                                    common/json, common/audiofile  (capture and IR loading, never in process)
          plugins/native/measure ──> common/dsp, plugins/native/sdk, common/audiofile
          plugins/analyze ──> common/dsp, common/audiofile, common/json   (ONNX Runtime: analyze only)
          engine/core ──> common/dsp                              (meters only, §1.3)
          ui/qt ──> common/dsp                                    (curves and analysers, non-RT)
layer 3   assistant (N7) ──> ui/client, contracts/api, common/model, common/json   (HTTPS: assistant only)
```

- **`ui/qt` links `common/dsp`.** An EQ's drawn curve must come from the
  same coefficient code that processes the audio. A curve computed twice
  drifts. The UI also runs the FFT for analyser displays itself (§2.6).
  Neither call is RT.
- **`plugins/host` never links `plugins/native`.** It loads our bundles as
  files, like anyone else's. That is what makes "our plugins are ordinary
  plugins" true, and it is what keeps the host's conformance tests honest.
- **Nothing under `plugins/native` depends on `engine/*` or `ui/*`.** A
  processor knows nothing about the product it runs in.
- **`common/dsp` uses no I/O, no threads, no format headers, no Qt and no
  inference runtime.** It is arithmetic.
- **ADR-0007 §1 says "Nothing outside `plugins/host` links a plugin SDK".**
  It is amended as follows:
  - `plugins/native/format` may include the plugin-side LV2 and VST3
    headers that DPF carries;
  - it produces loadable binaries that no Sonora process links.

  M5's forbidden-include check names `plugins/native/format` as the second
  allowed place for those headers, and only for the plugin side.

**Amendment 1 to §2.2, 2026-10-13 (coordinator, on the review of PR #34,
stage N0).** Two edges are added to the table:

```
layer 1   plugins/native/sdk ──> common/dsp            (flush-to-zero guard and smoothers, in the base class)
layer 2   plugins/native/measure ──> common/json       (reads quality.json, writes the JSON reports)
```

- **`plugins/native/sdk → common/dsp`.** `ProcessorBase` gives every
  processor flush-to-zero for the whole of `process()` and its parameter
  smoothing. Both must be the same code the processors use: an FTZ guard
  written twice is the arch-specific code ADR-0008 and §2.3 confine to
  `common/dsp/src/arch/`, and a second smoother is a second definition of
  "smoothing time", which the zipper bar is measured against. Both
  modules are neutral and layer 1, so no layering rule is crossed; the
  edge points from the interface to the arithmetic, never back.
- **`plugins/native/measure → common/json`.** §3.1 puts the thresholds in
  `quality.json` and has the harness write one JSON report per processor.
  `common/json` is the project's canonical JSON (ADR-0009 §5), so the
  harness reads and writes it through that, not through a second parser.
  The harness is offline and never on an audio thread.
- No other edge changes. M5's dependency check takes both.

The same review settled four more points for N0; they are recorded under
"Amendment 1" after "For the owner".

**2.3 Portability and real time.**
- **Neutral modules (ADR-0008 §1):** `common/dsp`, `plugins/native/sdk`, and
  each `plugins/native/<name>/src/`. `plugins/native/format` is admitted
  DPF's headers, as ADR-0008 §4 allows a later ADR to do.
- **Code specific to a CPU architecture** lives in
  `common/dsp/src/arch/<arch>/`, chosen by CMake from the target
  architecture, never by `#ifdef`. This covers flush-to-zero and
  denormals-are-zero, and any SIMD intrinsic. There is always a portable
  `generic/` implementation.
  - This is ADR-0008's rule applied to the architecture rather than the OS.
  - The textual check treats `arch/` the way it treats
    `common/platform/src/<os>/`.
  - arm64 is the case that makes this matter. The appliance is amd64 today.
- `Processor::process` and every `common/dsp` function it calls are
  `noexcept SONORA_RT`.
- Allocation, table building and FFT planning happen in `prepare(rate,
  max_frames, channels)`, never in `process`.
- Coefficient updates on a parameter change are computed in `process`. That
  is bounded arithmetic, so it is allowed.
- `-ffast-math` (and MSVC `/fp:fast`) is forbidden in `common/dsp` and
  `plugins/native`. It makes golden files compiler-dependent, and it breaks
  NaN handling.
- **The RTSan CI job** (ADR-0007, first update) runs the measurement harness
  and the golden tests. Those are what execute `process`.

**2.4 The parameter model: one table, everything generated from it.**
- Each processor declares a static parameter table. Each entry has:
  - a stable string id, never reused;
  - an index;
  - a range;
  - a unit;
  - a taper;
  - a default;
  - a smoothing time;
  - whether it is automatable;
  - a display format.
- From that one table, `plugins/native/sdk` generates:
  - the LV2 TTL and VST3 parameter info;
  - a JSON **descriptor** shipped inside each bundle, which the UI reads
    (§2.6);
  - the preset schema.
- Ids are **append-only**, like ADR-0007's `ParamId`. Renaming or removing
  one is a breaking change to every saved session. It needs a state
  migration and a version bump.
- **Smoothing is mandatory.** A parameter change never produces a step in
  the audio. §3's zipper test enforces it.
- **Automation.**
  - `Processor` receives parameter events with a sample offset inside the
    block.
  - The VST3 adapter passes VST3's offsets through. (Amended 2026-10-13,
    Amendment 2 item 2: through DPF it cannot. A change at a nonzero
    offset lands one block late and intermediate points are dropped,
    accepted as block-rate delivery like LV2's.)
  - LV2 control ports are per `run()` call, so the LV2 adapter delivers at
    block start. Sonora's host may split a block at automation points if
    the M9 ADR wants sample accuracy for LV2.
  - At quantum 128, with smoothing, block-rate delivery is 2.67 ms and is
    not audible as a step. That is an assumption to check by listening, not
    a measurement.
  - Engine automation (`automation.engine` on the DAW plan) reaches the host
    through ADR-0007 §4.3's parameter SPSC ring.
- **Presets are ours.**
  - They are canonical JSON through `common/json` (ADR-0009 §5), in
    `plugins/native/<name>/presets/`.
  - Each carries the processor id, its state version and parameter values
    by string id.
  - A preset naming an unknown id is **refused with a message**. A missing
    id takes its default.
  - Every factory preset is written by us, from a measurement or by ear.
    None is transcribed from another product's preset list (§5).
- **State** is the parameter values plus a versioned, processor-specific
  blob (for example a reference curve from §4). Each version bump comes with
  a migration and a test that loads the previous version's golden state.

**2.5 Ownership.**
- **A new agent role, `dsp-engineer`** (`.claude/agents/dsp-engineer.md`,
  part of this ADR). It owns `common/dsp`, `plugins/native/*` and
  `plugins/analyze`, the model manifest, and the measurement harness.
- **The plugin-engineer** keeps `plugins/host`, hosting and interchange. It
  reviews `plugins/native/sdk` and `format`, because our bundles must load
  in its host by the same rules as everyone's.
- **The ui-engineer** owns the editor panels in `ui/qt` (§2.6).
- **The researcher** checks every model's data provenance before the model
  enters the manifest (§4.6).
- **Gates.**
  - `code-reviewer` reviews every diff.
  - `rt-safety-auditor` reviews every diff to a `process` path. That means
    `common/dsp`, `plugins/native/<name>/src` and anything `engine/core`
    calls.

**2.6 Editors: drawn by the Sonora UI, not by the plugin.**
- A native processor's editor is a **Qt Quick panel inside `sonora-ui`**.
  - It is built from the bundle's descriptor, plus a per-processor QML file
    under `ui/qt`.
  - It follows the shell's visual language (ADR-0006) and a UI design spec
    when one exists.
  - It opens docked in the mixer or as an internal window. It is never an X11
    window through Xwayland (ADR-0010).
  - The plugin binaries contain **no editor code at all**.
- **Parameter path:** UI → engine (control channel) → host (ADR-0007 §4.3 parameter
  ring). The UI stays a client of the engine only (ADR-0007 §3).
  `ParameterChange` grows a plugin-parameter target. That is an additive
  `contracts/api` change, made with M9.
- **What an editor displays.** Examples: gain reduction, input and output
  levels, the tuner's detected pitch, an EQ's analyser.
  - The host publishes these per instance into a **read-only shared
    region**, pulled by the UI at 60 Hz. ADR-0006's rule holds: the audio
    thread never pushes.
  - Analysers get a lossy sample tap with a sequence counter. The UI runs
    the FFT itself, with `common/dsp`.
  - The region's layout is part of `contracts/plugin` and is settled by the
    M9 ADR. This ADR only fixes that it exists, is read-only to the UI, is
    written with atomics only by the host's RT thread, and is paced.
- **In another host** (a foreign DAW, or a Windows Sonora before its UI
  supports this), our bundles show the host's generic parameter UI. Building
  custom editors for other hosts is not in scope (see "For the owner").

### 3. Quality bar and validation

**3.1 The harness.**
- `sonora-dsp-measure` renders a processor offline through the `Processor`
  interface. In CI it also renders through the LV2 adapter, loaded by a
  minimal `lilv` driver. It writes one JSON report per processor,
  configuration and sample rate.
- Inputs are generated, never recorded:
  - log sweeps;
  - steps and tone bursts;
  - multitone;
  - seeded noise;
  - EBU loudness test signals.
- Reports go to `build/dsp-reports/`. Thresholds live next to each processor
  (`plugins/native/<name>/quality.json`), so a looser bar is a reviewed diff.

**3.2 What is measured, with initial thresholds.** These are first bars, to
be confirmed or tightened once `brain/research/native-plugin-suite.md`
lands. A processor does not ship in the default insert list until it meets
them.

| Measure | Applies to | Initial bar |
|---|---|---|
| Latency: measured equals reported | All, every configuration and sample rate | Exact, to the sample |
| Magnitude response against the design target | EQ, filters inside every processor | ±0.1 dB from 20 Hz to 20 kHz at 44.1 and 48 kHz, including near Nyquist. Bilinear cramping is not accepted; matched or oversampled designs from the literature are |
| Phase response | Minimum-phase modes | Within ±5° of the analog prototype to 10 kHz |
| Aliasing (non-harmonic energy on a 1–20 kHz sweep at nominal drive) | Every nonlinearity: tube, tape, opto and FET stages, clipper, limiter | ≤ −90 dBc in the default quality, ≤ −70 dBc in an "eco" mode |
| THD+N against level | Clean processors (EQ, digital compressor at unity, delay) | ≤ −110 dB at −1 dBFS |
| THD against level, harmonic profile | Analog models | Within a stated tolerance of the reference curve (§3.3) |
| Attack and release | Dynamics | Within ±10 % of the displayed value, from step tests. Analog models follow their reference curve instead |
| Loudness and true-peak | Meters, limiter, mastering console | EBU Tech 3341 and 3342 test cases, at each case's stated tolerance |
| Ceiling | Limiter | Zero true-peak overs above the set ceiling on a stress set, measured with a meter oversampled more finely than the limiter's own |
| Denormals | All | Per-block CPU after 10 s of silence following signal is at most 1.2× the active figure |
| Non-finite input | All | Output is always finite. It recovers within one block of valid input |
| Zipper | All automatable parameters | No step above −60 dBFS on full-range parameter sweeps |
| Neural kernel equivalence | `common/dsp/neural` on every shipped capture | Maximum absolute difference ≤ 1e-5 against the NAM project's MIT reference implementation, run in the test only, on the same input |
| Capture accuracy | Each shipped amp capture | Error-to-signal ratio against the re-recorded hardware on held-out material, recorded on its card. The bar is set per architecture once Unverified 9 is measured |
| Modelled noise and hum | Tape, tube, transformer, amp | Level and spectrum within a stated tolerance of the reference (§3.3). At zero setting, digitally silent |

**3.3 Analog character comes from physical models, validated against the
circuit, never against another product.**

**Physical modelling from the hardware literature is the method** for every
analog-character processor (owner, 2026-10-13). For each one, the model
comes from published circuit analysis and device physics, and the paper is
cited in the processor's source:
- **Tubes:** published triode and pentode models (Koren-family and later
  refinements), with grid current and the power-supply sag the circuit
  implies.
- **Harmonic distortion:** what the modelled devices produce. Not a
  waveshaper tuned by ear to a harmonic recipe.
- **Transformers:** core saturation and hysteresis, frequency-dependent
  (LF saturation first).
- **Tape:** magnetic hysteresis (the Jiles–Atherton family), head bump and
  gap loss, and the record and playback EQ standards.
- **Noise and hum:**
  - tape hiss and device noise floors with the published spectral shape and
    level;
  - mains hum at 50 or 60 Hz with its harmonic series.
  - Each is generated from a seeded generator, so golden files stay
    deterministic.
  - Each has its own level parameter. At zero it is digitally silent, so an
    operator can always remove it.
  - Hum and noise do not stack silently: each instance's contribution is
    shown in its editor. Eighteen tracks of modelled hiss is a noise floor
    the operator must be able to see and switch off.

Neural captures (§4.3) are used **only for the amp and cabinet family**,
where the owner asked for them. Elsewhere a capture would replace a model
we can explain with one we cannot.

**References for validation:**
- The reference for an opto or FET compressor, the tube stage or the tape
  path is a **SPICE simulation** (ngspice, free) of the published schematic,
  plus published measurements of the original units.
- When the owner can provide them, measurements of real hardware units are
  also references. Measuring hardware is not reverse engineering software.
- **A commercial plugin's output is never used as a reference, a fitting
  target or a null-test partner.** That would be cloning the vendor's
  emulation as a black box, which is the coordinator's constraint and
  ADR-0004.

**3.4 Golden files.**
- Each processor has rendered outputs for:
  - a fixed set of configurations and presets;
  - at 44.1, 48 and 96 kHz;
  - on 1–2 s generated inputs.
- Compared with a maximum absolute error of 1e-6 (−120 dBFS), the tests
  pass under GCC, Clang and MSVC. The MSVC side is ADR-0008's `portable`
  job.
- Regenerating a golden file is a deliberate commit. Its message says why the
  sound changed and carries the harness report before and after.
- Golden outputs are small float WAVs in the processor's `tests/`. Inputs
  are regenerated from seeds, not stored.

**3.5 CPU budget per instance.**
- Measured as worst case per block, p99.9 and max, at quantum 128, 48 kHz,
  stereo, in a single thread, over a 60 s render of the harness signals.
- Recorded with the CPU model, on the reference machine. That is the
  owner's laptop until the target machine exists (`docs/daw-mvp-plan`
  measures there first).

| Class | Processors | p99.9 per block | Max |
|---|---|---|---|
| A, channel | EQ, gate, compressor, delay, chorus, opto, FET | ≤ 20 µs | ≤ 50 µs |
| B, colour | Tube, tape, mix bus (oversampled) | ≤ 60 µs | ≤ 150 µs |
| C, space and bus | Reverb, limiter with true-peak, tuner | ≤ 150 µs | ≤ 300 µs |
| D, chain | The whole mastering console | ≤ 400 µs | ≤ 800 µs |
| E, neural amp | One amp capture plus its cabinet IR, mono in, stereo out, at the capture's native rate with resampling if the session differs | ≤ 250 µs | ≤ 500 µs |

- **Class E is enforced at load.** When an amp capture is loaded, it is
  benchmarked during `prepare` against a table of known architectures. A
  capture whose architecture costs more than class E on this machine is
  **refused with a message**. It is never loaded and allowed to miss
  deadlines in a session.
- Shipped captures use the smaller NAM-class architectures by default.
  Which sizes fit is Unverified 9.
- Guitar tracks are typically few, so class E is sized for about four
  instances in a session, not eighteen.

- The arithmetic behind class A: the Phase 4 exit (18 tracks × 4 plugins)
  at 20 µs each is 1.44 ms serial. That fits in 2.67 ms on two workers with
  headroom for the IPC round trip. It does not fit on one.
- A processor over its class budget ships only with an eco mode that meets
  it.
- These numbers are set from the arithmetic, not from a measurement on our
  hardware (Unverified 4).

### 4. AI: where inference runs

**4.1 The rule: AI chooses parameters, DSP makes the sound.**
- Every AI feature produces **settings for ordinary processors**: EQ bands,
  thresholds, a target loudness, a key.
- It never produces audio directly in the real-time path.
- The settings are applied as ordinary, undoable parameter changes (ADR-0009
  operations), shown to the operator, and saved in the session.

What this buys:
- the sound is deterministic, automatable and recallable without the model;
- a model update never changes an existing mix;
- "what to cut" is a visible suggestion, not a hidden transformation.

**The one exception is the amp and cabinet family.** There the neural
capture *is* the sound, at the owner's request. It is treated as a DSP
model with fixed weights, not as an AI feature:
- the capture is chosen by the operator;
- it is saved in the session by id and hash;
- it never changes unless the operator changes it.

**4.2 Four tiers.**

| Tier | Where it runs | What runs there |
|---|---|---|
| Real time, DSP only | Plugin host, in `process` | The vocal tuner: classical pitch detection (YIN-family, published) and pitch shifting (PSOLA or a phase vocoder, published), snapping to the scale of the session's key. The EQ, dynamics, reverb and delay that apply the AI's suggestions |
| Real time, fixed-architecture neural DSP | Plugin host, in `process`, through `common/dsp/neural` only | The amp captures (§4.3). Nothing else without an amendment |
| Analysis passes | `sonora-analyze`, a separate short-lived non-RT process launched by `engine/control`, like `sonora-convert` (ADR-0007 §3) | Mastering-console suggestions on a bounce of the mix. Reference timbre matching: the long-term spectral envelopes of a user-supplied reference and of the track, an instrument classifier, and the fitted EQ curve. "Artist tone": suggesting an amp capture, its settings, a cabinet and an EQ match from a user-supplied reference. Key detection. Source separation where a reference needs it. The mix assistant's planner (§4.4) |
| Per-block inference through a general runtime | **Nowhere, for now** | Nothing. Refused until the measurement in Unverified 5 shows it can be done without allocation and with a bounded worst case |

- **Analysis reads files, not the live graph.** It reads source files and
  bounces. So it has no coupling to the audio thread, and its crash costs a
  dialog, not a block.
- Results come back as a JSON file plus an event. The UI shows them as
  suggestions. Applying one is a normal parameter change.
- The key the tuner follows is **the session's key**, set by the operator or
  by accepting a key-detection suggestion. It needs a key field in the
  session model (the tempo and key map on the DAW plan). Until that exists,
  the tuner takes its key and scale as parameters.

**4.3 Two inference paths, and why there are two.**

**Real-time neural DSP for the amps runs in the plugin host**, as the owner
asked, but not through a general runtime.
- `common/dsp/neural` implements the **fixed architectures** that NAM-class
  captures use: the WaveNet-style dilated convolution stacks and the small
  LSTM. It is plain C++17 kernels.
- Weights are loaded and every buffer is sized in `prepare`. `process`
  performs fixed arithmetic: no allocation, no locks, no thread pool,
  `noexcept SONORA_RT`. The RTSan job covers it like any other `process`.
- **Model format.**
  - Amps: the NAM project's open `.nam` format (JSON: architecture,
    configuration, weights, sample rate). It is read with `common/json` at
    instantiate and is never touched in `process`.
  - Cabinets: mono or stereo WAV impulse responses, read with
    `common/audiofile`, run by partitioned convolution.
  - Both formats are open. An operator may load their own `.nam` files and
    IRs. What they load is their business. What **we ship** follows §4.6.
- The kernels are written by us. The NAM project's MIT reference
  implementation is a **test oracle** in the equivalence test (§3.2). It is
  not linked into the bundle.
  - Its dependency, Eigen, is MPL-2.0, which is file-level copyleft.
  - Keeping it out of the shipped bundle keeps "For the owner" item 1
    open.
- **CPU:** class E (§3.5), enforced at load. An architecture the machine
  cannot afford is refused, never admitted to miss deadlines.
- Captures are trained at one sample rate, usually 48 kHz. At another
  session rate the processor resamples, and the resampler's latency is
  part of the reported latency.

**General inference (ONNX Runtime, CPU execution provider, MIT-licensed) is
linked only by `sonora-analyze`.**
- Never by the engine, never by `common/dsp`, never by the plugin host.
- The host is where RT plugin processing happens. A general runtime brings
  a thread pool and an allocator that no measurement has cleared, and its
  crash would take every plugin in that host down.
- If Unverified 5 ever admits it per block, that is an amendment naming the
  one processor, not a default.
- Analysis threads run at normal priority, `nice`d, with a bounded thread
  count. While the transport is recording, analysis waits.

**4.4 The AI mix and mastering assistant.**

**What it is.** A planner that applies the workflow of a well-run studio to
the session:
- gain staging;
- bus structure (instrument groups and a mix bus);
- parallel compression and saturation;
- shared reverb and delay sends;
- the mix-bus chain;
- mastering (N4's console).

**Its output is a plan: a list of ordinary session operations.** For
example: add an aux, route these tracks to it, insert our compressor on it
with these settings, add a send at this level, trim this clip gain. The
operations are those of `common/model` (ADR-0009), so:
- the UI shows the plan as a reviewable diff before anything changes. The
  operator can accept all of it, part of it, or none;
- accepting it is **one undoable compound operation** through the model's
  `apply`. One undo removes all of it. Every change is a visible track,
  send, insert or parameter value;
- **there is no hidden processing.** The assistant never inserts a
  processor that is not shown in the mixer. It never changes audio files,
  and it never runs anything the operator cannot open and edit.
- It uses **only our native processors**, whose parameters it understands
  through their descriptors (§2.4). Third-party plugins in the session are
  left alone and named in the plan as "not adjusted".
- The workflow rules come from the published engineering literature and
  our own engineers' practice, written as data under
  `plugins/analyze/rules/` with tests. They are not taken from any
  product's assistant.

**Its first concrete feature (owner example, 2026-10-13): reverb and delay
on instruments and vocals.** The right time, the ideal time for the song's
style, and the ideal amount so it is not excessive, matched to an artist's
reference.
1. **Time from the tempo.**
   - Delay times are derived from the session's tempo map: note values,
     dotted and triplet.
   - Reverb pre-delay and decay are aligned to the beat grid, for example
     the decay set so the tail falls inside a chosen note value.
   - The tempo map is session data (DAW plan, tempo and key map). Until it
     exists, the feature takes a tempo parameter.
2. **Style and amount from a reference.** An analysis pass on a reference
   the user supplies estimates:
   - the decay time (RT60-style estimation from decaying segments);
   - the pre-delay;
   - the delay time and its feedback;
   - the wet-to-dry level, per instrument or for the vocal.
   - Where the reference is a full mix, **source separation** isolates the
     vocal or the instrument first. Separation is an ML model in
     `sonora-analyze`. Its weights fall under §4.6 like any model, and the
     common open separation models are trained on research-only datasets
     (Unverified 10).
3. **An "excess" check.**
   - Masking and clarity metrics, computed by rendering the session's
     parts with and without the wet returns:
     - spectral masking of the dry vocal by the wet return;
     - a clarity measure (C50 or C80 family) of the result;
     - wet-to-dry energy in the vocal's intelligibility band.
   - These flag a reverb or delay that is too long or too loud, and the
     check runs both on the assistant's own proposal and on what the
     operator set by hand.
4. **Applied as edits.**
   - It creates shared reverb and delay auxes if absent (our FDN reverb and
     delay, §6 N0 and N2), sets their parameters, and adds sends at the
     estimated levels.
   - All of this is one undoable plan, with each value shown next to the
     measurement it came from: "decay 1.9 s: reference estimate 1.8–2.1 s;
     tail ends on the half note at 92 BPM".

**4.5 An optional Claude reasoning layer.**

Status of this subsection: **Proposed.** The owner confirms cost and privacy
later. The owner asked whether Claude can be plugged into the plugins, or
whether everything will be native. The proposal is a **hybrid**.

**Native: everything in or near the audio path.**
- DSP and the neural amp models.
- Every analysis: spectrum, loudness, masking, key and tempo, and the
  reverb and delay estimation from a reference.
- The vocal tuner.
- All of it runs offline and RT-safe where it must, and **no audio leaves
  the machine**. Everything in N0–N6 works with no network.

**Claude: an optional reasoning layer above the native analysis.** It adds:
- natural-language intent, for example "vocal upfront like this
  reference";
- reasoning about the whole mix.

**It sees analysis features and session state, never raw audio.**
- What it sees:
  - `sonora-analyze`'s results (spectral and loudness figures, masking
    and clarity metrics, reverb and delay estimates, key and tempo);
  - the session's structure from `common/model` (tracks, buses, sends,
    inserts, parameter values, names).
- Its output is **the same kind of plan as §4.4**: a list of `common/model`
  operations. Examples: EQ cuts, sends, reverb and delay settings,
  parallel chains, each with an explanation.
- The plan goes through the same path as a native plan:
  - it is validated by the model's `apply` and by the processors'
    parameter descriptors, so an unknown operation or an out-of-range value
    is refused;
  - it is shown as a diff with the explanations;
  - it is applied only when the operator accepts, as one undoable
    operation (ADR-0009).
- Claude never applies anything itself.
- **Prompt injection is bounded by that schema.** Track names and file
  metadata reach the model, and the model's reply can only ever be a plan of
  valid operations that the operator reviews. The worst case is a bad
  suggestion, refused or undone.

**Where it runs.**
- A separate, non-RT process, `sonora-assistant`. It is a client of the
  control API (`contracts/api`, available from M4), as the UI and the
  control-surface bridge are (ADR-0007 §4.4). It reads the analysis results
  files.
- It is neither the engine nor the plugin host, and nothing in the RT path
  waits on it.
- It is the only target with network code: HTTPS to the API. It is an
  edge module under ADR-0008.
- It is owned by the ui-engineer, because it is a client, with the
  dsp-engineer for the analysis features it sends.

**It degrades gracefully.** With no network, no API key, an API error, or a
rate limit:
- native suggestions only;
- the UI says that the Claude layer is unavailable and why;
- it never hangs waiting for it.

Every suggestion is labelled with its source, native or Claude.

**Open, for the owner** ("For the owner", item 8):
- whether the appliance gets network access at all;
- where an API key is entered and stored. On the persistence partition,
  never in a session file, is the proposal;
- who pays, and the cost per request;
- rate limits;
- a **privacy statement** saying exactly what leaves the machine: feature
  values, the session structure, and the names the operator typed. No audio,
  ever.

**4.6 Models, captures and IRs: provenance, licensing, versions, size.**
- **Every model has a card in `brain/models/<model-id>.md`.** It records:
  - the architecture;
  - the training code commit;
  - every training dataset, with its licence and where it came from;
  - the evaluation figures;
  - the SHA-256;
  - the size.

  The researcher checks the data's licences before the model enters the
  manifest. The card exists before the model does.
- **Training data is only data we own or hold a licence for**, recorded per
  dataset:
  - our own recordings, with release forms from performers;
  - datasets whose licence allows training and redistribution of the
    resulting weights.
- Never allowed:
  - scraped commercial recordings;
  - **audio rendered through a commercial plugin**, which would be
    black-box cloning by another route;
  - any dataset whose licence is unclear.

  "Unclear" means excluded.
- **Amp captures and cabinet IRs we ship.** Each has a **capture card** in
  `brain/models/`. The card records:
  - the unit: type, serial number, and owned or rented;
  - for a rented unit, that the rental terms do not forbid recording it;
  - its settings;
  - the capture rig and date;
  - for IRs, the cabinet, speaker, microphone, position and room.

  Only hardware we own or rent is captured. Never allowed:
  - third-party captures or IRs (downloaded, bought or community-shared)
    without a written licence that allows redistribution, recorded on the
    card;
  - captures of a commercial amp *plugin*, which is black-box cloning.
- **Artist tones.** A capture or preset aims at a style of sound. It is
  named by its sound and by the circuit class ("British-voiced EL34 head,
  crunch", "4×12 closed-back, dynamic mic, cap edge"). **Never by a
  guitarist, a band, a record or an amp brand.** "Sound like this artist"
  is reference matching on audio the user supplies (§4.2). The result is
  the user's own preset, which they may name as they like.
- **Reference tracks the operator supplies**, for timbre, tone or reverb
  matching, are
  analysed locally. Only the derived curves are stored in the session. The
  audio is not copied into the session, and nothing is sent anywhere. The
  appliance has no network path for this.
- **Model files are not in git.**
  - `plugins/native/models/manifest.json` pins each model by version and
    SHA-256.
  - The build fetches release artefacts and verifies them, and **fails the
    build on a mismatch**.
  - `sonora-analyze` verifies the hash again at load.
  - A missing or mismatched model **disables that feature with a message**.
    It never falls back silently to another model.
- **Size bounds:** at most 25 MB per model and 100 MB for all models,
  captures and IRs on the image. A NAM-class capture is typically well
  under 1 MB, so the budget is mostly the analysis and separation models.
  These bounds are placeholders until the owner rules ("For the owner",
  item 5). ADR-0010's image budget is 1.5 GB. Any growth past these bounds
  is an amendment.
- **A model version bump** re-runs that feature's golden analysis tests:
  fixed inputs, with expected suggestions inside a tolerance. The new
  version must reach the previous evaluation figures, or the card says why
  not.

### 5. Legal and brand

- **Names are ours.**
  - Product and processor names are functional, or our own coinages.
  - No third-party product name, model number, company name or
    near-variant. "Near-variant" means no adjacent model numbers and no
    letter swaps that point at the original.
  - Directory names are functional (`eq`, `opto`, `fet`, `tape`, `tube`,
    `reverb`, `master`, `amp`, `cab`).
  - No guitarist, band or record name appears in a product, capture or
    factory preset name (§4.6).
- **No trade dress.** Editors use Sonora's visual language. None imitates a
  hardware faceplate, its colour scheme, its knob set or its meter.
- **Nothing copied:** no presets, impulse responses, samples, artwork,
  manuals or text from any vendor (ADR-0004). Factory presets are written
  by us.
- **Code from others.**
  - Free-licensed code may be reused, as ADR-0004 allows, with the licence
    and source recorded in the file and in `brain/`. Until the owner rules
    on the suite's licence ("For the owner", item 1), only **permissive**
    third-party code (MIT, BSD, ISC, zlib) enters `common/dsp` and
    `plugins/native`.
  - Published papers are implemented from the paper.
- **How inspiration may be described.**
  - Inside the product (UI, presets, bundle metadata, manuals) we describe
    the circuit class only. For example: "optical levelling amplifier",
    "FET peak limiter", "tape machine record and playback path".
  - We never name the original product, or the vendor whose emulation the
    owner had in mind.
  - Marketing outside the product follows the same default. Naming an
    original unit, even nominatively with a disclaimer, is the owner's
    decision with legal advice (item 2). It is not an agent's decision.
- **Refusals.** A task that asks to match, null against, or train on a
  named commercial plugin's output, or to reproduce its presets, is
  refused with a pointer to ADR-0004 and this section.

### 6. Order of work inside the Complete DAW milestone

Stage numbers below are this ADR's own (N0–N7). Where they fall among the
DAW stages is in the BACKLOG and `planning/progress.json` items. Everything
after N0 builds on the DSP core, as the owner asked.

| Stage | What | Needs first |
|---|---|---|
| **N0**, startable now | The `dsp-engineer` charter (this PR). `common/dsp` core: filters, smoothing, oversampling, detectors, delay lines, BS.1770 loudness and true-peak. The measurement harness and golden infrastructure. `plugins/native/sdk`. Then **EQ, gate, compressor, limiter, delay, chorus** as `Processor`s, measured and golden-tested offline | Nothing. No host and no engine needed; all offline. M5's check gets the new edges |
| **N1**, audible in Sonora | `plugins/native/format` (DPF, LV2 and VST3), validator and `lv2lint` in CI, bundles installed into the image. The descriptor-driven Qt panels. The telemetry region. Engine meters on `common/dsp` | `feature/plugin-host-lv2` merged (the host loads our LV2). M9: `contracts/plugin` with the parameter ring and the telemetry region. `ui.mixer.inserts` and `plugins.graph.parallel` on the DAW plan for inserts on real tracks |
| **N2**, colour and space | FDN reverb, tape, tube, transformer, the opto and FET models, all physical models (§3.3) with SPICE references first. Modelled noise and hum. The mix bus. The tuner's DSP, with key and scale as parameters | N0's oversampling and the aliasing harness. ngspice in the build container |
| **N3**, amp and cabinet | `common/dsp/neural` (§4.3), the `.nam` reader, partitioned convolution, the amp and cab processors. The capture rig and its procedure. Our first captures and IRs, each with a card | N0. The class-E measurement (Unverified 9) before the first capture is made, so we capture at an architecture we can run. Hardware owned or rented (owner, item 6) |
| **N4**, mastering console | The master chain (EQ, multiband and broadband dynamics, limiter, loudness target) as one processor. `sonora-analyze` with the mastering-suggestion pass, at first rule-based from loudness and spectral measurements, with no model | N1 and N2. Bounce to disk (`ui.mixer.bounce`). Undoable parameter operations (ADR-0009) |
| **N5**, AI mix assistant: reverb and delay first | The plan-as-operations machinery (§4.4). Its first feature: tempo-synced delay and reverb, reference-based estimation of decay, pre-delay and wet level, the excess check, and sends applied as one undoable plan. Then gain staging, bus structure, parallel compression and saturation, and the mix-bus chain, each as a rule set with tests | N2's reverb. N4's analysis process. The DAW plan's buses, sends and tempo map, and `common/model` operations for tracks, sends and inserts. ONNX Runtime and a cleared separation model only for full-mix references; a stem or solo reference needs none |
| **N6**, model-based AI | ONNX Runtime in `sonora-analyze`. Model cards and the manifest. Key detection. Reference timbre matching. Artist-tone matching for the amps. The tuner following the session key. Model-assisted mastering and mix suggestions | N4 and N5. The session's key field (DAW plan, tempo and key map). For each model, a researcher-cleared card |

| **N7**, Claude layer (Proposed, §4.5) | `sonora-assistant`: intent in natural language, whole-mix reasoning over N4–N6's analysis features and the session state, plans with explanations through the same review and undo path. Graceful fallback to native suggestions | N5's plan machinery and N6's analysis features. M4's control API. The owner's rulings on network, key handling, cost and privacy (item 8). Not started before those rulings |

- N5 comes before N6 on purpose. The reverb and delay feature is mostly
  measurement and arithmetic (tempo, decay estimation, masking). So the
  assistant's machinery (plans, diffs, undo) is proven before any model's
  output flows through it.
- The mastering console ships useful without any model (N4). Models make
  its suggestions better (N6). They do not make them possible.
- **On `plugins.stock`** (the curated free LV2 set on the DAW plan): it
  stays as a stop-gap until each native processor in the same category
  meets §3. The owner decides whether it ships afterwards (item 4).

## Rationale

- **One mechanism, at the price of one extra round trip on the master.**
  - Making our processors ordinary plugins means PDC, deadlines, crash
    containment, state, presets and automation are built once, for
    everyone's plugins.
  - The host's conformance tests run against our bundles as well, and
    those are the bundles we control best.
  - Internal processors would need a second processing path inside the
    engine, with its own state, its own automation and its own audit
    surface, in the process that holds the take.
- **The library split is what keeps the engine option open.** If bare
  metal shows the master chain cannot afford two host round trips, moving
  it costs a re-link and an audit, because the DSP never knew it was a
  plugin. Without the split, that discovery costs a rewrite.
- **Our own interface over DPF, not DPF everywhere.** DPF saves writing two
  format adapters. Confining it to `plugins/native/format` means the
  framework can be replaced in one directory if its VST3 fails validation.
- **Editors in `sonora-ui`.**
  - Native editors in the UI avoid the least integrated piece of ADR-0010
    (X11 windows over Wayland).
  - They keep Qt in exactly one target (ADR-0007).
  - They keep a second Qt instance out of a process that also loads
    third-party plugins. Two Qt versions in one address space is a known
    source of crashes.
  - The cost is that our plugins have no custom editor in other hosts.
- **AI chooses parameters.** This is what makes the feature compatible with
  the product's first rule:
  - no inference on the audio thread;
  - nothing nondeterministic between the operator and the sound;
  - a crash in the AI costs a suggestion, never a block.
  - It is also what the operator can trust: a suggested cut is visible and
    reversible.
- **Neural amps in the host, on our own kernels.**
  - The owner put neural inference in the host, and for the amps that is
    the only place it can go: the capture is the sound.
  - Fixed architectures with weights sized at `prepare` are ordinary DSP
    from the audio thread's point of view: bounded arithmetic, auditable,
    covered by RTSan.
  - A general runtime is none of those, so it stays in `sonora-analyze`.
- **The assistant's output is session operations.** That makes "never
  hidden processing" a property of the design rather than a promise:
  - there is no channel through which it could process audio invisibly;
  - undo already exists (ADR-0009);
  - a plan is reviewable as a diff before anything changes.
  - The optional Claude layer (§4.5) reuses that path unchanged. Adding it
    adds a source of plans, not a new way to change the session.
- **Validation against the circuit.** SPICE of a published schematic is a
  reference we may legally use, can regenerate, and can explain. A
  commercial emulation's output is none of those.
- **Measured bars, written down.** "Sounds good" has no regression test. An
  aliasing figure and a golden file do, and a reviewer can read a threshold
  diff.

## Consequences

- **New module surface.** Five directories and an executable, plus the
  edges in §2.2. M5's dependency check and the ADR-0008 allowlist grow
  accordingly. ADR-0007 gets a dated note pointing here when this ADR is
  accepted.
- **Two extra contract items for M9.** A plugin-parameter target on
  `ParameterChange`, and the read-only telemetry region. Both are additive.
  Neither changes anything that exists today.
- **CI grows.**
  - harness and golden tests in the `engine` job;
  - `lv2lint` and Steinberg's `validator` from N1;
  - RTSan over the harness;
  - golden tests under MSVC through the `portable` job.
  - The harness renders are short, but the CI time is unmeasured.
- **The build fetches model artefacts** from N3 on (captures and IRs),
  so a hash mismatch
  breaks the build. That is the intended loud failure. It is also a new
  network dependency of the image build, which today needs only Debian
  mirrors.
- **Our plugins look generic in other DAWs.** They have only a parameter
  list there, until the owner decides otherwise.
- **LV2 automation is block-rate** unless the host splits blocks.
- **The ISO grows.** By the bundles (small), by captures from N3, and by ORT
  and models once the first model lands (N5 or N6), all bounded by §4.6. ORT's installed size is not yet known (Unverified 3).
- **The mix assistant needs model operations that do not exist yet**: add
  an aux or bus, route, add a send, insert a processor with its state, set
  a parameter, and a compound operation that undoes as one. These are the
  DAW plan's mixer and track items. N5 cannot start before them, and it
  adds requirements to them: compound undo, and a plan-preview diff.
- **The capture library needs hardware and studio time.** Each capture
  costs a session with a real amp. That is the owner's cost, not an
  agent's (item 6).
- **Modelled noise is a new way to lose signal quality silently.** §3.3
  requires it to be visible and switchable, and each processor's editor
  must show it.
- **One more agent role** writing into the tree, so review capacity is
  shared further (ADR-0007 §8: throughput is capped by review).

## Unverified

Each item names the measurement that settles it. All go to the researcher
unless stated otherwise. Several are expected in the forthcoming
`brain/research/native-plugin-suite.md`.

1. **Licences.**
   - DPF's licence, as of the pinned commit, for the LV2 and VST3 parts.
     **Settled 2026-10-13**: ISC at `4238e1c`, LV2 headers ISC, VST3 via
     DPF's own ISC "travesty" headers; only those parts are vendored
     (`plugins/native/format/third_party/dpf/SONORA.md`).
   - Steinberg's VST3 SDK under MIT since 3.8, as reported 2025-10-29: read
     the text in the current distribution. **Settled 2026-10-13**: MIT
     confirmed from Steinberg's press release and the repository's
     `LICENSE.txt`; Debian's 3.7.10 headers are GPL-3 under the old dual
     licence; both GPL-3 compatible (`brain/research/plugin-host-vst3.md`).
   - ONNX Runtime's licence and its bundled third-party notices.
   - Whether the EBU test signals for Tech 3341 and 3342 may be
     redistributed, or must be fetched at test time.
2. **DPF's VST3 against Steinberg's `validator`.** It is reported as an
   independent implementation. Its conformance today is unknown. This is
   settled by the N1 checkpoint (§1.2), run by the dsp-engineer.
   **Measured 2026-10-13** (`brain/research/native-format-n1.md`): DPF at
   `4238e1c` passed 46 of validator 3.8.1's 47 tests; "Parameter Bypass
   persistence" failed on a DPF bug in UI-less plugins. With a local
   patch, 47 of 47 for all six bundles. The §1.2 fallback (the MIT SDK)
   was not taken: the coordinator accepted the patch on 2026-10-13
   (Amendment 2 item 1), on condition that it does not race, which the
   TSan job checks against every bundle. **Settled.**
3. **ONNX Runtime on Debian trixie.** Whether it is packaged, at which
   version, and its installed size. If it is not packaged, the build cost
   of vendoring it. Also its Windows availability under MSVC, for
   ADR-0008.
4. **The CPU budget table (§3.5)** comes from arithmetic, not from a
   measurement on our hardware. Measure an N0 EQ, compressor and limiter on
   the owner's laptop, and on the target machine when it exists. Then
   adjust the classes in an amendment. The dsp-engineer measures, and the
   figures go to `brain/research/`.
5. **Allocation-free, bounded inference.** Whether ONNX Runtime's `Run()`
   with a pre-allocated arena and a single intra-op thread performs no
   allocation and no locking after warm-up. Check under RTSan, with p99.9
   and max per call for a small model (for example a pitch tracker) at 128
   frames. Until this is measured, per-block inference through a general runtime is forbidden
   (§4.2).
6. **Block-rate LV2 automation with smoothing is inaudible at quantum 128.**
   Settle it with a listening test plus a measurement of step energy on
   fast automation ramps. If it fails, the host splits blocks at automation
   points (M9).
7. **Two serial host round trips per block**, for track inserts and then
   the master chain, on bare metal at quantum 128. This feeds §1.4. It
   rides with the existing "ADR-0007 IPC figures on the RT kernel" item.
8. **SPICE references.** Whether published schematics for the original opto
   and FET units, the tube stage and a tape machine's electronics simulate
   in ngspice with the published parts. The opto cell's light-dependent
   resistor needs a behavioural model taken from the literature, and its
   fidelity is the weak point. The tape model's magnetic hysteresis comes
   from the literature (Jiles–Atherton family), not from a schematic.
9. **Class E for NAM-class captures.** The per-block cost, p99.9 and max at
   quantum 128, of the common capture architectures (the WaveNet sizes from
   standard down to the smallest, and the LSTM) on the owner's laptop, plus
   a cabinet IR of 200 ms by partitioned convolution. This decides which
   architecture we capture at, and it must be measured **before the first
   capture session**. The dsp-engineer measures it, with our kernels and
   with the reference implementation for comparison.
10. **Source separation weights.** The widely used open separation models
    publish their code under permissive licences. Their weights are
    commonly trained on research datasets (MUSDB18 family) whose terms
    restrict commercial use. Which separation model, if any, has weights we
    may ship under §4.6? The researcher answers this before N5 needs a
    full-mix reference. Without one, N5 accepts only a stem or solo
    reference, and says so in the UI.
11. **Reverb and delay estimation from a reference.** How accurately decay
    time, pre-delay and wet level can be estimated from a separated
    (imperfect) vocal or instrument. Measure on our own multitracks, mixed
    with known settings, reporting error against the known values. The
    published blind reverberation-time estimation methods are the starting
    point.

## Alternatives rejected

**Internal-only processors in the engine's graph.**
- What it offers: no IPC hop, no format adapters, the lowest latency for the
  master chain.
- Why it is rejected:
  - Every processor would run in the process that holds the take, and every
    one would be in the RT auditor's scope.
  - It needs a second processing path beside the plugin path, with its own
    state, presets, automation and PDC reporting.
  - It gives nothing to a Windows Sonora that hosts VST3, or to any other
    host.
- §1.4 keeps the useful part: a gated promotion of a processor that has
  been measured.

**Format-native code with no shared interface** (write each processor
directly against LV2, or directly against VST3).
- What it offers: one fewer layer.
- Why it is rejected:
  - Every processor would carry format code.
  - The second format would cost one port per processor.
  - Engine promotion (§1.4) would mean un-wrapping each one.

**JUCE as the framework.**
- What it offers: mature, with LV2, VST3 and CLAP, and its own GUI.
- Why it is rejected:
  - Its GUI is a second toolkit beside Qt, with its own look.
  - Its licence is GPL or commercial. That would decide "For the owner"
    item 1 by default.
  - It is a large body of code inside every bundle.
- DPF behind our own interface costs less, and can be replaced.

**Editors inside the plugin binaries (Qt in the bundle, or DPF's DGL).**
- What it offers: our plugins would have their own editors in any host.
- Why it is rejected:
  - Qt inside a plugin puts a second Qt into the host process, beside
    third-party plugins that may carry their own. A version clash is a
    crash.
  - DGL is a second visual language.
  - Both route our editors through Xwayland under ADR-0010.
- Kept as an option for the owner (item 3), not as the default.

**Inference in the plugin host, as the brief allowed.**
- What it offers: one process fewer, and analysis next to the live signal.
- Why it is rejected:
  - The host runs the RT plugin graph. An inference thread pool competes
    for its cores, and a runtime crash takes every plugin in that host
    with it.
  - Analysis on files gives the same results for every feature the owner
    named.

**ML pitch correction per block** (a neural pitch tracker or shifter in
`process`).
- What it offers: possibly better tracking on difficult material.
- Why it is rejected for now:
  - Nothing shows that inference can run allocation-free with a bounded
    worst case (Unverified 5).
  - Classical detection and shifting are published, measurable, and cheap.
- Revisit when Unverified 5 is measured.

**Amp captures through ONNX Runtime in the host.**
- What it offers: one inference path for everything, and any architecture
  for free.
- Why it is rejected:
  - Its allocation and threading behaviour per call is unmeasured
    (Unverified 5), and it would sit beside the RT plugin graph.
  - NAM-class architectures are few and small. Kernels for them are a
    bounded piece of code we can audit.

**Linking the NAM reference implementation into the amp bundle.**
- What it offers: proven code.
- Why it is rejected: it brings Eigen (MPL-2.0) into the shipped bundle,
  which narrows "For the owner" item 1 before the owner rules. We use it as
  a test oracle instead.

**Physical models for amps too, instead of captures.**
- What it offers: consistency with §3.3, and every knob modelled.
- Why it is not the choice: the owner asked for capture-based amps, aimed
  at specific recorded tones, which a capture reproduces directly. Physical
  amp models remain possible later as their own processor. This ADR does
  not exclude them.

**An "auto-mix" processor that does the assistant's work inside one
plugin.**
- What it offers: one insert on the master, no session changes.
- Why it is rejected: it is exactly the hidden processing the owner ruled
  out. The operator could not see, edit or undo the decisions
  individually.

**Claude inside the plugins, or Claude as the only AI** (sending audio, or
calling the API from a processor).
- What it offers: Claude's knowledge applied directly, and less native
  analysis to build.
- Why it is rejected:
  - A network call has no place near `process`.
  - Sending audio off the machine is a privacy and licence problem for the
    operator's clients' recordings.
  - The product would stop working offline, which an appliance in a
    studio often is.
- §4.5 keeps the useful part: Claude reasons over features and proposes
  plans, and the native path stands on its own without it.

**Validating the analog models by null-testing against the vendor's
emulation the owner referenced.**
- What it offers: the fastest route to "sounds like the reference".
- Why it is rejected: it is black-box cloning, which the coordinator's
  constraint and ADR-0004 forbid, whatever the route.

## For the owner

The architect cannot settle these.

1. **The suite's licence.** Does it ship under GPL-3.0-or-later like the
   rest of the repository (the default), or does the owner want to keep a
   separate or commercial licence open? Until this is ruled, only
   permissive third-party code enters `common/dsp` and `plugins/native`.
   That keeps the option. Once copyleft code is in, the option is gone.
2. **Naming an original unit in marketing.** For example "modelled on the
   circuit of …", with a disclaimer. The default is no. Yes needs legal
   advice the agents cannot give.
3. **Are the plugins a product for other DAWs?** If yes, they need custom
   editors that run in foreign hosts. That reverses §2.6's "no editor in
   the binary" for those builds, and it is real work. The default is no:
   generic UI elsewhere.
4. **`plugins.stock` after the native suite meets §3.** Keep shipping the
   curated third-party set beside ours, or drop it.
5. **Model budget and data.** Confirm or change 25 MB per model and 100 MB
   in total. Decide whether to fund our own training recordings (performers
   with releases), which is the only data with no licence question at all.
6. **Hardware, and the money for it.**
   - Which amps and cabinets to own or rent for captures, and the studio
     time to capture them. The amp family has no content without this.
   - Whether to buy or borrow original or reissued opto and FET units for
     measurement. Without them, validation rests on SPICE and published
     measurements (§3.3).
   - Rental terms that forbid recording the unit are a blocker for that
     unit.
7. **Default state of modelled noise and hum.** On at the modelled level
   (more authentic) or off by default (cleaner sessions)? This ADR requires
   only that it is visible and switchable.
8. **The Claude layer (§4.5).** Confirm or reject the hybrid. If confirmed,
   rule on:
   - network access on the appliance;
   - API key entry and storage;
   - who pays, and the cost ceiling;
   - rate limits;
   - the privacy statement of exactly what leaves the machine.

   N7 does not start before these rulings. Nothing else in this ADR
   depends on them.

## Amendment 1, 2026-10-13: the coordinator's decisions on N0

Made by the coordinator on the review of PR #34 (`feature/dsp-core`, stage
N0). The measurements behind each are in
`brain/research/native-dsp-n0-measurements.md`.

1. **Edges.** `plugins/native/sdk → common/dsp` and
   `plugins/native/measure → common/json` are allowed. The reasons are in
   §2.2, "Amendment 1 to §2.2".
2. **EQ: two modes, the operator's choice, as Pro Tools offers a
   low-latency choice.**
   - **Mixing: "analog matched (2x)" is the default.** It meets §3.2's
     ±0.1 dB from 20 Hz to 20 kHz at 44.1 and 48 kHz, at a latency of 69
     samples at every rate, reported for PDC. No single second-order
     section meets ±0.1 dB for wide upper bells at 1x (measured: a
     least-squares-optimal biquad is 0.42 dB off for a 10 kHz, Q 1,
     +18 dB bell at 44.1 kHz), so the bar is met by running at twice the
     rate rather than by loosening it.
   - **Tracking: "zero latency" (1x).** Matched designs at the session
     rate, no added latency. It does not meet ±0.1 dB above about 2 kHz
     for wide bells, and it does not hide that: the harness reports its
     error against the analog prototype for every curve it measures
     (`plugins/native/eq/quality.json`, "reported, not a bar").
   - Phase holds §3.2's 5° for f0 up to 2 kHz and reaches 8.5° above. The
     EQ's `quality.json` states the bar that way. That loosening is
     recorded here for the architect; the coordinator has not ruled on it.
3. **Limiter: never above its ceiling.**
   - The look-ahead stage keeps its **0.05 dB target margin** under the
     ceiling.
   - **A mastering limiter must never exceed its ceiling.** After the
     look-ahead stage, a true-peak guard (`dsp::TruePeakGuard`) detects at
     8x, with a first stage built to be the reference meter's own filter,
     and also takes the BS.1770 4x meter's reading as a need, then applies
     a second look-ahead gain at the same target. A final clamp holds the
     sample peak and is asserted never to act. §3.2's "Ceiling" row is
     then met on the full-band case as well: zero overs on the BS.1770 4x
     meter and on the 16x reference, over the stress set and full-band
     white noise, at every rate, ceiling and release tested.
   - What it costs, measured: about 310 samples more latency at 48 kHz
     (the limiter reports 430 in all at its defaults), about 48 µs more
     per block (the limiter is 80 µs at p50, inside class C), and on
     tones 6 dB over the ceiling non-harmonic energy of -147 dBc or lower
     (bar -90 dBc).
4. **CPU measurement (§3.5): two deviations, accepted by the
   coordinator.**
   - **Not asserted in PR CI.** A shared runner says nothing about the
     reference machine. The §3.5 measurement, p99.9 and max per block over
     a 60 s render at q128, 48 kHz, stereo, single thread, is
     `tools/dsp-cpu.sh`, run by the nightly workflow and by hand on the
     reference machine; its reports are kept, and a class bar missed on a
     shared runner does not fail the build. ctest keeps the quick
     per-block check: denormals, silence against active, measured side by
     side.
   - **The class bar is asserted on the "code path" figure**: the
     per-block minimum over five identical renders, which removes the
     host's scheduling noise and leaves the cost of the code. The
     single-run p99.9 and max are reported beside it. Inside Docker
     Desktop the single-run max misses class A for every processor (a
     2.4 µs delay block measured 137 µs); only a bare-metal RT run says
     what the appliance sees (Unverified 4 stays open).
5. **CI cost.** The slow DSP tests (`dsp_limiter`, `dsp_loudness`,
   `native_limiter`, ctest label `slow`) run one rate, one ceiling and one
   release in PR CI and under the sanitizers (`SONORA_DSP_QUICK=1`), and in
   full in the nightly workflow, in Release and under ASan and UBSan. TSan
   leaves out the single-threaded `dsp_*` and `native_*` tests.

## Amendment 2, 2026-10-13: the N1 format stage with DPF

Made on the review of PR #40 (`feature/dsp-n1`, stage N1). The
measurements are in `brain/research/native-format-n1.md`, the DPF patches
in `plugins/native/format/third_party/dpf/SONORA.md`.

1. **§1.2: the local DPF patch is accepted instead of the MIT-SDK
   fallback (coordinator, 2026-10-13).** DPF at `4238e1c` failed one of
   validator 3.8.1's 47 tests ("Parameter Bypass persistence"): in a
   plugin without a UI one object is both component and controller, and
   DPF dropped a value the host set on the controller. §1.2 said a
   validator failure moves VST3 to the MIT SDK. The failure was one DPF
   bug with a small fix, so the coordinator accepts the local patch
   (SONORA.md, patch 1), and the VST3 adapter stays on DPF. With the
   patch, 47 of 47 pass for all six bundles. Conditions:
   - The patch must not race. A host may set a value from its main thread
     while `process()` runs, so in our build neither thread uses DPF's
     plain parameter cache for input parameters: both read and write the
     plugin's own value, which the Bridge keeps in atomics. The TSan job
     runs that case against every bundle (`native_format_threads_vst3_*`).
   - The fallback is not withdrawn. If a later validator or DPF update
     fails in a way a small local patch does not fix, §1.2 applies as
     written: the VST3 adapter moves to the MIT SDK, behind the same
     `Bridge` (`plugins/native/format/include/sonora/native/format/bridge.h`).
   - Every local patch is listed in SONORA.md and offered upstream.
2. **§2.4, "Automation": with DPF, VST3 automation is block-accurate, not
   sample-accurate.** §2.4 said the VST3 adapter passes VST3's sample
   offsets through. DPF's VST3 wrapper does not (`DistrhoPluginVST3.cpp`,
   `process()`, the two loops over `input_params` either side of `run()`):
   - a queue's first point is applied before `run()`, and only when its
     offset is 0;
   - the queue's last point is applied after `run()`;
   - so a change at an offset other than 0 takes effect in the **next**
     block, one block late, and every point between the first and the
     last is dropped.

   At quantum 128 that is 2.67 ms late at 48 kHz, then smoothed: the
   block-rate delivery §2.4 already accepts for LV2, accepted for VST3 on
   the same terms. Sonora loads the LV2 build (§1.1), so this touches
   other hosts and a future Windows Sonora only. Sample-accurate VST3
   needs a DPF change or the MIT-SDK adapter. Either is a later amendment
   and no processor change, because `Processor` already takes timestamped
   events.
