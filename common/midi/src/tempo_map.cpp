// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/midi/tempo_map.h"

#include <algorithm>
#include <cmath>

namespace sonora::midi {

namespace {

constexpr std::uint32_t max_usec_per_quarter = 0xFFFFFF;

std::int64_t floor_div(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

std::uint32_t clamp_tempo(std::uint32_t usec) {
    return std::clamp<std::uint32_t>(usec, 1, max_usec_per_quarter);
}

}  // namespace

TempoMap::TempoMap(int ppq, std::uint32_t usec_per_quarter)
    : ppq_(std::clamp(ppq, 1, 32767)),
      points_{TempoPoint{0, clamp_tempo(usec_per_quarter)}} {
    rebuild();
}

TempoMap TempoMap::from_events(int ppq, const EventList& events) {
    TempoMap map(ppq);
    EventList sorted;
    for (const Event& e : events) {
        if (tempo_of(e)) sorted.push_back(e);
    }
    sort_by_tick(sorted);
    for (const Event& e : sorted) map.set_tempo(e.tick, *tempo_of(e));
    return map;
}

void TempoMap::set_tempo(Tick tick, std::uint32_t usec_per_quarter) {
    const TempoPoint p{std::max<Tick>(tick, 0), clamp_tempo(usec_per_quarter)};
    auto it = std::lower_bound(
        points_.begin(), points_.end(), p.tick,
        [](const TempoPoint& a, Tick t) { return a.tick < t; });
    if (it != points_.end() && it->tick == p.tick) {
        *it = p;
    } else {
        points_.insert(it, p);
    }
    rebuild();
}

void TempoMap::rebuild() {
    elapsed_.assign(points_.size(), 0);
    for (std::size_t i = 1; i < points_.size(); ++i) {
        elapsed_[i] = elapsed_[i - 1] +
                      (points_[i].tick - points_[i - 1].tick) *
                          static_cast<std::int64_t>(points_[i - 1].usec_per_quarter);
    }
}

std::size_t TempoMap::segment_at(Tick tick) const {
    auto it = std::upper_bound(
        points_.begin(), points_.end(), tick,
        [](Tick t, const TempoPoint& a) { return t < a.tick; });
    return it == points_.begin() ? 0 : static_cast<std::size_t>(it - points_.begin() - 1);
}

std::uint32_t TempoMap::usec_per_quarter_at(Tick tick) const {
    return points_[segment_at(tick)].usec_per_quarter;
}

std::int64_t TempoMap::ticks_to_samples(Tick tick, std::uint32_t sample_rate) const {
    const std::size_t i = segment_at(tick);
    const std::int64_t n =
        elapsed_[i] + (tick - points_[i].tick) *
                          static_cast<std::int64_t>(points_[i].usec_per_quarter);
    // n / d seconds. Split into whole and remainder so that no product
    // exceeds about 2^56: r < d (2^35 at most) times a rate below 2^20.
    const std::int64_t d = static_cast<std::int64_t>(ppq_) * 1000000;
    const std::int64_t rate = sample_rate;
    const std::int64_t q = floor_div(n, d);
    const std::int64_t r = n - q * d;
    return q * rate + floor_div(2 * r * rate + d, 2 * d);
}

Tick TempoMap::samples_to_ticks(std::int64_t samples, std::uint32_t sample_rate) const {
    const std::int64_t rate = sample_rate;
    if (rate <= 0) return 0;

    // The segment whose first sample is at or before this one.
    std::size_t lo = 0;
    std::size_t hi = points_.size();
    while (hi - lo > 1) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (ticks_to_samples(points_[mid].tick, sample_rate) <= samples) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    const TempoPoint& p = points_[lo];
    const std::int64_t usec = p.usec_per_quarter;
    const std::int64_t d = static_cast<std::int64_t>(ppq_) * 1000000;

    // Elapsed microsecond-ticks at this sample: samples * d / rate, kept
    // as an integer x plus a fraction a0 / rate.
    const std::int64_t qs = floor_div(samples, rate);
    const std::int64_t rs = samples - qs * rate;
    const std::int64_t a = rs * d;
    const std::int64_t x = qs * d + a / rate - elapsed_[lo];
    const std::int64_t a0 = a % rate;

    // Ticks into the segment: (x + a0 / rate) / usec, rounded to nearest.
    const std::int64_t whole = floor_div(x, usec);
    const std::int64_t part = x - whole * usec;
    const bool round_up = 2 * (part * rate + a0) >= usec * rate;
    return p.tick + whole + (round_up ? 1 : 0);
}

double TempoMap::seconds_at(Tick tick) const {
    const std::size_t i = segment_at(tick);
    const double n =
        static_cast<double>(elapsed_[i]) +
        static_cast<double>(tick - points_[i].tick) * points_[i].usec_per_quarter;
    return n / (static_cast<double>(ppq_) * 1e6);
}

double TempoMap::bpm(std::uint32_t usec_per_quarter) {
    return 60e6 / static_cast<double>(clamp_tempo(usec_per_quarter));
}

std::uint32_t TempoMap::usec_from_bpm(double bpm) {
    if (!(bpm > 0.0)) return max_usec_per_quarter;
    const double usec = std::round(60e6 / bpm);
    if (usec >= max_usec_per_quarter) return max_usec_per_quarter;
    if (usec < 1.0) return 1;
    return static_cast<std::uint32_t>(usec);
}

}  // namespace sonora::midi
