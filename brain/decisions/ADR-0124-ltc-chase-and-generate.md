# ADR-0124: LTC chase and generate

Status: accepted (architect, 2026-11-09)

## Summary
LTC (SMPTE 12M linear timecode, an 80-bit biphase-mark signal carried as audio) gets its own module `drivers/ltc`, separate from the MIDI surfaces. A pure codec (decoder fed samples, encoder filling samples) and a control-thread chase/generate driver sit behind two small interfaces, `LtcSampleSource` and `LtcSampleSink`. Wiring to the audio thread is a later item. No protocol, `contracts/` or format change.

## Context
MTC (ADR-0122) rides a `MidiPort`. LTC is audio: the decoder must see samples of one input channel, the generator must write samples to one output channel. Tapping a channel on the audio thread is real-time-path work with its own risk, and it needs a hardware answer (UMC1820 line-level LTC, jitter) we do not have.

## Decision
1. New module `drivers/ltc` (library `sonora_ltc`, tests labelled `drivers`). It does not depend on `drivers/surfaces`, so it does not wait for MTC's `Timecode` type; it has its own small `LtcTime` (h, m, s, frame, rate 24/25/29.97df/30; user bits ignored). Unifying the two types is a later refactor, once both are on main.
2. `LtcDecoder::feed(span<const float>)`: zero-crossing interval tracking with a self-adjusting bit clock, sync-word search (0x3FFD), emits `LtcTime` per good frame. Fixed state, no allocation after construction. A reversed sync word (tape running backwards) is reported as "not locked" in v1.
3. `LtcEncoder::render(LtcTime, span<float>)`: continues the biphase waveform across calls (phase kept), so block sizes are free. Allocation-free, so it may run on the audio thread when wired.
4. `LtcChase` / `LtcChaseDriver` (control thread): reads `LtcSampleSource` each tick and issues `api::EngineControl` locate/play/stop with the same policy as MTC: 2-frame lock window, stop after N frame periods without a good frame (a tick count, never wall clock), and **never locate while recording** (count drift instead).
5. `LtcGenerator` (control side): publishes the transport position to `LtcSampleSink`. Chase or generate is a constructor argument, validated at start-up and failing loudly; a rate mismatch with the session is refused.
6. Samples cross threads only through a `RingBuffer` in the later wiring item. Not here.

## Rationale
A pure codec is testable with synthetic waveforms and no device. A separate module keeps audio-tap work apart from the MIDI surface library and avoids sharing files with the MTC feature. Refusing locate while recording follows the rule that a take is never lost.

## Consequences
Two timecode types exist until unified. The audio-thread tap and the settings UI remain to do. Decoding of real, noisy, varispeed LTC is unmeasured.

## Alternatives rejected
- LTC inside `drivers/surfaces` beside MTC: wrong transport, and it would share files and CMake with MTC.
- Reuse MTC's `Timecode`: depends on an unmerged feature; stacked work is refused.
- Decode inside `Session::process()`: puts parsing on the RT path before any measurement says it is needed.
- libltc: a new dependency in the image for about 300 lines of code; revisit if our decoder proves weak.

## Unverified (hand to the researcher)
- LTC level and jitter from UMC1820 inputs and outputs; minimum usable amplitude.
- Varispeed range over which the decoder holds lock (assumption: +-10 %).
- Latency from the input tap to the control tick; default offset.
- Whether LTC out needs a hardware-clocked sample counter rather than the session's.
