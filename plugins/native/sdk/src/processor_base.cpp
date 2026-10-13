// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/native/processor_base.h"

#include <cmath>

#include "sonora/dsp/denormals.h"

namespace sonora::native {

namespace {
constexpr double kBypassFadeSeconds = 0.010;
constexpr double kDeclickSeconds = 0.003;  // each way

// Input beyond +120 dBFS is a fault upstream (a blown-up filter, a buffer
// of garbage), not a signal: no converter or mix bus produces it. It is
// treated as NaN is, replaced by silence, because a finite 1e30 does as
// much harm: a limiter sits 600 dB down for its release, a resonant EQ
// rings at 1e30 for seconds, a delay repeats it (the "1e30 in" check).
constexpr float kInputLimit = 1.0e6f;

// False for NaN, the infinities and anything past the limit.
inline bool valid_sample(float x) noexcept { return std::fabs(x) <= kInputLimit; }
}  // namespace

void ParamSmoother::configure(const ParamInfo& p, double sample_rate) noexcept {
    log_ = p.taper == Taper::Log && p.min > 0.0f;
    const double n = std::round(p.smoothing_ms * 1e-3 * sample_rate);
    s_.set_ramp_samples(static_cast<std::uint32_t>(n < 0.0 ? 0.0 : n));
}

ProcessorBase::ProcessorBase(const Descriptor& d) : desc_(d) {
    values_.resize(d.param_count);
    smoothers_.resize(d.param_count);
    pending_.resize(d.param_count);
    has_pending_.resize(d.param_count);
    for (std::uint32_t i = 0; i < d.param_count; ++i) {
        values_[i] = d.params[i].def;
        smoothers_[i].reset(values_[i]);
    }
}

bool ProcessorBase::prepare(double sample_rate, std::uint32_t max_frames, std::uint32_t channels) {
    if (!(sample_rate > 0.0) || max_frames == 0 || channels < desc_.min_channels ||
        channels > desc_.max_channels) {
        return false;
    }
    rate_ = sample_rate;
    max_frames_ = max_frames;
    channels_ = channels;
    for (std::uint32_t i = 0; i < desc_.param_count; ++i) {
        // Structural values queued for a declick that never finished.
        if (has_pending_[i]) values_[i] = pending_[i];
        has_pending_[i] = 0;
        smoothers_[i].configure(desc_.params[i], rate_);
        smoothers_[i].reset(values_[i]);
    }
    declick_ = Declick::Idle;
    declick_len_ = static_cast<std::uint32_t>(std::lround(kDeclickSeconds * rate_));
    if (declick_len_ == 0) declick_len_ = 1;

    const std::size_t n = static_cast<std::size_t>(channels) * max_frames;
    in_buf_.assign(n, 0.0f);
    wet_buf_.assign(n, 0.0f);
    sc_buf_.assign(n, 0.0f);
    in_ptr_.assign(channels, nullptr);
    sc_ptr_.assign(channels, nullptr);
    wet_ptr_.assign(channels, nullptr);

    latency_ = 0;
    if (!on_prepare()) return false;
    for (std::uint32_t i = 0; i < desc_.param_count; ++i) on_param(i, true);

    dry_line_.assign(channels, std::vector<float>(latency_ + 1, 0.0f));
    dry_pos_ = 0;
    bypass_mix_.set_ramp_samples(static_cast<std::uint32_t>(std::lround(kBypassFadeSeconds * rate_)));
    bypass_mix_.reset(values_[0] >= 0.5f ? 1.0 : 0.0);
    reset();
    return true;
}

void ProcessorBase::reset() noexcept {
    for (auto& d : dry_line_) {
        for (float& v : d) v = 0.0f;
    }
    dry_pos_ = 0;
    on_reset();
}

float ProcessorBase::param(std::uint32_t index) const noexcept {
    if (index >= desc_.param_count) return 0.0f;
    return has_pending_[index] ? pending_[index] : values_[index];
}

bool ProcessorBase::set_param(std::uint32_t index, float value) noexcept {
    if (index >= desc_.param_count) return false;
    apply_value(index, sanitize(desc_.params[index], value), true);
    return true;
}

void ProcessorBase::apply_value(std::uint32_t index, float v, bool jump) noexcept {
    has_pending_[index] = 0;
    values_[index] = v;
    if (index == 0) {
        if (jump) bypass_mix_.reset(v >= 0.5f ? 1.0 : 0.0);
        else bypass_mix_.set_target(v >= 0.5f ? 1.0 : 0.0);
        return;
    }
    if (jump) smoothers_[index].reset(v);
    else smoothers_[index].set_target(v);
    // A reconfiguring value waits for prepare(); the processor reads it
    // there, so the latency it reports always matches what it does.
    if (!desc_.params[index].reconfigures) on_param(index, jump);
}

void ProcessorBase::apply_event(const ParamEvent& e) noexcept {
    if (e.index >= desc_.param_count) return;
    const ParamInfo& p = desc_.params[e.index];
    const float v = sanitize(p, e.value);
    if (p.declick && !p.reconfigures) {
        if (v == param(e.index)) return;
        pending_[e.index] = v;
        has_pending_[e.index] = 1;
        if (declick_ == Declick::Idle) {
            declick_ = Declick::Out;
            declick_pos_ = 0;
        } else if (declick_ == Declick::In) {
            // Turn round from wherever the fade-in has got to.
            declick_ = Declick::Out;
            declick_pos_ = declick_len_ - declick_pos_;
        }
        return;
    }
    apply_value(e.index, v, false);
}

void ProcessorBase::render_chunk(std::uint32_t offset, std::uint32_t frames) noexcept {
    // Split at the bottom of a declick, then render.
    std::uint32_t pos = 0;
    while (pos < frames) {
        std::uint32_t end = frames;
        if (declick_ == Declick::Out) {
            const std::uint32_t left = declick_len_ - declick_pos_;
            if (left == 0) {
                // At silence: switch the structure.
                for (std::uint32_t i = 0; i < desc_.param_count; ++i) {
                    if (has_pending_[i]) apply_value(i, pending_[i], true);
                }
                declick_ = Declick::In;
                declick_pos_ = 0;
                continue;
            }
            if (pos + left < end) end = pos + left;
        }
        const std::uint32_t n = end - pos;
        for (std::uint32_t c = 0; c < channels_; ++c) {
            in_ptr_[c] = &in_buf_[static_cast<std::size_t>(c) * max_frames_ + offset + pos];
            sc_ptr_[c] = &sc_buf_[static_cast<std::size_t>(c) * max_frames_ + offset + pos];
            wet_ptr_[c] = &wet_buf_[static_cast<std::size_t>(c) * max_frames_ + offset + pos];
        }
        render(in_ptr_.data(), wet_ptr_.data(), sc_ptr_.data(), channels_, n);
        if (declick_ != Declick::Idle) {
            for (std::uint32_t i = 0; i < n; ++i) {
                double g = static_cast<double>(declick_pos_ + i) / declick_len_;
                if (declick_ == Declick::Out) g = 1.0 - g;
                // Raised cosine: no corner at either end of the fade.
                g = 0.5 - 0.5 * std::cos(3.14159265358979323846 * (g < 0.0 ? 0.0 : (g > 1.0 ? 1.0 : g)));
                for (std::uint32_t c = 0; c < channels_; ++c) wet_ptr_[c][i] *= static_cast<float>(g);
            }
            declick_pos_ += n;
            if (declick_ == Declick::In && declick_pos_ >= declick_len_) declick_ = Declick::Idle;
        }
        pos = end;
    }
}

void ProcessorBase::process(const AudioBuffers& io, const ParamEvent* events,
                            std::uint32_t event_count, const Transport& transport) noexcept {
    const dsp::DenormalGuard ftz;
    transport_ = transport;
    if (io.channels != channels_ || channels_ == 0) {
        for (std::uint32_t c = 0; c < io.channels; ++c) {
            for (std::uint32_t i = 0; i < io.frames; ++i) io.out[c][i] = 0.0f;
        }
        return;
    }
    std::uint32_t ev = 0;
    std::uint32_t done = 0;
    while (done < io.frames) {
        std::uint32_t chunk = io.frames - done;
        if (chunk > max_frames_) chunk = max_frames_;

        // Sanitised copies: in-place hosts, and no NaN reaches a filter.
        for (std::uint32_t c = 0; c < channels_; ++c) {
            float* dst = &in_buf_[static_cast<std::size_t>(c) * max_frames_];
            const float* src = io.in[c] + done;
            for (std::uint32_t i = 0; i < chunk; ++i) {
                const float x = src[i];
                dst[i] = valid_sample(x) ? x : 0.0f;
            }
            float* sdst = &sc_buf_[static_cast<std::size_t>(c) * max_frames_];
            if (io.sidechain != nullptr && io.sidechain_channels > 0) {
                const float* s = io.sidechain[c < io.sidechain_channels ? c : io.sidechain_channels - 1] + done;
                for (std::uint32_t i = 0; i < chunk; ++i) sdst[i] = valid_sample(s[i]) ? s[i] : 0.0f;
            } else {
                for (std::uint32_t i = 0; i < chunk; ++i) sdst[i] = dst[i];
            }
        }

        // Render and mix between events, so each change (bypass included)
        // lands on its sample. Bypass mixes the wet against the dry signal
        // delayed by the latency, so the timing never moves when it toggles.
        const std::uint32_t len = latency_ + 1;
        bool output_bad = false;
        std::uint32_t pos = 0;
        while (pos < chunk) {
            while (ev < event_count && events[ev].offset <= done + pos) apply_event(events[ev++]);
            std::uint32_t end = chunk;
            if (ev < event_count && events[ev].offset < done + chunk) end = events[ev].offset - done;
            render_chunk(pos, end - pos);
            for (std::uint32_t i = pos; i < end; ++i) {
                const double b = bypass_mix_.next();
                const std::uint32_t rd = (dry_pos_ + 1) % len;  // latency_ samples ago
                for (std::uint32_t c = 0; c < channels_; ++c) {
                    const std::size_t k = static_cast<std::size_t>(c) * max_frames_ + i;
                    dry_line_[c][dry_pos_] = in_buf_[k];
                    const float dry = dry_line_[c][rd];
                    const float wet = wet_buf_[k];
                    const float y = b == 0.0 ? wet : (b == 1.0 ? dry : static_cast<float>(wet + b * (dry - wet)));
                    output_bad |= !std::isfinite(y);
                    io.out[c][done + i] = y;
                }
                dry_pos_ = rd;
            }
            pos = end;
        }
        if (output_bad) {
            // A state that has blown up does not recover on its own: clear
            // it, and send silence rather than a non-finite sample.
            for (std::uint32_t c = 0; c < channels_; ++c) {
                for (std::uint32_t i = 0; i < chunk; ++i) io.out[c][done + i] = 0.0f;
            }
            on_reset();
            ++nonfinite_blocks_;
        }
        done += chunk;
    }
    // Events past the block's end (a host error) still take effect.
    while (ev < event_count) apply_event(events[ev++]);
    if (bounded_) {
        ++bounded_blocks_;
        bounded_ = false;
    }
}

}  // namespace sonora::native
