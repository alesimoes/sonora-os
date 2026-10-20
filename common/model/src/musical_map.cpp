// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/model/musical_map.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace sonora::model {

namespace {

constexpr std::uint64_t kLow32 = 0xFFFFFFFFu;
// Every quotient a conversion may keep: room to add a base inside
// [kMinMusical, kMaxMusical] without overflowing before the range check.
constexpr std::uint64_t kMaxQuotient = std::uint64_t{1} << 62;

// --- 128-bit unsigned arithmetic, portably ----------------------------------

struct U128 {
    std::uint64_t hi = 0, lo = 0;
};

U128 multiply(std::uint64_t a, std::uint64_t b) {
    const std::uint64_t a0 = a & kLow32, a1 = a >> 32;
    const std::uint64_t b0 = b & kLow32, b1 = b >> 32;
    const std::uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    const std::uint64_t mid = (p00 >> 32) + (p01 & kLow32) + (p10 & kLow32);
    return {p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32), (mid << 32) | (p00 & kLow32)};
}

U128 plus(U128 a, std::uint64_t b) {
    U128 r{a.hi, a.lo + b};
    if (r.lo < a.lo) ++r.hi;
    return r;
}

// a - b, for a >= b.
U128 minus(U128 a, std::uint64_t b) {
    U128 r{a.hi, a.lo - b};
    if (a.lo < b) --r.hi;
    return r;
}

U128 twice(U128 a) { return {(a.hi << 1) | (a.lo >> 63), a.lo << 1}; }

bool at_most(U128 a, std::uint64_t b) { return a.hi == 0 && a.lo <= b; }

int leading_zeros(std::uint64_t v) {   // v != 0
    int n = 0;
    for (int s = 32; s > 0; s >>= 1) {
        if ((v >> (64 - s)) == 0) {
            n += s;
            v <<= s;
        }
    }
    return n;
}

// floor(u / v) and the remainder, for u.hi < v so the quotient fits 64
// bits. Knuth's algorithm D in 32-bit digits (Hacker's Delight, divlu);
// the products that wrap are meant to.
std::uint64_t divide(U128 u, std::uint64_t v, std::uint64_t& rem) {
    constexpr std::uint64_t b = std::uint64_t{1} << 32;
    const int s = leading_zeros(v);
    v <<= s;
    const std::uint64_t vn1 = v >> 32, vn0 = v & kLow32;
    const std::uint64_t un32 = s == 0 ? u.hi : (u.hi << s) | (u.lo >> (64 - s));
    const std::uint64_t un10 = u.lo << s;
    const std::uint64_t un1 = un10 >> 32, un0 = un10 & kLow32;
    std::uint64_t q1 = un32 / vn1;
    std::uint64_t rhat = un32 - q1 * vn1;
    while (q1 >= b || q1 * vn0 > b * rhat + un1) {
        --q1;
        rhat += vn1;
        if (rhat >= b) break;
    }
    const std::uint64_t un21 = un32 * b + un1 - q1 * v;
    std::uint64_t q0 = un21 / vn1;
    rhat = un21 - q0 * vn1;
    while (q0 >= b || q0 * vn0 > b * rhat + un0) {
        --q0;
        rhat += vn1;
        if (rhat >= b) break;
    }
    rem = (un21 * b + un0 - q0 * v) >> s;
    return q1 * b + q0;
}

std::int64_t floor_div(std::int64_t a, std::int64_t b) {   // b > 0
    std::int64_t q = a / b;
    if (a % b != 0 && a < 0) --q;
    return q;
}

