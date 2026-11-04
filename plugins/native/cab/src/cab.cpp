// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/native/cab.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sonora/dsp/convolver.h"
#include "sonora/dsp/math.h"

namespace sonora::native {

namespace {

using namespace cab;

const ParamInfo kParams[kParamCount] = {
    kBypassParam,
    {"output", "Output", Unit::Decibel, Taper::Linear, -24.0f, 12.0f, 0.0f, 50.0f, true, false, false, "%.1f dB", nullptr, 0},
    {"mix", "Mix", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 100.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
};

const Descriptor kDesc = {"org.sonora.cab", "Sonora Cabinet", "Amp", 1, kParams, kParamCount, 1, 2, false};

// P = 128: a 128-tap direct head and one 256-point transform pair a block.
// The cost for a 0.3 s response is in quality.json's note.
constexpr std::uint32_t kPartition = 128;

double bessel_i0(double x) noexcept {
    double sum = 1.0, term = 1.0;
    const double q = 0.25 * x * x;
    for (int k = 1; k < 60; ++k) {
        term *= q / (static_cast<double>(k) * k);
        sum += term;
        if (term < 1e-17 * sum) break;
    }
    return sum;
}

// Band-limited resampling by a Kaiser-windowed sinc (beta 9, 32 zero
// crossings each side at the lower of the two rates' cutoff: about -90 dB
// stopband), J. O. Smith, "Digital Audio Resampling Home Page", CCRMA.
// Offline: this runs in load_ir(), never in process().
std::vector<double> resample(const float* x, std::size_t n, double from, double to) {
    std::vector<double> y;
    if (from == to) {
        y.assign(x, x + n);
        return y;
    }
    const double ratio = to / from;
    const std::size_t out_n =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(static_cast<double>(n) * ratio)));
    const double cut = std::min(1.0, ratio);
    constexpr double kHalf = 32.0, kBeta = 9.0;
    const double span = kHalf / cut;  // input samples each side
    const double i0b = bessel_i0(kBeta);
    y.assign(out_n, 0.0);
    for (std::size_t m = 0; m < out_n; ++m) {
        const double t = static_cast<double>(m) / ratio;
        const long j0 = std::max(0L, static_cast<long>(std::ceil(t - span)));
        const long j1 = std::min(static_cast<long>(n) - 1, static_cast<long>(std::floor(t + span)));
        double acc = 0.0;
        for (long j = j0; j <= j1; ++j) {
            const double d = (t - static_cast<double>(j)) * cut;
            const double w = d / kHalf;
            if (w <= -1.0 || w >= 1.0) continue;
            const double sinc = std::fabs(d) < 1e-12 ? 1.0 : std::sin(dsp::kPi * d) / (dsp::kPi * d);
            acc += static_cast<double>(x[j]) * cut * sinc * bessel_i0(kBeta * std::sqrt(1.0 - w * w)) / i0b;
        }
        y[m] = acc;
    }
    return y;
}

const FileSlotInfo kIrSlot = {"ir", "Impulse response", "wav"};

// A load that meets the previous one still waiting to be taken over (Busy)
// waits for the audio thread's next block: 40 x 5 ms. A host that restores
// state with no audio running gives up after that and the slot keeps its
// old response.
constexpr int kBusyTries = 40;
constexpr int kBusyWaitMs = 5;

class CabImpl final : public Cab, public FileSlots {
public:
    CabImpl() : Cab(kDesc) {}

    void set_ir_reader(IrReader reader) override { reader_ = reader; }
    FileSlots* files() noexcept override { return reader_ != nullptr ? this : nullptr; }
    const FileSlots* files() const noexcept override { return reader_ != nullptr ? this : nullptr; }

    std::uint32_t slot_count() const noexcept override { return 1; }
    const FileSlotInfo& slot(std::uint32_t) const noexcept override { return kIrSlot; }

    bool load(std::uint32_t index, const std::string& path, std::string& error) override {
        if (index != 0 || reader_ == nullptr) {
            error = "no such file slot";
            return false;
        }
        // One file load at a time: two control threads (a restore and a
        // chooser) would otherwise leave path_ naming the other's response.
        std::lock_guard<std::mutex> lock(file_mutex_);
        if (path.empty() && path_.empty()) return true;
        LoadResult r = LoadResult::Busy;
        for (int attempt = 0; attempt < kBusyTries && r == LoadResult::Busy; ++attempt) {
            if (attempt > 0) std::this_thread::sleep_for(std::chrono::milliseconds(kBusyWaitMs));
            r = path.empty() ? clear_ir() : reader_(*this, path, error);
        }
        if (r == LoadResult::Busy) {
            error = "the previous response is still being taken over; no audio is running to finish it";
            return false;
        }
        if (r == LoadResult::Invalid) {
            if (error.empty()) error = "not a usable impulse response";
            return false;
        }
        path_ = path;
        return true;
    }

