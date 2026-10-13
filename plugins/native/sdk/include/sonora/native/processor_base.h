// SPDX-License-Identifier: GPL-3.0-or-later
//
// The behaviour every native processor shares, written once:
// - parameter values, smoothing (ADR-0011 §2.4) and sample-accurate events,
//   by splitting the block at each event's offset;
// - latency-compensated bypass with a crossfade (§1.5);
// - declicked structural switches (ParamInfo::declick);
// - flush-to-zero for the whole of process() (§3.2, "Denormals");
// - non-finite input, and input beyond +120 dBFS, replaced by silence, and
//   a non-finite output resets the processor and outputs silence for that
//   block, so output is always finite and recovers on the next valid block
//   (§3.2, "Non-finite input");
// - in-place hosts (out aliasing in), via internal buffers sized in
//   prepare().
//
// A processor implements on_prepare(), on_reset(), on_param() and render().
#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

#include "sonora/dsp/smoothing.h"
#include "sonora/native/processor.h"

namespace sonora::native {

// A parameter's smoother, linear in the plain or the log domain by taper.
class ParamSmoother {
public:
    void configure(const ParamInfo& p, double sample_rate) noexcept;
    void reset(double value) noexcept { s_.reset(map(value)); }
    void set_target(double value) noexcept SONORA_RT { s_.set_target(map(value)); }
    double next() noexcept SONORA_RT { return unmap(s_.next()); }
    double advance(std::uint32_t n) noexcept SONORA_RT { return unmap(s_.advance(n)); }
    double current() const noexcept { return unmap(s_.current()); }
    double target() const noexcept { return unmap(s_.target()); }
    bool moving() const noexcept { return s_.moving(); }

private:
    double map(double v) const noexcept { return log_ ? std::log(v) : v; }
    double unmap(double v) const noexcept { return log_ ? std::exp(v) : v; }
    dsp::LinearSmoother s_;
    bool log_ = false;
};

class ProcessorBase : public Processor {
public:
    explicit ProcessorBase(const Descriptor& d);

    const Descriptor& descriptor() const noexcept override { return desc_; }
    bool prepare(double sample_rate, std::uint32_t max_frames,
                 std::uint32_t channels) final;
    void reset() noexcept final;
    std::uint32_t latency() const noexcept final { return latency_; }
    void process(const AudioBuffers& io, const ParamEvent* events, std::uint32_t event_count,
                 const Transport& transport) noexcept SONORA_RT final;
    float param(std::uint32_t index) const noexcept final;
    bool set_param(std::uint32_t index, float value) noexcept SONORA_RT final;

    // How many blocks were silenced because the output went non-finite.
    std::uint64_t nonfinite_blocks() const noexcept { return nonfinite_blocks_; }
    // How many blocks had an input the processor could not follow and
    // bounded instead (the delay's host tempo below its 20 BPM floor).
    // Never a silent clamp: the M9 telemetry region publishes this to the
    // editor, and the measurement harness asserts it.
    std::uint64_t bounded_blocks() const noexcept { return bounded_blocks_; }

protected:
    // Allocate and configure; read `reconfigures` parameters with value().
    // Call set_latency() here. Returns false for an unsupported layout.
    virtual bool on_prepare() = 0;
    virtual void on_reset() noexcept = 0;
    // A parameter's target changed. `jump` is true outside playback
    // (prepare, set_param): the smoother is already at the new value.
    virtual void on_param(std::uint32_t index, bool jump) noexcept SONORA_RT {
        (void)index;
        (void)jump;
    }
    // Process `frames` samples of every channel. in and out never alias.
    virtual void render(const float* const* in, float* const* out, const float* const* sidechain,
                        std::uint32_t sidechain_channels, std::uint32_t frames) noexcept SONORA_RT = 0;

    double sample_rate() const noexcept { return rate_; }
    std::uint32_t channels() const noexcept { return channels_; }
    std::uint32_t max_frames() const noexcept { return max_frames_; }
    float value(std::uint32_t index) const noexcept { return values_[index]; }
    ParamSmoother& smoother(std::uint32_t index) noexcept { return smoothers_[index]; }
    const Transport& transport() const noexcept { return transport_; }
    void set_latency(std::uint32_t samples) noexcept { latency_ = samples; }
    // Call from render() when an input had to be bounded; counted once per
    // block in bounded_blocks().
    void note_bounded() noexcept { bounded_ = true; }

private:
    void apply_event(const ParamEvent& e) noexcept;
    void apply_value(std::uint32_t index, float v, bool jump) noexcept;
    void render_chunk(std::uint32_t offset, std::uint32_t frames) noexcept;

    const Descriptor& desc_;
    double rate_ = 48000.0;
    std::uint32_t channels_ = 0;
    std::uint32_t max_frames_ = 0;
    std::uint32_t latency_ = 0;
    Transport transport_;
    std::vector<float> values_;
    std::vector<ParamSmoother> smoothers_;

    // Sanitised input, sidechain and wet output, channel-major.
    std::vector<float> in_buf_, sc_buf_, wet_buf_;
    std::vector<const float*> in_ptr_, sc_ptr_;
    std::vector<float*> wet_ptr_;

    // Bypass: the dry signal delayed by latency_, crossfaded with the wet.
    std::vector<std::vector<float>> dry_line_;
    std::uint32_t dry_pos_ = 0;
    dsp::LinearSmoother bypass_mix_;

    // Declick for structural switches: fade out, apply, fade in.
    enum class Declick : std::uint8_t { Idle, Out, In };
    Declick declick_ = Declick::Idle;
    std::uint32_t declick_len_ = 0;
    std::uint32_t declick_pos_ = 0;
    std::vector<float> pending_;
    std::vector<std::uint8_t> has_pending_;

    std::uint64_t nonfinite_blocks_ = 0;
    std::uint64_t bounded_blocks_ = 0;
    bool bounded_ = false;
};

}  // namespace sonora::native