// round(x * p / q) with halves towards +inf: floor((2xp + q) / 2q). The
// bounds keep |x| < 2^54, p and q under 2^54, so 2|x|p + q < 2^109 and 2q
// fits 64 bits. `fast` is the largest |x| for which all of it fits int64.
// False when the quotient is too large to be a frame or a tick.
bool round_scaled(std::int64_t x, std::uint64_t p, std::uint64_t q, std::uint64_t fast,
                  std::int64_t& out) {
    const std::uint64_t mag =
        x < 0 ? std::uint64_t{0} - static_cast<std::uint64_t>(x) : static_cast<std::uint64_t>(x);
    if (mag <= fast) {
        out = floor_div(2 * x * static_cast<std::int64_t>(p) + static_cast<std::int64_t>(q),
                        2 * static_cast<std::int64_t>(q));
        return true;
    }
    const U128 a = twice(multiply(mag, p));
    const std::uint64_t d = 2 * q;
    std::uint64_t rem = 0;
    if (x >= 0) {
        const U128 n = plus(a, q);
        if (n.hi >= d) return false;
        const std::uint64_t quot = divide(n, d, rem);
        if (quot > kMaxQuotient) return false;
        out = static_cast<std::int64_t>(quot);
        return true;
    }
    // q - 2|x|p: in [0, q] it floors to 0; below, floor away from zero.
    if (at_most(a, q)) {
        out = 0;
        return true;
    }
    const U128 n = minus(a, q);
    if (n.hi >= d) return false;
    std::uint64_t quot = divide(n, d, rem);
    if (rem != 0) ++quot;
    if (quot > kMaxQuotient) return false;
    out = -static_cast<std::int64_t>(quot);
    return true;
}

bool in_musical_range(std::int64_t n) { return n >= kMinMusical && n <= kMaxMusical; }

// bpm = m / 2^s exactly, for a normal positive float.
void exact_value(float bpm, std::uint64_t& m, int& s) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &bpm, sizeof bits);
    std::uint64_t mant = (bits & 0x7FFFFFu) | 0x800000u;
    int e = static_cast<int>((bits >> 23) & 0xFFu) - 127 - 23;
    while ((mant & 1u) == 0) {
        mant >>= 1;
        ++e;
    }
    if (e >= 0) {
        m = mant << e;
        s = 0;
    } else {
        m = mant;
        s = -e;
    }
}

// The fast path holds while 2|x|p + q stays under 2^63.
std::uint64_t fast_limit(std::uint64_t p, std::uint64_t q) {
    return ((std::uint64_t{1} << 62) - q) / p;
}

}  // namespace

bool is_meter_denominator(std::uint32_t d) {
    return d >= 1 && d <= kMaxMeterDenominator && (d & (d - 1)) == 0;
}

float bpm_within_bounds(float bpm) {
    if (!(std::isfinite(bpm) && bpm > 0.0f)) return bpm;
    return std::min(kMaxBpm, std::max(kMinBpm, bpm));
}

std::uint32_t nearest_meter_denominator(std::uint32_t d) {
    if (d == 0) return 0;
    if (d >= kMaxMeterDenominator) return kMaxMeterDenominator;
    std::uint32_t low = 1;
    while (low * 2 <= d) low *= 2;
    return d - low < low * 2 - d ? low : low * 2;
}

std::string tempo_map_problem(std::uint32_t sample_rate, const std::vector<TempoEntry>& tempo,
                              std::string& where) {
    where.clear();
    if (sample_rate == 0) {
        where = "sample_rate";
        return "must be greater than 0";
    }
    if (sample_rate > kMaxSampleRate) {
        where = "sample_rate";
        return "must be at most " + std::to_string(kMaxSampleRate) +
               ", the highest rate musical time is exact at";
    }
    if (tempo.empty()) {
        where = "tempo";
        return "needs at least one entry";
    }
    for (std::size_t i = 0; i < tempo.size(); ++i) {
        const TempoEntry& t = tempo[i];
        where = "tempo[" + std::to_string(i) + "]";
        if (i == 0 && t.at != 0) return "the first entry must be at 0";
        if (t.at < 0 || t.at > kMaxInteger) return "at is out of range";
        if (i > 0 && t.at <= tempo[i - 1].at) {
            return "entries must be sorted by at, without duplicates";
        }
        if (!(std::isfinite(t.bpm) && t.bpm > 0.0f)) return "bpm must be a positive number";
        if (t.bpm < kMinBpm || t.bpm > kMaxBpm) return "bpm must be from 1 to 1000";
        if (t.meter[0] == 0 || t.meter[1] == 0) return "meter must be two counts of at least 1";
        if (t.meter[0] > 0xFFFF) return "meter must have at most 65535 beats to the bar";
        if (!is_meter_denominator(t.meter[1])) {
            return "the meter's beat must be a power of two from 1 to 64";
        }
    }
    where.clear();
    return {};
}

