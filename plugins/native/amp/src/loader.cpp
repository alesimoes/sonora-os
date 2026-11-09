// SPDX-License-Identifier: GPL-3.0-or-later
//
// The amp's control threads: building a capture's chain, timing it against
// the class-E budget (ADR-0111 §5), handing it to the audio thread, the
// "model" file slot, and prepare().
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include "amp_impl.h"
#include "sonora/ampcab/nam_file.h"
#include "sonora/dsp/math.h"

namespace sonora::native::amp {

namespace {

const FileSlotInfo kModelSlot = {"model", "Amp capture", "nam"};

// As the cabinet: a load that meets the previous one still being taken over
// waits for the audio thread's next block, 40 x 5 ms, then gives up.
constexpr int kBusyTries = 40;
constexpr int kBusyWaitMs = 5;

// The rate a capture is assumed to have when none is loaded: the format's
// default, so loading the usual capture never changes the latency.
constexpr double kDefaultCaptureRate = 48000.0;

std::string figure(double us) {
    char b[32];
    std::snprintf(b, sizeof b, "%.0f", us);
    return b;
}

std::string over_budget(const ChainCost& c, const Budget& b) {
    return "capture refused: on this machine it takes " + figure(c.p999_us) + " us per 128-sample block (p99.9) and " +
           figure(c.max_us) + " us at most, over the class E budget of " + figure(b.p999_us) + " us and " +
           figure(b.max_us) + " us" + (c.stopped_early ? " (timing stopped once the budget was clearly exceeded)" : "");
}

double trim_db_of(const CaptureSource& src) {
    if (!src.has_loudness || !std::isfinite(src.loudness_db)) return 0.0;
    return std::clamp(kReferenceDb - src.loudness_db, -kMaxTrimDb, kMaxTrimDb);
}

}  // namespace

const FileSlotInfo& AmpImpl::slot(std::uint32_t) const noexcept { return kModelSlot; }

bool AmpImpl::build_slot(const CaptureSource& src, double session_rate, std::uint32_t max_frames, Slot& slot,
                         Status& status, std::string& message) {
    std::string err;
    std::unique_ptr<NeuralModel> model = src.make ? src.make(&err) : nullptr;
    if (!model) {
        message = "capture refused: " + (err.empty() ? std::string("no network") : err);
        return false;
    }
    auto chain = std::make_unique<ModelChain>();
    // At least a q128 block, so the timing below is the class bars' block.
    if (!chain->build(std::move(model), src.rate, session_rate, std::max(max_frames, kTimedBlock), &err)) {
        message = "capture refused: " + err;
        return false;
    }
    const ChainCost cost = time_chain(*chain, kTimedBlock, kTimedBlocks, kTimedRuns, budget_.max_us);
    if (cost.p999_us > budget_.p999_us || cost.max_us > budget_.max_us) {
        message = over_budget(cost, budget_);
        return false;
    }
    slot.chain = std::move(chain);
    const double trim_db = trim_db_of(src);
    slot.trim = static_cast<float>(dsp::db_to_gain(trim_db));
    status = Status{};
    status.loaded = true;
    status.has_loudness = src.has_loudness;
    status.trim_db = trim_db;
    status.capture_rate = src.rate;
    status.p999_us = cost.p999_us;
    status.max_us = cost.max_us;
    return true;
}

Amp::LoadResult AmpImpl::load_capture(CaptureSource source, std::string& message) {
    message.clear();
    std::lock_guard<std::mutex> lock(load_mutex_);
    if (!prepared_) {
        // Timed at its own rate now, so an impossible capture is refused
        // when the operator picks it; prepare() times it again.
        Slot probe;
        Status st;
        if (!build_slot(source, source.rate, kTimedBlock, probe, st, message)) {
            notice_ = message;
            return LoadResult::Refused;
        }
        source_ = std::move(source);
        has_source_ = true;
        status_ = st;
        notice_.clear();
        return LoadResult::Ok;
    }
    // The idle slot first: without it the audio thread may still read both.
    Slot* s = idle_.exchange(nullptr, std::memory_order_acquire);
    if (s == nullptr) return LoadResult::Busy;
    Slot fresh;
    Status st;
    if (!build_slot(source, sample_rate(), max_frames(), fresh, st, message)) {
        idle_.store(s, std::memory_order_release);
        notice_ = message;
        return LoadResult::Refused;
    }
    if (fresh.chain->latency() != latency()) {
        idle_.store(s, std::memory_order_release);
        message = "the capture runs at " + figure(source.rate) + " Hz and needs " +
                  std::to_string(fresh.chain->latency()) + " samples of latency here, not " +
                  std::to_string(latency()) + ": it plays once the host restarts the plugin";
        source_ = std::move(source);
        has_source_ = true;
        status_ = st;
        notice_.clear();
        return LoadResult::Deferred;
    }
    // The old chain in the idle slot is freed here, on this thread.
    *s = std::move(fresh);
    pending_.store(s, std::memory_order_release);
    source_ = std::move(source);
    has_source_ = true;
    status_ = st;
    notice_.clear();
    return LoadResult::Ok;
}

Amp::LoadResult AmpImpl::load_nam(const ampcab::NamModel& model, std::string& message) {
    auto shared = std::make_shared<const ampcab::NamModel>(model);
    CaptureSource src;
    src.make = [shared](std::string* error) { return make_neural_model(*shared, error); };
    src.rate = model.sample_rate;
    src.has_loudness = model.has_loudness;
    src.loudness_db = model.loudness_db;
    return load_capture(std::move(src), message);
}

Amp::LoadResult AmpImpl::clear_capture() {
    std::lock_guard<std::mutex> lock(load_mutex_);
    if (prepared_) {
        Slot* s = idle_.exchange(nullptr, std::memory_order_acquire);
        if (s == nullptr) return LoadResult::Busy;
        *s = Slot{};
        pending_.store(s, std::memory_order_release);
    }
    source_ = CaptureSource{};
    has_source_ = false;
    status_ = Status{};
    notice_.clear();
    return LoadResult::Ok;
}

std::string AmpImpl::notice() const {
    std::lock_guard<std::mutex> lock(load_mutex_);
    return notice_;
}

Status AmpImpl::status() const {
    std::lock_guard<std::mutex> lock(load_mutex_);
    return status_;
}

bool AmpImpl::load(std::uint32_t index, const std::string& path, std::string& error) {
    if (index != 0) {
        error = "no such file slot";
        return false;
    }
    std::lock_guard<std::mutex> lock(file_mutex_);
    if (path.empty() && path_.empty()) return true;
    ampcab::NamModel model;
    if (!path.empty() && !ampcab::read_nam_file(path, &model, &error)) return false;
    LoadResult r = LoadResult::Busy;
    std::string message;
    for (int attempt = 0; attempt < kBusyTries && r == LoadResult::Busy; ++attempt) {
        if (attempt > 0) std::this_thread::sleep_for(std::chrono::milliseconds(kBusyWaitMs));
        r = path.empty() ? clear_capture() : load_nam(model, message);
    }
    if (r == LoadResult::Busy) {
        error = "the previous capture is still being taken over; no audio is running to finish it";
        return false;
    }
    if (r == LoadResult::Refused) {
        error = message;
        return false;
    }
    path_ = path;
    return true;
}

std::string AmpImpl::path(std::uint32_t) const {
    std::lock_guard<std::mutex> lock(file_mutex_);
    return path_;
}

std::uint32_t AmpImpl::default_latency() const {
    RatePlan plan;
    std::string err;
    const double rate = has_source_ ? source_.rate : kDefaultCaptureRate;
    // A session rate the resampler cannot reach leaves no capture playable
    // (build_slot refuses it) and nothing to delay.
    if (!plan_rates(sample_rate(), rate, &plan, &err)) return 0;
    return plan.latency;
}

void AmpImpl::size_buffers(std::uint32_t latency) {
    dry_len_ = latency + 1;
    dry_.assign(static_cast<std::size_t>(channels()) * dry_len_, 0.0f);
    dry_pos_ = 0;
    mono_.assign(max_frames(), 0.0f);
    wet_.assign(max_frames(), 0.0f);
    fade_len_ = std::max<std::uint32_t>(16, static_cast<std::uint32_t>(std::lround(kFadeMs * 1e-3 * sample_rate())));
    fade_.resize(fade_len_ + 1);
    for (std::uint32_t i = 0; i <= fade_len_; ++i) {
        fade_[i] = 0.5 + 0.5 * std::cos(dsp::kPi * static_cast<double>(i) / fade_len_);
    }
    // The resamplers' start-up, then one more fade's worth for the network
    // to answer the signal it was switched into.
    hold_len_ = latency + fade_len_;
}

bool AmpImpl::on_prepare() {
    std::lock_guard<std::mutex> lock(load_mutex_);
    slots_[0] = Slot{};
    slots_[1] = Slot{};
    cur_ = &slots_[0];
    incoming_ = nullptr;
    pending_.store(nullptr, std::memory_order_relaxed);
    idle_.store(&slots_[1], std::memory_order_relaxed);
    phase_ = Phase::Steady;
    fpos_ = 0;
    std::uint32_t latency = default_latency();
    if (has_source_) {
        Status st;
        std::string message;
        if (build_slot(source_, sample_rate(), max_frames(), slots_[0], st, message)) {
            latency = slots_[0].chain->latency();
            status_ = st;
        } else {
            // Refused here (a slower rate pair, a busier machine): the amp
            // passes the input through and the operator is told why.
            notice_ = message;
            source_ = CaptureSource{};
            has_source_ = false;
            status_ = Status{};
            latency = default_latency();
        }
    }
    size_buffers(latency);
    set_latency(latency);
    prepared_ = true;
    return true;
}

}  // namespace sonora::native::amp
