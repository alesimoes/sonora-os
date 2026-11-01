// SPDX-License-Identifier: GPL-3.0-or-later
#include "live_take.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sonora::ui {

using model::Frames;

namespace {

constexpr Frames kOpenEnd = std::numeric_limits<Frames>::max();

// Where the engine keeps the take's region (engine/control closed_take.cpp,
// document_punch() and document_loop()); `loop` when each pass starts again
// at `from`.
struct Window {
    Frames from = 0;
    Frames to = kOpenEnd;
    bool loop = false;
};

Window window_of(const model::Transport& t) {
    Window w;
    if (t.punch[0] >= 0 && t.punch[1] > t.punch[0]) {
        w.from = t.punch[0];
        w.to = t.punch[1];
    } else if (t.loop_record && t.loop[0] >= 0 && t.loop[1] > t.loop[0]) {
        w.from = t.loop[0];
        w.to = t.loop[1];
        w.loop = true;
    }
    return w;
}

// The furthest a first reading may sit past the one before it and still
// have the take start at the earlier one: a few ticks. Further means the
// transport located, and the take starts where it went.
constexpr Frames kStartSlack = 8 * LiveTake::kBucket;

}  // namespace

void LiveTake::clear() {
    lanes_.clear();
    recording_ = false;
    stopped_ticks_ = 0;
}

void LiveTake::start_lane(int row, Frames at) {
    Lane l;
    l.row = row;
    l.start = at;
    l.end = at;
    lanes_.push_back(std::move(l));
}

void LiveTake::tick(bool recording, Frames playhead, const std::vector<bool>& armed,
                    const std::vector<bool>& audio, const std::vector<float>& peaks,
                    const model::Transport& settings) {
    const Frames previous = last_;
    const bool had_previous = seen_;
    last_ = playhead;
    seen_ = true;
    if (!recording) {
        if (recording_) {
            recording_ = false;
            stopped_ticks_ = 0;
        }
        if (!lanes_.empty() && ++stopped_ticks_ > kHoldTicks) clear();
        return;
    }
    const Window w = window_of(settings);
    Frames from = previous;
    if (!recording_) {
        // A new take: what an earlier one left is not this one's.
        lanes_.clear();
        recording_ = true;
        stopped_ticks_ = 0;
        // The take began where the playhead stood at the previous reading,
        // so the peak read now, the maximum since then, is its first.
        if (!had_previous || previous > playhead || playhead - previous > kStartSlack) {
            from = playhead;
        }
    } else if (playhead < previous) {
        if (!w.loop) return;   // no other take runs backwards
        // A loop take's next pass: drawn again from A, as it records over.
        for (Lane& l : lanes_) {
            l.start = l.end = w.from;
            l.peaks.clear();
        }
        from = w.from;
    }
    const Frames a = std::max(from, w.from);
    const Frames b = std::min(playhead, w.to);
    for (std::size_t i = 0; i < armed.size() && i < peaks.size(); ++i) {
        if (!armed[i] || i >= audio.size() || !audio[i]) continue;
        const int row = static_cast<int>(i);
        Lane* lane = nullptr;
        for (Lane& l : lanes_) {
            if (l.row == row) lane = &l;
        }
        if (b <= a) {
            // Before the punch-in, or past the punch-out: nothing lands.
            continue;
        }
        if (lane == nullptr) {
            start_lane(row, a);
            lane = &lanes_.back();
        }
        if (lane->peaks.empty() && lane->end == lane->start) lane->start = lane->end = a;
        const Frames lo = std::max(a, lane->start);
        if (b <= lo) continue;
        const auto first = static_cast<std::size_t>((lo - lane->start) / kBucket);
        const auto last = static_cast<std::size_t>((b - 1 - lane->start) / kBucket);
        if (lane->peaks.size() <= last) lane->peaks.resize(last + 1, 0.0f);
        const float p = std::clamp(peaks[i], 0.0f, 1.0f);
        for (std::size_t k = first; k <= last; ++k) lane->peaks[k] = std::max(lane->peaks[k], p);
        lane->end = std::max(lane->end, b);
    }
}

void LiveTake::peaks(const Lane& lane, Frames first, double fpb, int count, PeakPair* out) const {
    for (int c = 0; c < count; ++c) {
        const double f0 = static_cast<double>(first) + c * fpb;
        const double f1 = f0 + std::max(1.0, fpb);
        const double lo = std::max(f0, static_cast<double>(lane.start));
        const double hi = std::min(f1, static_cast<double>(lane.end));
        float m = 0.0f;
        if (hi > lo && !lane.peaks.empty()) {
            const auto k0 = static_cast<std::size_t>((lo - lane.start) / kBucket);
            const auto k1 = std::min(lane.peaks.size() - 1,
                                     static_cast<std::size_t>((hi - 1 - lane.start) / kBucket));
            for (std::size_t k = k0; k <= k1 && k < lane.peaks.size(); ++k) {
                m = std::max(m, lane.peaks[k]);
            }
        }
        out[c] = {-m, m};
    }
}

}  // namespace sonora::ui