bool MusicalMap::build(std::uint32_t sample_rate, const std::vector<TempoEntry>& tempo,
                       std::string& why) {
    entries_.clear();
    reachable_ = 0;
    rate_ = 0;
    std::string where;
    why = tempo_map_problem(sample_rate, tempo, where);
    if (!why.empty()) {
        if (!where.empty()) why = where + ": " + why;
        return false;
    }
    rate_ = sample_rate;
    entries_.reserve(tempo.size());
    for (const TempoEntry& t : tempo) {
        Entry e;
        e.at = t.at;
        std::uint64_t m = 0;
        int s = 0;
        exact_value(t.bpm, m, s);
        // tau = bpm * Q / (60 R) = m Q / (2^s 60 R): under 2^54 over 2^48.
        std::uint64_t num = m * static_cast<std::uint64_t>(kTicksPerQuarter);
        std::uint64_t den = (std::uint64_t{1} << s) * 60u * sample_rate;
        const std::uint64_t g = std::gcd(num, den);
        e.num = num / g;
        e.den = den / g;
        e.fast_frames = fast_limit(e.num, e.den);
        e.fast_ticks = fast_limit(e.den, e.num);
        e.beats = t.meter[0];
        e.note = t.meter[1];
        e.beat_ticks = kTicksPerWhole / e.note;
        entries_.push_back(e);
    }

    reachable_ = 1;
    for (std::size_t k = 1; k < entries_.size(); ++k) {
        const Entry& p = entries_[k - 1];
        Entry& e = entries_[k];
        std::int64_t q = 0;
        if (!round_scaled(e.at - p.at, p.num, p.den, p.fast_frames, q) ||
            !in_musical_range(p.tick + q)) {
            break;
        }
        e.tick = p.tick + q;
        if (e.beats == p.beats && e.note == p.note) {
            // A tempo change alone carries the count on.
            e.bar = p.bar;
            e.bar_tick = p.bar_tick;
        } else {
            // A meter change starts a bar at its own tick. A bar it cuts
            // short, by more than half a display tick, is still a bar.
            const Ticks bar = static_cast<Ticks>(p.beats) * p.beat_ticks;
            const Ticks d = e.tick - p.bar_tick;
            const Ticks into = d % bar;
            e.bar = p.bar + d / bar + (into * 2 * kDisplayTicksPerBeat > p.beat_ticks ? 1 : 0);
            e.bar_tick = e.tick;
        }
        reachable_ = k + 1;
    }
    return true;
}

std::size_t MusicalMap::entry_at_frame(Frames f) const {
    const auto it = std::upper_bound(entries_.begin(), entries_.end(), f,
                                     [](Frames v, const Entry& e) { return v < e.at; });
    return it == entries_.begin() ? 0 : static_cast<std::size_t>(it - entries_.begin()) - 1;
}

std::size_t MusicalMap::entry_at_tick(Ticks t) const {
    const auto end = entries_.begin() + static_cast<std::ptrdiff_t>(reachable_);
    const auto it = std::upper_bound(entries_.begin(), end, t,
                                     [](Ticks v, const Entry& e) { return v < e.tick; });
    return it == entries_.begin() ? 0 : static_cast<std::size_t>(it - entries_.begin()) - 1;
}

namespace {

bool frame_to_tick(const MusicalMap::Entry& e, Frames f, Ticks& out) {
    std::int64_t q = 0;
    if (!round_scaled(f - e.at, e.num, e.den, e.fast_frames, q)) return false;
    const Ticks t = e.tick + q;
    if (!in_musical_range(t)) return false;
    out = t;
    return true;
}

bool tick_to_frame(const MusicalMap::Entry& e, Ticks t, Frames& out) {
    std::int64_t q = 0;
    if (!round_scaled(t - e.tick, e.den, e.num, e.fast_ticks, q)) return false;
    const Frames f = e.at + q;
    if (!in_musical_range(f)) return false;
    out = f;
    return true;
}

}  // namespace

