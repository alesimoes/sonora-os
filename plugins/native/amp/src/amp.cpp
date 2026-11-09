// SPDX-License-Identifier: GPL-3.0-or-later
//
// The amp's audio thread: the input summed to mono under the input gain,
// the current capture's chain, the normalisation trim, the dry path delayed
// by the latency, and the hand-over between two slots (amp.h).
#include <algorithm>
#include <cmath>

#include "amp_impl.h"
#include "sonora/dsp/math.h"

namespace sonora::native {

namespace {

using namespace amp;

const char* const kOffOn[] = {"off", "on"};

const ParamInfo kParams[kParamCount] = {
    kBypassParam,
    {"input", "Input", Unit::Decibel, Taper::Linear, -24.0f, 24.0f, 0.0f, 50.0f, true, false, false, "%.1f dB", nullptr, 0},
    {"output", "Output", Unit::Decibel, Taper::Linear, -24.0f, 12.0f, 0.0f, 50.0f, true, false, false, "%.1f dB", nullptr, 0},
    {"mix", "Mix", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 100.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
    // A step of up to kMaxTrimDb: declicked by the base, not smoothed.
    {"normalise", "Normalise", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 1.0f, 0.0f, false, false, true, "%.0f", kOffOn, 2},
};

// State version 1: the first. The capture's path is the file slot's state.
const Descriptor kDesc = {"org.sonora.amp", "Sonora Amp", "Amp", 1, kParams, kParamCount, 1, 2, false};

}  // namespace

namespace amp {

AmpImpl::AmpImpl() : Amp(kDesc) {}

double AmpImpl::phase_gain(std::uint32_t i) const noexcept {
    switch (phase_) {
    case Phase::Out: return fade_[fpos_ + i];
    case Phase::Hold: return 0.0;
    case Phase::In: return fade_[fade_len_ - (fpos_ + i)];
    case Phase::Steady: break;
    }
    return 1.0;
}

void AmpImpl::advance_phase(std::uint32_t frames) noexcept {
    if (phase_ == Phase::Steady) return;
    fpos_ += frames;
    if (fpos_ < phase_len()) return;
    fpos_ = 0;
    if (phase_ == Phase::Out) {
        Slot* old = cur_;
        cur_ = incoming_;
        // The new network starts from its prewarmed state and the
        // resamplers from silence; the hold lets them settle unheard.
        if (cur_->chain) cur_->chain->reset();
        // Cleared before the old slot is released, so a loader that takes
        // it cannot meet a stale pending.
        pending_.store(nullptr, std::memory_order_relaxed);
        idle_.store(old, std::memory_order_release);
        phase_ = Phase::Hold;
    } else if (phase_ == Phase::Hold) {
        phase_ = Phase::In;
    } else {
        phase_ = Phase::Steady;
    }
}

void AmpImpl::on_reset() noexcept {
    if (cur_ != nullptr && cur_->chain) cur_->chain->reset();
    std::fill(dry_.begin(), dry_.end(), 0.0f);
    dry_pos_ = 0;
}

void AmpImpl::render(const float* const* in, float* const* out, const float* const*, std::uint32_t,
                     std::uint32_t frames) noexcept {
    const std::uint32_t chs = channels();
    const float inv_chs = 1.0f / static_cast<float>(chs);
    std::uint32_t done = 0;
    while (done < frames) {
        if (phase_ == Phase::Steady) {
            Slot* n = pending_.load(std::memory_order_acquire);
            if (n != nullptr) {
                incoming_ = n;
                phase_ = Phase::Out;
                fpos_ = 0;
            }
        }
        std::uint32_t seg = frames - done;
        if (phase_ != Phase::Steady) seg = std::min(seg, phase_len() - fpos_);

        ModelChain* chain = cur_->chain.get();
        if (chain != nullptr) {
            for (std::uint32_t i = 0; i < seg; ++i) {
                float s = 0.0f;
                for (std::uint32_t c = 0; c < chs; ++c) s += in[c][done + i];
                mono_[i] = static_cast<float>(s * inv_chs * dsp::db_to_gain(smoother(kInput).next()));
            }
            chain->process(mono_.data(), wet_.data(), seg);
        } else {
            // Keeps the input gain's ramp where a capture would see it.
            smoother(kInput).advance(seg);
        }
        const float trim = value(kNormalise) >= 0.5f ? cur_->trim : 1.0f;

        for (std::uint32_t i = 0; i < seg; ++i) {
            const double g = phase_gain(i);
            const double og = dsp::db_to_gain(smoother(kOutput).next());
            const double mx = smoother(kMix).next() * 0.01;
            const std::uint32_t k = done + i;
            const std::uint32_t read = dry_pos_ + 1 == dry_len_ ? 0 : dry_pos_ + 1;
            for (std::uint32_t c = 0; c < chs; ++c) {
                float* line = dry_.data() + static_cast<std::size_t>(c) * dry_len_;
                line[dry_pos_] = in[c][k];
                const double dry = line[read];
                // No capture: the wet path is the input, as late as a capture's.
                const double wet = chain != nullptr ? static_cast<double>(wet_[i]) * trim : dry;
                out[c][k] = static_cast<float>(og * ((1.0 - mx) * dry + mx * g * wet));
            }
            dry_pos_ = read;
        }
        done += seg;
        advance_phase(seg);
    }
}

}  // namespace amp

std::unique_ptr<Amp> make_amp() { return std::make_unique<amp::AmpImpl>(); }

}  // namespace sonora::native
