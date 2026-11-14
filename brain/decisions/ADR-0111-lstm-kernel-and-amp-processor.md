# ADR-0111 - The LSTM kernel, and the amp processor that runs either NAM architecture

- Status: Accepted
- Date: 2026-11-08
- Item: `ampcab.kernels`
- Relates to: ADR-0004 (clean room), ADR-0008 (portability), ADR-0011
  (native plugin suite: §3.2, §3.5 class E, §4.3, §4.6)

## Summary

`common/dsp/neural/lstm.h` is a fixed-architecture kernel for the NAM
trainer's LSTM, with the WaveNet's real-time contract, caps checked at load,
an exact weight count, the same rational tanh and SIMD table, and the same
1e-5 bar against the NAM reference. The amp processor (`plugins/native/ampcab`)
runs a `.nam` of either architecture behind one small `NeuralModel`
interface, times a block at `prepare`, and refuses, with a message, a capture
over the class-E budget.

## Context

ADR-0011 §4.3 names two architectures for NAM-class captures: the
WaveNet-style dilated stack and "the small LSTM". The WaveNet kernel is on
main (`common/dsp/neural/wavenet.h`, its AVX2 table from slice 2, #342). The
`.nam` reader (`plugins/native/ampcab/src/nam_file.cpp`) refuses LSTM files
"until the LSTM kernel lands". Many published community captures and the
trainer's lighter presets are LSTMs, so the amp family cannot load them.

The amp processor itself does not exist yet: there is a reader, a capture
card and a WaveNet, but nothing that picks the kernel, checks its cost
against class E, deals with the capture's sample rate or uses the
`metadata.loudness` the reader already parses.

## Decision

1. **The LSTM kernel is the NAM trainer's LSTM**, in
   `common/dsp/include/sonora/dsp/neural/lstm.h`, implemented by us in
   `common/dsp/src/neural/lstm.cpp`:
   - `LstmSpec { num_layers, input_size (always 1), hidden_size }`, the
     `.nam` config's three fields.
   - Per layer, in the file's order: `W [4H][in + H]` row-major (layer 0
     reads `in = 1`, the guitar sample; layer l > 0 reads layer l - 1's
     `h`), `bias [4H]` (the trainer's `bias_ih + bias_hh`, summed on
     export), initial `h [H]`, initial `c [H]`. Then the head `[1][H]` and
     its bias `[1]`. No head scale.
   - Gate order i, f, g, o, as PyTorch: `c = σ(f) c + σ(i) tanh(g)`,
     `h = σ(o) tanh(c)`, output `head_w · h_last + head_b`.
   - The WaveNet's real-time contract: `load()` and `prepare()` allocate;
     `reset()` and `process()` are `noexcept SONORA_RT` and do fixed
     arithmetic only, with nothing allocated, locked or thrown. `prepare`
     computes the reference's prewarm (`max(1, int(0.5 × rate))` samples of
     silence from the file's `h` and `c`) and `reset()` restores it with
     O(layers × H) stores. Non-finite input is read as silence and input is
     held to ±16, so the output stays finite.
2. **Caps, so load is bounded.** `lstm_check(spec)` returns `""` or one
   sentence. It holds 1 to `kLstmMaxLayers` (4) layers, `input_size` 1,
   `hidden_size` 1 to `kLstmMaxHidden` (128), and at most
   `kLstmMaxWeights` (131 072) weights in all. 4 × 64 (116 545 weights)
   and 1 × 128 (66 945) pass; 2 × 128 (198 785) is refused on the total.
   Each sample costs about one multiply-add per weight, so the total also
   bounds the work a block can ask for. Whether it fits on this machine is
   item 5's benchmark, not the cap.
3. **`lstm_weight_count(spec)` is the exact count a file must carry**:
   `Σ_l (4H(in_l + H) + 4H + 2H) + H + 1`. `load()` refuses any other
   count with both numbers. The contract test holds 1 × 16 = 1 201,
   1 × 24 = 2 569 and 2 × 8 = 905, worked out by hand.
4. **One arithmetic for both kernels.** The LSTM uses `vec_math.h`'s
   rational `tanh_approx` and `sigmoid_approx` and the run-time SIMD table
   (`detail::Kernels`, generic or AVX2 chosen by CPUID), with
   `allow_simd()` and `kernels_name()` as on the WaveNet. Every table gives
   the generic table's bits. The kernel is held to **1e-5** absolute
   against the NAM project's MIT reference (NeuralAmpModelerCore,
   `NAM/lstm.cpp`), the WaveNet's bar. The reference is a test oracle,
   never linked (ADR-0011 §4.3).