    std::string path(std::uint32_t) const override {
        std::lock_guard<std::mutex> lock(file_mutex_);
        return path_;
    }

    LoadResult load_ir(const float* ir, std::size_t frames, double ir_rate) override {
        if (ir == nullptr || frames == 0 || !(ir_rate >= 1000.0 && ir_rate <= 768000.0)) return LoadResult::Invalid;
        for (std::size_t i = 0; i < frames; ++i) {
            if (!dsp::is_finite(ir[i]) || std::fabs(ir[i]) > 1e9f) return LoadResult::Invalid;
        }
        std::lock_guard<std::mutex> lock(load_mutex_);
        return load_locked(ir, frames, ir_rate);
    }

protected:
    // The response of a cab with nothing loaded: a unit impulse at the
    // session rate (resampling one would change its gain).
    LoadResult clear_ir() {
        std::lock_guard<std::mutex> lock(load_mutex_);
        if (!prepared_) {
            source_.clear();
            return LoadResult::Ok;
        }
        const float one = 1.0f;
        const LoadResult r = load_locked(&one, 1, sample_rate());
        // Not kept as the source: a later prepare() at another rate would
        // resample it, and a resampled impulse is not a unit one.
        if (r == LoadResult::Ok) source_.clear();
        return r;
    }

    LoadResult load_locked(const float* ir, std::size_t frames, double ir_rate) {
        if (!prepared_) {
            source_.assign(ir, ir + frames);
            source_rate_ = ir_rate;
            return LoadResult::Ok;
        }
        // Take the idle kernel first: if there is none the audio thread may
        // still be reading both, and the stored source must stay as it was.
        dsp::ConvolutionKernel* k = idle_.exchange(nullptr, std::memory_order_acquire);
        if (k == nullptr) return LoadResult::Busy;
        bool cut = false;
        if (!build(*k, ir, frames, ir_rate, cut)) {
            idle_.store(k, std::memory_order_release);
            return LoadResult::Invalid;
        }
        source_.assign(ir, ir + frames);
        source_rate_ = ir_rate;
        pending_.store(k, std::memory_order_release);
        return cut ? LoadResult::Truncated : LoadResult::Ok;
    }

protected:
    bool on_prepare() override {
        std::lock_guard<std::mutex> lock(load_mutex_);
        max_taps_ = static_cast<std::uint32_t>(std::ceil(kMaxSeconds * sample_rate()));
        for (auto& k : kernels_) {
            k = std::make_unique<dsp::ConvolutionKernel>();
            if (!k->prepare(kPartition, max_taps_)) return false;
        }
        for (auto& c : conv_) {
            if (!c.prepare(kPartition, max_taps_)) return false;
        }
        fade_len_ = std::max<std::uint32_t>(
            16, static_cast<std::uint32_t>(std::lround(kFadeMs * 1e-3 * sample_rate())));
        fade_.resize(fade_len_ + 1);
        for (std::uint32_t i = 0; i <= fade_len_; ++i) {
            // Raised cosine, 1 -> 0; the fade-in reads it backwards.
            fade_[i] = 0.5 + 0.5 * std::cos(dsp::kPi * static_cast<double>(i) / fade_len_);
        }
        cur_ = kernels_[0].get();
        pending_.store(nullptr, std::memory_order_relaxed);
        idle_.store(kernels_[1].get(), std::memory_order_relaxed);
        phase_ = Phase::Steady;
        fpos_ = 0;
        if (!source_.empty()) {
            bool cut = false;
            if (!build(*cur_, source_.data(), source_.size(), source_rate_, cut)) return false;
        }
        prepared_ = true;
        set_latency(dsp::Convolver::latency());
        return true;
    }

    void on_reset() noexcept override {
        for (auto& c : conv_) c.reset();
    }

