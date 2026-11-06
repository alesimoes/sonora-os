// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/model/tempo_edit.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "sonora/model/musical_map.h"

namespace sonora::model {
namespace {

// Strong guarantee: `map` is replaced only by a map the format accepts.
bool commit(std::vector<TempoEntry>& map, std::vector<TempoEntry>&& next,
            std::uint32_t sample_rate, std::string& why) {
    std::string where;
    const std::string bad = tempo_map_problem(sample_rate, next, where);
    if (!bad.empty()) {
        why = where.empty() ? bad : where + ": " + bad;
        return false;
    }
    map = std::move(next);
    return true;
}

bool bpm_ok(double bpm, std::string& why) {
    if (std::isfinite(bpm) && bpm >= kMinBpm && bpm <= kMaxBpm) return true;
    why = "tempo goes from 1 to 1000 bpm";
    return false;
}

TempoEntry held(const TempoEntry& in_force, Frames at, float bpm) {
    TempoEntry e = in_force;
    e.at = at;
    e.bpm = bpm;
    return e;
}

}  // namespace

std::size_t tempo_entry_in_force(const std::vector<TempoEntry>& map, Frames at) {
    std::size_t k = 0;
    for (std::size_t i = 1; i < map.size() && map[i].at <= at; ++i) k = i;
    return k;
}

bool tempo_set_at(std::vector<TempoEntry>& map, std::uint32_t sample_rate, Frames at, float bpm,
                  std::string& why) {
    if (map.empty() || at < 0) {
        why = "there is no tempo map to change at that position";
        return false;
    }
    if (!bpm_ok(bpm, why)) return false;
    std::vector<TempoEntry> next = map;
    const std::size_t k = tempo_entry_in_force(next, at);
    if (next[k].at == at) {
        next[k].bpm = bpm;
    } else {
        next.insert(next.begin() + static_cast<std::ptrdiff_t>(k) + 1, held(next[k], at, bpm));
    }
    return commit(map, std::move(next), sample_rate, why);
}

bool tempo_remove_at(std::vector<TempoEntry>& map, std::uint32_t sample_rate, Frames at,
                     std::string& why) {
    std::vector<TempoEntry> next = map;
    const auto it = std::find_if(next.begin(), next.end(),
                                 [&](const TempoEntry& e) { return e.at == at; });
    if (it == next.end()) {
        why = "there is no tempo change at that position";
        return false;
    }
    if (it == next.begin()) {
        why = "the first tempo cannot be removed; set it instead";
        return false;
    }
    // A meter change that goes takes its tempo-only followers with it: they
    // fall back to the meter before it, as meter_set_near wrote them.
    const auto gone = it->meter;
    const auto prev = (it - 1)->meter;
    const auto first_after = next.erase(it);
    if (gone != prev) {
        for (auto j = first_after; j != next.end() && j->meter == gone; ++j) j->meter = prev;
    }
    return commit(map, std::move(next), sample_rate, why);
}

bool tempo_ramp(std::vector<TempoEntry>& map, std::uint32_t sample_rate, Frames from, Frames to,
                float bpm_from, float bpm_to, std::string& why) {
    if (map.empty() || from < 0 || to <= from) {
        why = "a ramp needs a range: select the stretch it runs over";
        return false;
    }
    if (!bpm_ok(bpm_from, why) || !bpm_ok(bpm_to, why)) return false;
    const TempoEntry in_force = map[tempo_entry_in_force(map, from)];
    for (const TempoEntry& e : map) {
        if (e.at > from && e.at < to && e.meter != in_force.meter) {
            why = "the range holds a meter change; ramp each side of it";
            return false;
        }
    }
    const double beats = static_cast<double>(to - from) / sample_rate *
                         (0.5 * (static_cast<double>(bpm_from) + bpm_to)) / 60.0;
    std::size_t n = static_cast<std::size_t>(std::llround(beats * 2.0));
    n = std::min<std::size_t>({std::max<std::size_t>(n, 2), kMaxRampSteps,
                               static_cast<std::size_t>(to - from)});
    if (n < 2) {
        why = "the range is too short to ramp over";
        return false;
    }

    std::vector<TempoEntry> next;
    for (const TempoEntry& e : map)
        if (e.at < from) next.push_back(e);
    const std::int64_t span = to - from;
    for (std::size_t i = 0; i < n; ++i) {
        const double x = static_cast<double>(i) / static_cast<double>(n - 1);
        next.push_back(held(in_force,
                            from + span * static_cast<std::int64_t>(i) /
                                       static_cast<std::int64_t>(n),
                            static_cast<float>(bpm_from + (static_cast<double>(bpm_to) - bpm_from) * x)));
    }
    // The ramp's end is held: a change at `to` takes the end tempo, else one
    // is made there.
    const bool has_to = std::any_of(map.begin(), map.end(),
                                    [&](const TempoEntry& e) { return e.at == to; });
    if (!has_to) next.push_back(held(in_force, to, bpm_to));
    for (const TempoEntry& e : map) {
        if (e.at < to) continue;
        TempoEntry c = e;
        if (e.at == to) c.bpm = bpm_to;
        next.push_back(c);
    }
    return commit(map, std::move(next), sample_rate, why);
}

bool tempo_scale(std::vector<TempoEntry>& map, std::uint32_t sample_rate, Frames from, Frames to,
                 double factor, std::string& why) {
    if (map.empty() || from < 0 || to <= from) {
        why = "scaling needs a range: select the stretch to change";
        return false;
    }
    if (!(std::isfinite(factor) && factor > 0.0)) {
        why = "the factor must be a positive number";
        return false;
    }
    const TempoEntry at_from = map[tempo_entry_in_force(map, from)];
    const TempoEntry at_to = map[tempo_entry_in_force(map, to)];
    const auto has = [&](Frames f) {
        return std::any_of(map.begin(), map.end(), [&](const TempoEntry& e) { return e.at == f; });
    };
    std::vector<TempoEntry> next;
    for (const TempoEntry& e : map)
        if (e.at < from) next.push_back(e);
    if (!has(from)) {
        const float bpm = static_cast<float>(at_from.bpm * factor);
        if (!bpm_ok(bpm, why)) return false;
        next.push_back(held(at_from, from, bpm));
    }
    for (const TempoEntry& e : map) {
        if (e.at < from || e.at >= to) continue;
        const float bpm = static_cast<float>(e.bpm * factor);
        if (!bpm_ok(bpm, why)) return false;
        next.push_back(held(e, e.at, bpm));
    }
    if (!has(to)) next.push_back(held(at_to, to, at_to.bpm));
    for (const TempoEntry& e : map)
        if (e.at >= to) next.push_back(e);
    return commit(map, std::move(next), sample_rate, why);
}

bool nearest_bar_line(const std::vector<TempoEntry>& map, std::uint32_t sample_rate, Frames near,
                      Frames& out) {
    MusicalMap m;
    std::string why;
    if (!m.build(sample_rate, map, why)) return false;
    const Frames f = std::max<Frames>(0, near);
    Ticks t = 0;
    if (!m.tick_at(f, t)) return false;
    const BarsBeats b = m.bars_beats_at(t);
    Ticks lo = 0, hi = 0;
    Frames flo = 0, fhi = 0;
    if (!m.tick_of({b.bar, 1, 0}, lo) || !m.tick_of({b.bar + 1, 1, 0}, hi)) return false;
    if (!m.frame_at(lo, flo) || !m.frame_at(hi, fhi)) return false;
    out = (f - flo <= fhi - f) ? flo : fhi;
    return true;
}

bool meter_set_near(std::vector<TempoEntry>& map, std::uint32_t sample_rate, Frames near,
                    std::uint32_t beats, std::uint32_t note, Frames& at, std::string& why) {
    if (beats == 0 || beats > 0xFFFF || !is_meter_denominator(note)) {
        why = "a meter is 1 to 65535 beats over 1, 2, 4, 8, 16, 32 or 64";
        return false;
    }
    Frames bar = 0;
    if (!nearest_bar_line(map, sample_rate, near, bar)) {
        why = "that position is outside the range musical time covers";
        return false;
    }
    std::vector<TempoEntry> next = map;
    std::size_t k = tempo_entry_in_force(next, bar);
    const std::array<std::uint32_t, 2> old_meter = next[k].meter;
    if (next[k].at != bar) {
        next.insert(next.begin() + static_cast<std::ptrdiff_t>(k) + 1,
                    held(next[k], bar, next[k].bpm));
        ++k;
    }
    // The change holds until the next meter change: the tempo-only entries
    // after it (a ramp's steps) carry the new meter, or the old one would
    // come back at the first of them and start a bar there.
    for (std::size_t j = k; j < next.size() && next[j].meter == old_meter; ++j) {
        next[j].meter = {{beats, note}};
    }
    if (!commit(map, std::move(next), sample_rate, why)) return false;
    at = bar;
    return true;
}

}  // namespace sonora::model