5. **The amp processor runs either architecture behind one interface.** In
   `plugins/native/ampcab` (the amp and cab family's directory; ADR-0011
   calls it "the amp processor"), a `NeuralModel` interface has
   `process(in, out, frames)`, `reset()` and `latency()`, which is always
   0 for both kernels. `WaveNet` and `Lstm` each sit behind it in a small
   adapter, chosen once from `NamModel`'s architecture. Dispatch is one
   virtual call per block, never per sample. At `prepare`, after sizing,
   the processor times a quantum-128 block of the harness signal through
   the model (p99.9 and max over a fixed number of blocks, the §3.5
   method). If the capture with its cabinet IR is over class E (250 µs
   p99.9, 500 µs max), the capture is **refused with a held message**
   naming the measured figure and the budget. It is never loaded at a
   lower quality, never run with fewer layers and never left to miss
   deadlines.
6. **Sample rate and level.**
   - The kernel always runs at the capture's rate (`NamModel::sample_rate`,
     48 kHz when the file says nothing). When the session runs at another
     rate, the processor builds its resampler at load, off the audio
     thread: the filter designed, the polyphase tables and buffers sized.
     `process` then runs it as fixed arithmetic. Its group delay is reported
     as the processor's latency, exactly, and a harness test holds measured
     against reported latency at every supported session rate.
   - Loudness normalisation uses `metadata.loudness` when the file has it
     (`NamModel::has_loudness`, `loudness_db`). The output is trimmed to
     the processor's reference level and shown on the faceplate. A file
     without it is left at unity, and the faceplate says so.

## Rationale

- One kernel per published architecture, with the format's own weight
  order, is what makes a `.nam` file load or fail in a way we can explain.
  The refusal sentence goes straight into the operator's notice.
- The LSTM is a recurrence: each sample's gates depend on the last `h`. It
  cannot use the WaveNet's block-wide products, so its per-sample work is a
  `4H × (in + H)` matrix-vector product. The SIMD table's `matmul_rows`
  with `n = 1` does that without a second set of hot loops to keep
  bit-equal.
- Caps on the weights, not only on the dimensions, keep `load()`'s copy and
  the worst-case work bounded whatever a file claims. The class-E timing is
  the real gate, because only this machine knows what it can afford.
- Timing at `prepare` instead of using a table of costs follows ADR-0011
  §3.5: a cost table is wrong on every machine but the one it was measured
  on.
- A virtual call per block costs nothing next to the model. Per sample, it
  would sit inside the recurrence.

## Consequences

- `nam_file.cpp` stops refusing LSTM files once the kernel lands. The
  reader then maps the config to `LstmSpec` and checks it with
  `lstm_check` and `lstm_weight_count`.
- `common/dsp` builds without the kernel: `lstm_spec.cpp` holds the spec
  functions, and `include(cmake/lstm_kernel.cmake OPTIONAL)` lets the
  kernel task add `lstm.cpp` and its tests without editing
  `common/dsp/CMakeLists.txt` again. Until then nothing may call `Lstm`'s
  members (the link would fail, which is the intent).
- The amp processor's latency is 0 at the capture's rate, and the
  resampler's group delay at any other rate. PDC follows it.
- A capture refused on class E on one machine may load on a faster one.
  The message gives the figure so the operator can tell why.
- `test_dsp_lstm_contract` (label `dsp`) holds items 2 and 3. The kernel
  task adds the equivalence, block-split, reset, non-finite, no-allocation
  and table-against-table tests on the WaveNet's model.

## Alternatives rejected

- **ONNX Runtime or libtorch in the plugin host.** Megabytes of runtime
  for a few thousand weights, a thread pool and an allocator no measurement
  has cleared on the audio thread, and libtorch's licence and size besides.
  ADR-0011 §4.3 already keeps general inference to `sonora-analyze`.
- **A generic graph interpreter** reading layers from the file. It would
  accept architectures nobody has validated against the reference, and it
  pays dispatch per operation. Two fixed kernels are less code than the
  interpreter and its tests.
- **Per-sample virtual dispatch** between the architectures. The LSTM is a
  per-sample recurrence, so the call would sit inside the hottest loop.
  Dispatch is per block.
- **Degrading an over-budget capture** (fewer layers, a lower rate, a
  smaller hidden size). It would sound unlike the capture without saying
  so. ADR-0011 §3.5 says refuse.

## Unverified

- The LSTM's class-E cost on a quiet reference machine: which sizes
  (1 × 16, 1 × 24, 2 × 8 and up) fit with a cabinet IR, at 48 kHz and at a
  resampled session rate. This is handed to the researcher with ADR-0011's
  Unverified 9. Until it is measured, the load-time timing is the only
  gate.
