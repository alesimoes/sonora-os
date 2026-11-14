// SPDX-License-Identifier: GPL-3.0-or-later
#include "chain.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>

#include "sonora/dsp/math.h"

namespace sonora::native::amp {

bool ModelChain::build(std::unique_ptr<NeuralModel> model, double capture_rate, double session_rate,
                       std::uint32_t max_frames, std::string* error) {
    if (!model || max_frames == 0) {
        if (error) *error = "no network to run";
        return false;
    }
    if (!dsp::plan_rates(session_rate, capture_rate, &plan_, error)) return false;
    max_frames_ = max_frames;
    if (!plan_.resample) {
        model->prepare(static_cast<int>(max_frames), capture_rate);
        latency_ = model->latency();
        capture_.clear();
        fifo_.clear();
    } else {
        // A network delay in capture samples is not a whole number of
        // session samples in general, and both kernels have none.
        if (model->latency() != 0) {
            if (error) *error = "a network with its own latency cannot run at another session rate";
            return false;
        }
        if (!dsp::prepare_pair(plan_, max_frames, &up_, &down_)) {
            if (error) *error = "the rate conversion cannot be prepared";
            return false;
        }
        capture_.assign(up_.max_out(), 0.0f);
        // After K session inputs the second stage has made at least K
        // outputs and at most K + q / p + 1, so what a block leaves over is
        // under q / p + 2: the FIFO holds that plus one call's worth.
        fifo_.assign(down_.max_out() + plan_.q / plan_.p + 4, 0.0f);
        model->prepare(static_cast<int>(up_.max_out()), capture_rate);
        latency_ = plan_.latency;
    }
    model_ = std::move(model);
    reset();
    return true;
}

void ModelChain::reset() noexcept {
    if (model_) model_->reset();
    if (plan_.resample) {
        up_.reset();
        down_.reset();
        std::fill(fifo_.begin(), fifo_.end(), 0.0f);
    }
    fill_ = 0;
}

void ModelChain::process(const float* in, float* out, std::uint32_t frames) noexcept {
    if (!plan_.resample) {
        model_->process(in, out, static_cast<int>(frames));
        return;
    }
    // All of `in` is read here, before `out` is written: they may alias.
    const std::uint32_t n = up_.process(in, frames, capture_.data(), static_cast<std::uint32_t>(capture_.size()));
    model_->process(capture_.data(), capture_.data(), static_cast<int>(n));
    fill_ += down_.process(capture_.data(), n, fifo_.data() + fill_, static_cast<std::uint32_t>(fifo_.size()) - fill_);
    const std::uint32_t take = std::min(frames, fill_);
    std::memcpy(out, fifo_.data(), sizeof(float) * take);
    // Never met (see build()); a guard so the output stays defined.
    for (std::uint32_t i = take; i < frames; ++i) out[i] = 0.0f;
    std::memmove(fifo_.data(), fifo_.data() + take, sizeof(float) * (fill_ - take));
    fill_ -= take;
}

namespace {

// A guitar-like test signal: a 110 Hz fundamental with a little noise, at
// a DI's level. The kernels' cost does not depend on it (fixed arithmetic),
// but a real signal keeps the timing honest should that ever change.
void fill_block(std::vector<float>& x, std::uint64_t& n, std::uint32_t& lcg, double rate) {
    for (float& v : x) {
        lcg = lcg * 1664525u + 1013904223u;
        const double noise = (static_cast<double>(lcg >> 8) / 16777216.0 - 0.5) * 0.02;
        v = static_cast<float>(0.25 * std::sin(2.0 * dsp::kPi * 110.0 * static_cast<double>(n) / rate) + noise);
        ++n;
    }
}

ChainCost summarise(std::vector<double> us) {
    ChainCost c;
    if (us.empty()) return c;
    std::sort(us.begin(), us.end());
    c.p999_us = us[static_cast<std::size_t>(0.999 * static_cast<double>(us.size() - 1))];
    c.max_us = us.back();
    return c;
}

}  // namespace

ChainCost time_chain(ModelChain& chain, std::uint32_t block, int blocks, int passes, double abort_max_us) {
    using Clock = std::chrono::steady_clock;
    block = std::min(block, chain.max_frames());
    std::vector<float> x(block), y(block);
    std::vector<double> best(static_cast<std::size_t>(blocks), std::numeric_limits<double>::infinity());
    std::vector<double> run;
    // The rate only shapes the test tone; 48 kHz is the class bars' rate.
    const double rate = 48000.0;
    for (int pass = 0; pass < passes; ++pass) {
        chain.reset();
        run.clear();
        std::uint64_t n = 0;
        std::uint32_t lcg = 12345u;
        double total = 0.0;
        for (int b = 0; b < blocks; ++b) {
            fill_block(x, n, lcg, rate);
            const auto t0 = Clock::now();
            chain.process(x.data(), y.data(), block);
            const auto t1 = Clock::now();
            const double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
            run.push_back(us);
            best[static_cast<std::size_t>(b)] = std::min(best[static_cast<std::size_t>(b)], us);
            total += us;
            if (total > static_cast<double>(blocks) * abort_max_us) {
                ChainCost c = summarise(run);
                c.stopped_early = true;
                chain.reset();
                return c;
            }
        }
    }
    chain.reset();
    return summarise(best);
}

}  // namespace sonora::native::amp