bool MusicalMap::tick_at(Frames f, Ticks& out) const {
    if (entries_.empty() || !in_musical_range(f)) return false;
    const std::size_t k = entry_at_frame(f);
    return k < reachable_ && frame_to_tick(entries_[k], f, out);
}

bool MusicalMap::frame_at(Ticks t, Frames& out) const {
    if (entries_.empty() || !in_musical_range(t)) return false;
    return tick_to_frame(entries_[entry_at_tick(t)], t, out);
}

std::size_t MusicalMap::frames_to_ticks(const Frames* in, std::size_t n, Ticks* out) const {
    if (entries_.empty()) return 0;
    std::size_t k = 0;
    Frames last = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const Frames f = in[i];
        if (!in_musical_range(f)) return i;
        if (i == 0 || f < last) {
            k = entry_at_frame(f);
        } else {
            while (k + 1 < entries_.size() && entries_[k + 1].at <= f) ++k;
        }
        last = f;
        if (k >= reachable_ || !frame_to_tick(entries_[k], f, out[i])) return i;
    }
    return n;
}

std::size_t MusicalMap::ticks_to_frames(const Ticks* in, std::size_t n, Frames* out) const {
    if (entries_.empty()) return 0;
    std::size_t k = 0;
    Ticks last = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const Ticks t = in[i];
        if (!in_musical_range(t)) return i;
        if (i == 0 || t < last) {
            k = entry_at_tick(t);
        } else {
            while (k + 1 < reachable_ && entries_[k + 1].tick <= t) ++k;
        }
        last = t;
        if (!tick_to_frame(entries_[k], t, out[i])) return i;
    }
    return n;
}

BarsBeats MusicalMap::bars_beats_at(Ticks t) const {
    if (entries_.empty()) return {};
    const Entry& e = entries_[entry_at_tick(t)];
    const Ticks bar = static_cast<Ticks>(e.beats) * e.beat_ticks;
    const std::int64_t whole = floor_div(t - e.bar_tick, bar);
    const Ticks in = t - e.bar_tick - whole * bar;
    BarsBeats r;
    r.bar = e.bar + whole + 1;
    r.beat = in / e.beat_ticks + 1;
    r.tick = static_cast<int>(in % e.beat_ticks * kDisplayTicksPerBeat / e.beat_ticks);
    return r;
}

bool MusicalMap::tick_of(const BarsBeats& b, Ticks& out) const {
    constexpr std::int64_t kMaxBeat = std::int64_t{1} << 31;
    if (entries_.empty() || b.beat > kMaxBeat || b.beat < -kMaxBeat) return false;
    for (std::size_t k = 0; k < reachable_; ++k) {
        const Entry& e = entries_[k];
        const bool last = k + 1 == reachable_;
        // A bar that ended before this entry, and was not reached in the
        // one before: what is left of it is the entry's own tick.
        if (b.bar <= e.bar) {
            out = e.tick;
            return true;
        }
        const Ticks bar = static_cast<Ticks>(e.beats) * e.beat_ticks;
        const std::int64_t bars = b.bar - 1 - e.bar;
        if (bars > kMaxMusical / bar) {
            if (last) return false;
            continue;
        }
        const Ticks x = e.bar_tick + bars * bar + (b.beat - 1) * e.beat_ticks +
                        floor_div(2 * static_cast<std::int64_t>(b.tick) * e.beat_ticks +
                                      kDisplayTicksPerBeat,
                                  2 * kDisplayTicksPerBeat);
        if (x < e.tick) {
            out = e.tick;
            return true;
        }
        if (last || x < entries_[k + 1].tick) {
            if (!in_musical_range(x)) return false;
            out = x;
            return true;
        }
    }
    return false;
}

}  // namespace sonora::model
