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
   the coordinator accepted it on 2026-10-13 (ADR-0011 Amendment 2). On the
   review of PR #40 the patch was narrowed so that it does not race with
   `process()` (SONORA.md, patch 1). Three more DPF patches are metadata
   only (`pprops:notAutomatic` for lv2lint, and no `dg:` or `mod:`
   namespace in our TTL), and two harden the audio thread (a VST3 block
   longer than promised is refused, and the bundles print nothing). All
   six are listed in `plugins/native/format/third_party/dpf/SONORA.md`.
2. **DPF's VST3 is not sample-accurate.** It applies the first point of a
   parameter queue at offset 0 before `run()` and the last point after it,
   so a change lands at a block boundary. ADR-0011 §2.4 says the VST3
   adapter passes VST3's offsets through; with DPF it cannot. At quantum 128
   with the processors' smoothing this is the same block-rate delivery
   LV2 has. Sample accuracy for VST3 needs either a DPF change or the MIT
   SDK adapter. Accepted as block-rate delivery in ADR-0011 Amendment 2.
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

- (Done in the second N1 PR, below.) Mono bundles; a side-chain input for
  the compressor and gate.
- The other N1 items: descriptor-driven Qt panels (the descriptor JSON is
  now in every bundle), the telemetry region, engine meters, and the
  bundles in the ISO (install rules exist: `lib/lv2`, `lib/vst3`).
- CPU: the adapter adds an atomic exchange per block and a scan when a
  parameter changed. Not measured separately from the N0 figures.

## Second N1 PR: mono, key input, automation, re-activation

- Date: 2026-10-13. Branch `feature/dsp-n1-io`. Same container, GCC 14,
  Release; conformance and equality results, no timing figure.

**What was added.**

- A mono bundle beside every stereo one: `sonora-<name>-mono.lv2` and
  `.vst3`, 1 in and 1 out, URI `urn:sonora:native:<name>-mono`, VST3
  plugin word `SM..` (the stereo ones keep `SN..`, their URIs and class
  IDs unchanged). DPF fixes the port count at compile time, so a layout is
  a bundle. ADR-0011 asks for mono in, stereo out only of the class-E amp
  (§3.5), so no N0 processor has a mono-to-stereo build.
- A key input on the compressor and gate, in both layouts, as many
  channels as the main input: LV2 ports `lv2_key_in_1` (and `_2`) with
  `lv2:isSideChain`, after the main inputs; in VST3 an aux input bus,
  inactive until a host activates it. Adding the ports moved
  the stereo compressor's and gate's LV2 control port indices up by two;
  the symbols are unchanged and nothing saved refers to the indices yet.
  An unconnected key port (a null from LV2) or an inactive key bus
  (VST3, through DPF patch 7) is no key; a connected or active silent one
  is a silent key (`format/bridge.h`, ADR-0011 Amendment 2 item 3).
- The plugin host routes a key: `--sidechain N` takes the last N of the
  region's channels as the key; LV2 side-chain ports and the first VST3
  aux input bus read it, and without one they read the plugin's own input
  (LV2) or stay inactive (VST3), which both key from the input. `--reactivate` re-activates every plugin after the
  first validation, so a latency-changing value given at load takes
  effect. `--probe` now lists each writable control's id, which the
  driver uses to put a named change on the parameter ring.

**Results.**

| Check | Result |
|---|---|
| lv2lint 0.16.2 | 12 of 12 bundles pass, the same two warnings each |
| validator 3.8.1 | 47 of 47 for each of the 12, the aux bus included |
| Renders through the host, LV2 and VST3 | 84 of 84 bit-exact against the N0 reference: per processor golden x3, bypass, mono and automation, plus key and key-mono for the compressor and gate, plus reconfigure for the EQ and limiter |
| Latency after re-activation | EQ mode 0: 0 (69 before); limiter look-ahead 3 ms at 48 kHz: 502 (430 before), equal to the Processor's |
| Mutation check | Each new case fails when its feature is removed from its args: no key (max difference 0.46 compressor, 0.50 gate), no change (0.22 to 0.77), no re-activation (latency 69 against 0, 430 against 502) |
| VST3 timing, `native_format_late_vst3_*` | A change at offset 64 of block k is not heard in block k and is heard from sample 0 of block k+1, identical to the same change sent at offset 0 of block k+1; all 12 bundles |
| TSan, ASan and UBSan | `native_format_*`, 61 tests: clean |

**Delivery, as measured.** The host puts a ring change into the block it
is applied to at offset 0. LV2 delivers it in that block's `run()`; DPF's
VST3 applies an offset-0 point before `run()`. So through Sonora's host
both formats smooth the change from the first sample of the block it was
sent for, and the reference with the event at that block's first frame is
matched exactly. A nonzero offset only comes from another VST3 host, and
there it lands a block late (Amendment 2), which the `late` scenario pins.

**Review fixes (PR #43, RT audit and code review).** Same container, GCC
14, Release unless stated; equality results, no timing figure.

| Finding | Fix | Test, before the fix, after it |
|---|---|---|
| High: External key on, nothing routed, keyed from silence (compressor never acted, gate stayed shut) | DPF patch 7: an inactive or missing VST3 side-chain input is null; Sonora's host connects an LV2 key port with no key to the plugin's own input (ADR-0011 Amendment 2 item 3) | `native_{lv2,vst3}_{compressor,gate}_nokey{,-mono}-48000`, 8 renders: failed (max difference 0.464 compressor, 0.496 gate), now bit-exact. The fixture fails unless a silent key moves the reference (0.464, 0.496). `native_format_nokey_vst3_*`, 4 bundles: 8163 to 16374 samples differed from the switch off, now 0; an active silent key still differs (0.113 compressor, 0.250 gate) |
| Medium: the chain's key buffers shared and unguarded | Copied again before every plugin; canaries behind them, checked like the stages; at least one key buffer per signal channel, so each replica of a mono plugin has its own | `plugin_host_key`: two plugins that overwrite their key input, the second's output 8192 of 8192 samples wrong, now 0; with no key the key input reads the input (8192 wrong, now 0). `plugin_host_refusals`: a plugin that writes 64 samples past its key input ran, now refused (WroteOutsideBuffers), with and without a key routed |
| Low: VST3 re-activation ignored a failed `setActive(true)` | `Processor::reactivate()` returns bool; `serve` refuses the chain (InstantiateFailed) | `plugin_host_vst3_refusals`, a test class that activates once: was Ready, now Refused |
| Low: DPF's channel assertion printing from `process()` | None needed: `SONORA_DPF_QUIET` already silences it | `native_format_wide_vst3_*`, 12 bundles, one channel too many with fds 1 and 2 captured: 0 bytes. The same bundles built with `-USONORA_DPF_QUIET`: 1300 bytes, and the test fails |

Totals: `tools/build-plugins.sh` 296 of 296 (lv2lint 12 of 12, validator
12 of 12 bundles, 92 renders); `tools/build-engine.sh` 137 of 137;
`native_format_*` under TSan and under ASan with UBSan, 77 of 77 each.

**Not done.** Re-activation while running, for a latency-changing value
moved during playback: the host pauses nothing today, and pausing the
processing thread for the main thread's `setActive` is the M9 control
channel's job (`planning/BACKLOG.md`). How the engine asks for a key is
likewise M9's (`--sidechain` anticipates it without changing the header).
