// SPDX-License-Identifier: GPL-3.0-or-later
//
// Frames <-> ticks <-> bars|beats against the session's tempo map
// (brain/specs/midi.md §2, ADR-0014 §2). The one conversion every reader
// shares: the plan builder, the take resolver, the interface's counters,
// ruler and grid, the SMF converter. Two readers with two conversions would
// put a note at 5|1|000 and the grid line at 5|1|000 a frame apart.
//
// Integer arithmetic on the exact value of each bpm float, with 128-bit
// products written portably (MSVC has no __int128, ADR-0008). No float or
// double is in a conversion, so GCC and MSVC, with or without FMA
// contraction, give the same tick.
//
// Never on the audio thread (midi.md §2.5): building one allocates, and a
// conversion may divide in 128 bits. The plan carries frames.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "sonora/model/model.h"

namespace sonora::model {

using Ticks = std::int64_t;

// Pro Tools' published internal resolution. What decides it is that a tick
// is shorter than a frame above 3 bpm at 48 kHz, so a recorded event comes
// back to its frame (midi.md §1).
constexpr Ticks kTicksPerQuarter = 960'000;
// A beat of meter n/d lasts kTicksPerWhole / d ticks, an integer for every
// denominator the bounds allow.
constexpr Ticks kTicksPerWhole = 4 * kTicksPerQuarter;
// The counters' ticks: "5|3|480" is 480 of 960 into the beat.
constexpr int kDisplayTicksPerBeat = 960;

// The bounds that keep every numerator under 2^107 and every denominator
// under 2^53 (midi.md §2.2, §2.4; owner question O7).
constexpr float kMinBpm = 1.0f;
constexpr float kMaxBpm = 1000.0f;
constexpr std::uint32_t kMaxMeterDenominator = 64;
constexpr std::uint32_t kMaxSampleRate = 384'000;

// Every frame and tick a conversion takes or gives.
constexpr std::int64_t kMinMusical = -(std::int64_t{1} << 53);
constexpr std::int64_t kMaxMusical = (std::int64_t{1} << 53) - 1;

// A power of two from 1 to kMaxMeterDenominator: what an SMF time signature
// can express, and what makes a beat a whole number of ticks.
bool is_meter_denominator(std::uint32_t d);

// What the v3 -> v4 migration does to a value outside the bounds (O7), and
// what a reader handed a map that never went through the loader may do.
// Clamped into [kMinBpm, kMaxBpm]; a bpm that is not a positive finite
// number is returned as it is, for the format's own check to refuse.
float bpm_within_bounds(float bpm);
// The nearest allowed power of two; halfway goes to the larger, so 3 reads
// as 4 and 6 as 8. Anything above 64 is 64; 0 stays 0, for the format's own
// check to refuse.
std::uint32_t nearest_meter_denominator(std::uint32_t d);

// Empty when `tempo` is a map the format allows at `sample_rate`, inside
// the bounds; else the reason, with `where` the element it is about
// ("tempo[2]", "sample_rate"), relative to the session's top level.
std::string tempo_map_problem(std::uint32_t sample_rate, const std::vector<TempoEntry>& tempo,
                              std::string& where);

// A position read as the map counts it: 1-based bar and beat (the meter's
// denominator note), display ticks 0 to 959.
struct BarsBeats {
    std::int64_t bar = 1;
    std::int64_t beat = 1;
    int tick = 0;
    bool operator==(const BarsBeats& o) const {
        return bar == o.bar && beat == o.beat && tick == o.tick;
    }
};

class MusicalMap {
public:
    // One entry of the tempo map with what is derived from those before it.
    // Never stored: a map is rebuilt from the settings it reads.
    struct Entry {
        Frames at = 0;               // a_k
        Ticks tick = 0;              // T_k, rounded once from the entry before
        // Ticks per frame, the exact rational value, reduced: num / den.
        std::uint64_t num = 1, den = 1;
        std::uint32_t beats = 4;     // the meter's numerator
        std::uint32_t note = 4;      // the meter's denominator
        Ticks beat_ticks = 0;        // kTicksPerWhole / note
        std::int64_t bar = 0;        // 0-based, the bar in progress at `tick`
        Ticks bar_tick = 0;          // where that bar began
        // The largest |delta| for which the 64-bit fast path cannot
        // overflow, frames -> ticks and ticks -> frames.
        std::uint64_t fast_frames = 0, fast_ticks = 0;
    };

    // Empty: every conversion fails.
    MusicalMap() = default;

    // False, with the reason and the key path, when the rate or map is
    // outside the bounds or not shaped as the format requires; the map is
    // then left empty.
    bool build(std::uint32_t sample_rate, const std::vector<TempoEntry>& tempo,
               std::string& why);
    bool build(const SessionSettings& s, std::string& why) {
        return build(s.sample_rate, s.tempo, why);
    }

    bool empty() const { return entries_.empty(); }
    std::uint32_t sample_rate() const { return rate_; }
    const std::vector<Entry>& entries() const { return entries_; }
    // Entries [0, reachable()) have a tick inside [kMinMusical, kMaxMusical].
    // A high tempo at a low rate can put a later entry past 2^53 ticks;
    // every frame from there on is out of range.
    std::size_t reachable() const { return reachable_; }

    // tick(f) = T_k + round((f - a_k) * tau_k), k the last entry with
    // a_k <= f, entry 0 before it; frame(t) = a_k + round((t - T_k) / tau_k),
    // k the last reachable entry with T_k <= t. Halves round towards +inf,
    // by floor division, so negative arguments are defined. False, with
    // `out` untouched, when the argument or the result is outside
    // [kMinMusical, kMaxMusical]: refused by the caller, never wrapped.
    bool tick_at(Frames f, Ticks& out) const;
    bool frame_at(Ticks t, Frames& out) const;

    // Many at once. Each entry is found once and then walked, which is what
    // makes a plan's worth of events affordable (midi.md §2.2, "Cost");
    // input in any other order is still converted correctly, only slower.
    // Returns how many were converted from the front: `n`, or the index of
    // the first one out of range, where it stops.
    std::size_t frames_to_ticks(const Frames* in, std::size_t n, Ticks* out) const;
    std::size_t ticks_to_frames(const Ticks* in, std::size_t n, Frames* out) const;

    // The session-format.md rule ("How the map is counted") in ticks. A
    // tick before 0 reads in bar 0 or below. An empty map reads 1|1|000.
    BarsBeats bars_beats_at(Ticks t) const;
    // The tick a reading names. A beat of a bar a meter change cut short,
    // past the change, names the change's own tick; anything before 1|1|000
    // names 0. False when the result would be out of range.
    bool tick_of(const BarsBeats& b, Ticks& out) const;

    // Indices into entries(): the entry in force at a frame, and at a tick.
    std::size_t entry_at_frame(Frames f) const;
    std::size_t entry_at_tick(Ticks t) const;

private:
    std::uint32_t rate_ = 0;
    std::vector<Entry> entries_;
    std::size_t reachable_ = 0;
};

}  // namespace sonora::model
