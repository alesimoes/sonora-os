# Native plugin formats, stage N1: what was measured

- Date: 2026-10-13
- Author: dsp-engineer
- Scope: ADR-0011 §1.2, the N1 checkpoint. The six N0 processors (EQ,
  compressor, gate, limiter, delay, chorus) built as LV2 and VST3 bundles
  through DPF behind `sonora::native::Processor`, and checked four ways:
  lv2lint, Steinberg's validator, Sonora's own hosts, and the golden
  references rendered through the adapters.
- Where: the `sonora-plugins:trixie` container (`tools/build-plugins.sh`)
  on the owner's laptop under Docker Desktop. GCC 14, Release. These are
  conformance and equality results; nothing here is a timing figure.

## Results

| Check | Tool | Result |
|---|---|---|
| LV2 metadata and binary | lv2lint 0.16.2, ELF checks on | 6 of 6 bundles pass. Two warnings each, no author e-mail or homepage (we have neither yet) |
| VST3 conformance | validator from VST3 SDK 3.8.1_build_84 | 47 of 47 tests for each of the 6 bundles, after one DPF patch (below) |
| Through Sonora's LV2 host | `plugin_driver --scenario render` | 24 of 24 cases: max \|plugin − reference\| = 0 |
| Through Sonora's VST3 host | same | 24 of 24 cases: max \|plugin − reference\| = 0 |
| Latency reported through the host | same | Equal to the Processor's at every rate: EQ 69, limiter 420/430/550 (44.1/48/96 kHz), the others 0 |
| Bypass through the formats | same, `bypass-48000` cases | The input delayed by exactly the reported latency, both formats |

The 48 render cases are 6 processors × {golden at 44.1, 48 and 96 kHz,
bypass at 48 kHz} × {LV2, VST3}. The bar is ADR-0011 §3.4's 1e-6; every
case is bit-exact.

**What "reference" means here.** `sonora-dsp-measure --host-fixture`
renders the N0 Processor offline with its suite's golden configuration,
under what the host imposes and the plugin cannot see past:

- the host's validation preroll (4 blocks of 128 frames of silence,
  `Chain::validate`);
- parameters arriving in the first block, and for VST3 as normalised
  floats through DPF's arithmetic;
- the VST3 host's transport (120 BPM; the delay follows it), and no
  transport from the LV2 host.

The same renderer compares its LV2 reference with the committed golden
WAV: 0 difference for the EQ, compressor, limiter and delay at every rate,
and for the gate at 44.1 and 48 kHz (2.2e-8 at 96 kHz). The chorus differs
(up to 0.96): its LFO runs through the 512 samples of preroll, so its phase
is elsewhere when the input starts. That is the host's preroll, not the
adapter; the render against the preroll-aware reference is exact.

## Findings

1. **DPF's VST3 failed one validator test, unpatched.** "Parameter Bypass
   persistence": in a plugin without a UI the component is also the
   controller, and DPF drops a value the host sets with
   `setParamNormalized()`, so `getParamNormalized()` never reflects it.
   46 of 47 passed. A 9-line patch makes it take the value the way DPF's
   separate-controller build does; 47 of 47 then pass. ADR-0011 §1.2 says a
   validator failure moves the VST3 adapter to the MIT SDK. Since the
   failure was one DPF bug with a small fix, the patch was taken instead;
   the coordinator confirms or overrules that. Two more DPF patches are
   metadata only (`pprops:notAutomatic` for lv2lint, and no third-party
   namespace prefix in our TTL). All three are listed in
   `plugins/native/format/third_party/dpf/SONORA.md`.
2. **DPF's VST3 is not sample-accurate.** It applies the first point of a
   parameter queue at offset 0 before `run()` and the last point after it,
   so a change lands at a block boundary. ADR-0011 §2.4 says the VST3
   adapter passes VST3's offsets through; with DPF it cannot. At quantum 128
   with the processors' smoothing this is the same block-rate delivery
   LV2 has. Sample accuracy for VST3 needs either a DPF change or the MIT
   SDK adapter.
3. **Parameters that reconfigure** (the EQ's mode, the limiter's
   look-ahead) take effect at the plugin's next activation, because their
   latency may only change off the audio thread (§1.5). A host that never
   deactivates the plugin (most DAWs while playing) keeps the old
   configuration, and the reported latency keeps matching the audio. Sonora's
   host has to re-activate the instance and re-plan PDC when one changes
   (M9).
4. **Values before the first block are the initial state.** LV2 delivers
   control ports only inside `run()`, so without care every parameter of a
   freshly loaded instance would ramp from its default over its smoothing
   time. The adapter (`format/bridge.h`) applies whatever arrives before the
   first block as jumps, and smooths everything after.
5. **VST3 class IDs** are DPF's four words, "DPF " "clas" <plugin> "Snra",
   stored in host byte order, so each word reads reversed on x86_64:
   `vst3:2046504473616C6371654E5361726E53` is the EQ.
   They are fixed by the `UID` in each processor's CMakeLists and must
   never change.

## Not done in this stage

- Mono bundles. Every bundle is 2 in, 2 out; the host refuses a stereo
  plugin on a mono chain.
- A side-chain input for the compressor and gate. The host routes none.
- The other N1 items: descriptor-driven Qt panels (the descriptor JSON is
  now in every bundle), the telemetry region, engine meters, and the
  bundles in the ISO (install rules exist: `lib/lv2`, `lib/vst3`).
- CPU: the adapter adds an atomic exchange per block and a scan when a
  parameter changed. Not measured separately from the N0 figures.
