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
#include "sonora/dsp/ir_prepare.h"
#include "sonora/dsp/math.h"
#include "sonora/dsp/resample.h"

namespace sonora::native {

namespace {

using namespace cab;

const char* const kOffOn[] = {"off", "on"};

const ParamInfo kParams[kParamCount] = {
    kBypassParam,
    {"output", "Output", Unit::Decibel, Taper::Linear, -24.0f, 12.0f, 0.0f, 50.0f, true, false, false, "%.1f dB", nullptr, 0},
    {"mix", "Mix", Unit::Percent, Taper::Linear, 0.0f, 100.0f, 100.0f, 20.0f, true, false, false, "%.0f %%", nullptr, 0},
    {"trim", "Trim", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 0.0f, 0.0f, false, true, false, "%.0f", kOffOn, 2},
    {"normalise", "Normalise", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 0.0f, 0.0f, false, true, false, "%.0f", kOffOn, 2},
};

const Descriptor kDesc = {"org.sonora.cab", "Sonora Cabinet", "Amp", 2, kParams, kParamCount, 1, 2, false};

// P = 128: a 128-tap direct head and one 256-point transform pair a block.
// The cost for a 0.3 s response is in quality.json's note.
constexpr std::uint32_t kPartition = 128;

// Offline rate conversion with the shared polyphase resampler (ADR-0180),
// run in load_ir(), never in process(). The stage's group delay D (high-rate
// samples) is raised to a multiple of p*q when the table allows, so it is a
// whole number of output samples (D/q) and the response lands exactly on the
// session's sample grid; the first D/q outputs are dropped. Otherwise (an
// exotic ratio) D stays a multiple of p and is rounded to a sample.
bool resample(const float* x, std::size_t n, double from, double to, std::vector<double>& y) {
    if (from == to) {
        y.assign(x, x + n);
        return true;
    }
    dsp::RatePlan plan;
    std::string err;
    if (!dsp::plan_rates(from, to, &plan, &err)) return false;
    const std::uint64_t pq = static_cast<std::uint64_t>(plan.p) * plan.q;
    std::uint64_t delay = plan.delay_up;
    if (2 * (delay + pq) + plan.p <= dsp::Resampler::kMaxCoefficients) delay = (delay + pq - 1) / pq * pq;
    constexpr std::uint32_t kChunk = 4096;
    dsp::Resampler rs;
    if (!rs.prepare(plan.p, plan.q, plan.cut_up, static_cast<std::uint32_t>(delay), kChunk)) return false;
    const std::size_t skip = static_cast<std::size_t>(std::llround(static_cast<double>(delay) / plan.q));
    const std::size_t out_n =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(static_cast<double>(n) * to / from)));
    y.assign(out_n, 0.0);
    std::vector<float> zeros(kChunk, 0.0f), out(rs.max_out());
    std::size_t made = 0, fed = 0;
    while (made < skip + out_n) {
        const std::uint32_t m = static_cast<std::uint32_t>(std::min<std::size_t>(kChunk, n > fed ? n - fed : kChunk));
        const float* src = fed < n ? x + fed : zeros.data();
        const std::uint32_t got = rs.process(src, m, out.data(), static_cast<std::uint32_t>(out.size()));
        fed += m;
        for (std::uint32_t i = 0; i < got; ++i, ++made) {
            if (made >= skip && made - skip < out_n) y[made - skip] = out[i];
        }
    }
    return true;
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
        prep_.trim = value(kTrim) >= 0.5f;
        prep_.normalise = value(kNormalise) >= 0.5f;
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
        // The stored source stays as recorded; the options are applied to a
        // copy, so switching them off again restores the response exactly. A
        // silent response is left as it is (nothing to trim or scale).
        std::vector<float> conditioned;
        if (prep_.trim || prep_.normalise) {
            conditioned.assign(ir, ir + frames);
            if (dsp::prepare_ir(conditioned, prep_)) {
                ir = conditioned.data();
                frames = conditioned.size();
            }
        }
        // Cut to kMaxSeconds of the response's own time before resampling, so
        // a long file costs no more than a 1 s one; the closing 10 ms fade is
        // laid on the source for the same reason.
        std::vector<float> cut_src;
        truncated = false;
        const auto keep = static_cast<std::size_t>(std::ceil(kMaxSeconds * ir_rate));
        if (frames > keep) {
            truncated = true;
            cut_src.assign(ir, ir + keep);
            const std::size_t f = std::min<std::size_t>(keep / 2, static_cast<std::size_t>(0.010 * ir_rate));
            for (std::size_t i = 0; i < f; ++i) {
                cut_src[keep - 1 - i] *=
                    0.5f - 0.5f * static_cast<float>(std::cos(dsp::kPi * (static_cast<double>(i) + 0.5) / static_cast<double>(f)));
            }
            ir = cut_src.data();
            frames = keep;
        }
        std::vector<double> taps;
        if (!resample(ir, frames, ir_rate, sample_rate(), taps)) return false;
        if (taps.size() > max_taps_) taps.resize(max_taps_);
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
    dsp::IrPrepare prep_;  // as of the last prepare(); load_mutex_

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