    void render(const float* const* in, float* const* out, const float* const*, std::uint32_t,
                std::uint32_t frames) noexcept override {
        const std::uint32_t chs = channels();
        std::uint32_t done = 0;
        while (done < frames) {
            if (phase_ == Phase::Steady) {
                dsp::ConvolutionKernel* n = pending_.load(std::memory_order_acquire);
                if (n != nullptr) {
                    incoming_ = n;
                    phase_ = Phase::Out;
                    fpos_ = 0;
                }
            }
            std::uint32_t seg = frames - done;
            if (phase_ != Phase::Steady) seg = std::min(seg, phase_len() - fpos_);
            for (std::uint32_t c = 0; c < chs; ++c) conv_[c].process(*cur_, in[c] + done, out[c] + done, seg);
            for (std::uint32_t i = 0; i < seg; ++i) {
                double g = 1.0;
                if (phase_ == Phase::Out) g = fade_[fpos_ + i];
                else if (phase_ == Phase::Hold) g = 0.0;
                else if (phase_ == Phase::In) g = fade_[fade_len_ - (fpos_ + i)];
                const double og = dsp::db_to_gain(smoother(kOutput).next());
                const double mx = smoother(kMix).next() * 0.01;
                const double dry = 1.0 - mx, wet = mx * g;
                const std::uint32_t k = done + i;
                for (std::uint32_t c = 0; c < chs; ++c) {
                    out[c][k] = static_cast<float>(og * (dry * in[c][k] + wet * out[c][k]));
                }
            }
            done += seg;
            if (phase_ != Phase::Steady) {
                fpos_ += seg;
                if (fpos_ >= phase_len()) {
                    fpos_ = 0;
                    if (phase_ == Phase::Out) {
                        dsp::ConvolutionKernel* old = cur_;
                        cur_ = incoming_;
                        // Cleared before the old kernel is released, so a
                        // loader that takes it cannot meet a stale pending.
                        pending_.store(nullptr, std::memory_order_relaxed);
                        idle_.store(old, std::memory_order_release);
                        phase_ = Phase::Hold;
                    } else if (phase_ == Phase::Hold) {
                        phase_ = Phase::In;
                    } else {
                        phase_ = Phase::Steady;
                    }
                }
            }
        }
    }

private:
    // Hold: the convolver takes a new kernel's head at once but its tail
    // only at the next block of P, so the wet path stays at zero for P
    // samples after the swap, and the fade-in meets only the new response.
    enum class Phase { Steady, Out, Hold, In };
    std::uint32_t phase_len() const noexcept { return phase_ == Phase::Hold ? kPartition : fade_len_; }

    bool build(dsp::ConvolutionKernel& k, const float* ir, std::size_t frames, double ir_rate, bool& truncated) {
        std::vector<double> taps = resample(ir, frames, ir_rate, sample_rate());
        truncated = false;
        if (taps.size() > max_taps_) {
            truncated = true;
            taps.resize(max_taps_);
            const std::size_t f =
                std::min<std::size_t>(taps.size() / 2, static_cast<std::size_t>(0.010 * sample_rate()));
            for (std::size_t i = 0; i < f; ++i) {
                taps[taps.size() - 1 - i] *=
                    0.5 - 0.5 * std::cos(dsp::kPi * (static_cast<double>(i) + 0.5) / static_cast<double>(f));
            }
        }
        return k.set(taps.data(), static_cast<std::uint32_t>(taps.size()));
    }

    IrReader reader_ = nullptr;
    mutable std::mutex file_mutex_;  // control threads only; held across a whole file load
    std::string path_;
    std::mutex load_mutex_;  // control threads only; never taken by render()
    bool prepared_ = false;
    std::vector<float> source_;
    double source_rate_ = 0.0;
    std::uint32_t max_taps_ = 0;

    std::unique_ptr<dsp::ConvolutionKernel> kernels_[2];
    std::atomic<dsp::ConvolutionKernel*> pending_{nullptr};  // loader -> audio
    std::atomic<dsp::ConvolutionKernel*> idle_{nullptr};     // audio -> loader
    dsp::ConvolutionKernel* cur_ = nullptr;                  // audio thread only
    dsp::ConvolutionKernel* incoming_ = nullptr;
    dsp::Convolver conv_[2];

    Phase phase_ = Phase::Steady;
    std::uint32_t fpos_ = 0, fade_len_ = 16;
    std::vector<double> fade_;
};

}  // namespace

std::unique_ptr<Cab> make_cab() { return std::make_unique<CabImpl>(); }

}  // namespace sonora::native
