// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Ticks <-> samples through a tempo map.
//
// Deliberately minimal: tempo changes only, step changes, no ramps, no
// meter. It is shaped to move into common/model once that exists
// (timeline-model.md): a value type, sorted points, no hidden state that a
// snapshot could not copy.
//
// Tempo is held as microseconds per quarter note, as SMF stores it, not as
// BPM. That keeps every conversion exact in integers and a file's tempo
// round-trips bit for bit; bpm() and usec_from_bpm() convert at the edges.

#include <cstdint>
#include <vector>

#include "sonora/midi/event.h"

namespace sonora::midi {

struct TempoPoint {
    Tick tick = 0;
    std::uint32_t usec_per_quarter = default_usec_per_quarter;

    friend bool operator==(const TempoPoint& a, const TempoPoint& b) {
        return a.tick == b.tick && a.usec_per_quarter == b.usec_per_quarter;
    }
};

class TempoMap {
public:
    // One point at tick 0, at the SMF default tempo. ppq must be 1..32767,
    // the range an SMF header can express; outside it is clamped.
    explicit TempoMap(int ppq = 960,
                      std::uint32_t usec_per_quarter = default_usec_per_quarter);

    // Every tempo event in the list, in tick order. A tempo before the
    // first event at tick 0 is the default; a later event at a tick
    // replaces an earlier one at the same tick, as a player would.
    static TempoMap from_events(int ppq, const EventList& events);

    // Inserts, or replaces the tempo at that tick. Ticks below 0 and tempos
    // outside 1..0xFFFFFF (the SMF field) are clamped.
    void set_tempo(Tick tick, std::uint32_t usec_per_quarter);

    int ppq() const { return ppq_; }
    const std::vector<TempoPoint>& points() const { return points_; }
    std::uint32_t usec_per_quarter_at(Tick tick) const;

    // Exact: rounded to the nearest sample once, never accumulated. Ticks
    // before 0 extrapolate the first tempo. Exact for positions up to about
    // 2^38 ticks (days of music at any sane ppq), beyond which the
    // intermediate products overflow.
    std::int64_t ticks_to_samples(Tick tick, std::uint32_t sample_rate) const;
    // Rounded to the nearest tick. When a tick spans at least one sample,
    // samples_to_ticks(ticks_to_samples(t)) == t.
    Tick samples_to_ticks(std::int64_t samples, std::uint32_t sample_rate) const;

    double seconds_at(Tick tick) const;

    static double bpm(std::uint32_t usec_per_quarter);
    static std::uint32_t usec_from_bpm(double bpm);

    friend bool operator==(const TempoMap& a, const TempoMap& b) {
        return a.ppq_ == b.ppq_ && a.points_ == b.points_;
    }

private:
    std::size_t segment_at(Tick tick) const;
    void rebuild();

    int ppq_;
    std::vector<TempoPoint> points_;
    // Elapsed time at each point in microsecond-ticks (us * ppq): the sum of
    // ticks x us/quarter over earlier segments. Integer, so no drift.
    std::vector<std::int64_t> elapsed_;
};

}  // namespace sonora::midi
