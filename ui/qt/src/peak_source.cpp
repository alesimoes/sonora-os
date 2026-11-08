// SPDX-License-Identifier: GPL-3.0-or-later
#include "peak_source.h"

#include <algorithm>
#include <cmath>

#include "sonora/peak_file.h"

namespace sonora::ui {

namespace {

constexpr double kPi = 3.14159265358979323846;

// A stable pseudo-random value in [0, 1) for (source, step): the same bucket
// gets the same jitter however often the timeline is redrawn or scrolled.
float hash01(uint64_t a, uint64_t b) {
    uint64_t x = a * 0x9e3779b97f4a7c15ull ^ (b + 0x632be59bd9b4e019ull);
    x ^= x >> 31;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    return static_cast<float>(x >> 40) / static_cast<float>(1ull << 24);
}

}  // namespace

float SyntheticPeakSource::envelope(model::Id source, model::Frames frame) const {
    if (frame < 0 || rate_ == 0) return 0.0f;
    const double t = static_cast<double>(frame) / rate_;
    const double beat = 60.0 / bpm_;
    const uint64_t step = static_cast<uint64_t>(frame / std::max<uint32_t>(1, rate_ / 200));
    const float grain = 0.85f + 0.15f * hash01(source, step);
    switch (source % 3) {
        case 0: {  // percussive: a hit on every beat, a smaller one off it
            const double ph = std::fmod(t, beat) / beat;
            const double off = std::fmod(t + beat / 2, beat) / beat;
            const double v = 0.9 * std::exp(-ph * 14.0) + 0.35 * std::exp(-off * 18.0);
            return static_cast<float>(std::min(1.0, v + 0.03)) * grain;
        }
        case 1: {  // sustained: a slow swell, as a bass or a pad
            const double v = 0.45 + 0.2 * std::sin(2 * kPi * t / (beat * 8) + source) +
                             0.12 * std::sin(2 * kPi * t / beat);
            return static_cast<float>(std::clamp(v, 0.05, 0.95)) * grain;
        }
        default: {  // phrased: sung lines with breaths between them
            const double bar = beat * 4;
            const double ph = std::fmod(t, bar * 2) / (bar * 2);
            const double gate = ph < 0.8 ? std::sin(kPi * ph / 0.8) : 0.02;
            const double v = gate * (0.55 + 0.3 * std::sin(2 * kPi * t * 0.7 + source));
            return static_cast<float>(std::clamp(v, 0.01, 0.95)) * grain;
        }
    }
}

void SyntheticPeakSource::peaks(model::Id source, uint16_t channel, model::Frames first,
                                double frames_per_bucket, int count, PeakPair* out) const {
    (void)channel;
    const double fpb = std::max(1.0, frames_per_bucket);
    for (int i = 0; i < count; ++i) {
        const double a = static_cast<double>(first) + i * fpb;
        // Four probes a bucket: enough for the shape of an overview, and the
        // cost of a full-window rebuild stays a few thousand evaluations.
        float e = 0.0f;
        for (int k = 0; k < 4; ++k) {
            e = std::max(e, envelope(source, static_cast<model::Frames>(a + fpb * k / 4.0)));
        }
        const float asym = 0.9f + 0.1f * hash01(source + 7, static_cast<uint64_t>(a));
        out[i] = PeakPair{-e * asym, e};
    }
}

// --- FilePeakSource --------------------------------------------------------

struct FilePeakSource::Entry {
    Located where;
    enum class State { Queued, Ready, Failed } state = State::Queued;
    std::shared_ptr<const PeakFile> file;
};

FilePeakSource::FilePeakSource(Locate locate, std::function<void()> on_ready)
    : locate_(std::move(locate)), on_ready_(std::move(on_ready)), worker_([this] { work(); }) {}

FilePeakSource::~FilePeakSource() {
    {
        std::lock_guard<std::mutex> lock(m_);
        stop_ = true;
    }
    cv_.notify_all();
    worker_.join();
}

std::shared_ptr<FilePeakSource::Entry> FilePeakSource::entry(model::Id source) const {
    Located where;
    if (!locate_ || !locate_(source, where)) return nullptr;
    std::lock_guard<std::mutex> lock(m_);
    auto it = entries_.find(source);
    // Another session's source with the same id, or a take that grew.
    if (it != entries_.end() && (it->second->where.peak != where.peak ||
                                 it->second->where.length != where.length)) {
        entries_.erase(it);
        it = entries_.end();
    }
    if (it == entries_.end()) {
        auto e = std::make_shared<Entry>();
        e->where = std::move(where);
        it = entries_.emplace(source, e).first;
        queue_.push_back(e);
        cv_.notify_one();
    }
    return it->second;
}

void FilePeakSource::peaks(model::Id source, uint16_t channel, model::Frames first,
                           double frames_per_bucket, int count, PeakPair* out) const {
    for (int i = 0; i < count; ++i) out[i] = PeakPair{};
    const std::shared_ptr<Entry> e = entry(source);
    if (e == nullptr) return;
    std::shared_ptr<const PeakFile> file;
    {
        std::lock_guard<std::mutex> lock(m_);
        if (e->state == Entry::State::Ready) file = e->file;
    }
    if (file == nullptr) return;
    std::vector<float> lo(static_cast<std::size_t>(count)), hi(static_cast<std::size_t>(count));
    file->read(channel, first, frames_per_bucket, count, lo.data(), hi.data());
    for (int i = 0; i < count; ++i) out[i] = PeakPair{lo[static_cast<std::size_t>(i)],
                                                      hi[static_cast<std::size_t>(i)]};
}

bool FilePeakSource::ready(model::Id source) const {
    const std::shared_ptr<Entry> e = entry(source);
    if (e == nullptr) return false;
    std::lock_guard<std::mutex> lock(m_);
    return e->state == Entry::State::Ready;
}

void FilePeakSource::wait_idle() const {
    std::unique_lock<std::mutex> lock(m_);
    idle_.wait(lock, [this] { return queue_.empty() && !busy_; });
}

void FilePeakSource::work() {
    for (;;) {
        std::shared_ptr<Entry> e;
        {
            std::unique_lock<std::mutex> lock(m_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            e = queue_.front();
            queue_.erase(queue_.begin());
            busy_ = true;
        }
        auto file = std::make_shared<PeakFile>();
        std::string why;
        bool ok = file->open(e->where.peak, why) && file->frames() == e->where.length;
        // Missing, stale or unreadable: build it from the audio.
        if (!ok && build_peak_file(e->where.wav, e->where.peak, why)) {
            ok = file->open(e->where.peak, why) && file->frames() == e->where.length;
        }
        {
            std::lock_guard<std::mutex> lock(m_);
            e->file = ok ? file : nullptr;
            e->state = ok ? Entry::State::Ready : Entry::State::Failed;
        }
        // Redraw is requested before idle is announced, so a waiter that
        // sees idle also sees the redraw.
        if (ok && on_ready_) on_ready_();
        {
            std::lock_guard<std::mutex> lock(m_);
            busy_ = false;
        }
        idle_.notify_all();
    }
}

}  // namespace sonora::ui
