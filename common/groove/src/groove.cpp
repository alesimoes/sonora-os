// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/groove/groove.h"

#include <algorithm>
#include <cmath>

namespace sonora::groove {

using model::Frames;
using model::MusicalMap;
using model::Ticks;

namespace {

// Where a tick sits on the grid of the tempo entry in force.
struct SlotPos {
    Ticks anchor = 0;       // tick of the entry in force
    double slot_ticks = 0;  // length of one slot
    long long slot = 0;     // nearest slot, counted from the anchor
    double frac = 0;        // (t - slot tick) / slot_ticks, within +-0.5
};

SlotPos slot_pos(const MusicalMap& map, Ticks t, std::size_t spb) {
    const auto& e = map.entries()[map.entry_at_tick(t)];
    SlotPos p;
    p.anchor = e.tick;
    p.slot_ticks = static_cast<double>(e.beat_ticks) / static_cast<double>(spb);
    const double rel = static_cast<double>(t - e.tick) / p.slot_ticks;
    p.slot = static_cast<long long>(std::floor(rel + 0.5));
    p.frac = rel - static_cast<double>(p.slot);
    return p;
}

std::size_t slot_in_beat(long long slot, std::size_t spb) {
    const long long n = static_cast<long long>(spb);
    return static_cast<std::size_t>(((slot % n) + n) % n);
}

bool template_problem(const GrooveTemplate& t, std::string& why) {
    if (t.slots_per_beat < 1 || t.slots_per_beat > kMaxGrooveSlots ||
        t.slots.size() != t.slots_per_beat) {
        why = "groove template needs 1 to 64 slots per beat, one entry each";
        return true;
    }
    for (const auto& s : t.slots) {
        if (!(std::fabs(s.offset) <= kMaxSlotOffset) || !(s.gain >= 0.0)) {
            why = "groove slot offset must be within half a slot and gain not negative";
            return true;
        }
    }
    return false;
}

double median(std::vector<double>& v) {
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

// The frame a region start goes to, or false when the map cannot say.
bool target_frame(const MusicalMap& map, const GrooveTemplate& tmpl, Frames start,
                  double strength, Frames& out) {
    Ticks t = 0;
    if (!map.tick_at(start, t)) return false;
    const SlotPos p = slot_pos(map, t, tmpl.slots_per_beat);
    const double off =
        tmpl.slots[slot_in_beat(p.slot, tmpl.slots_per_beat)].offset * strength / 100.0;
    const double want =
        static_cast<double>(p.anchor) + (static_cast<double>(p.slot) + off) * p.slot_ticks;
    return map.frame_at(static_cast<Ticks>(std::llround(want)), out);
}

}  // namespace

bool groove_swing(double percent, std::size_t slots_per_beat, GrooveTemplate& out,
                  std::string& why) {
    if (slots_per_beat < 2 || slots_per_beat > kMaxGrooveSlots || slots_per_beat % 2 != 0) {
        why = "swing needs an even number of slots per beat, 2 to 64";
        return false;
    }
    if (!(percent >= 50.0 && percent <= 75.0)) {
        why = "swing is 50 to 75 percent";
        return false;
    }
    GrooveTemplate t;
    t.slots_per_beat = slots_per_beat;
    t.slots.assign(slots_per_beat, GrooveSlot{});
    // A pair spans two slots; its second half starts at `percent` of it.
    const double delay = 2.0 * percent / 100.0 - 1.0;
    for (std::size_t i = 1; i < slots_per_beat; i += 2) t.slots[i].offset = delay;
    out = std::move(t);
    return true;
}

bool groove_from_onsets(const MusicalMap& map, const std::vector<Frames>& onsets,
                        std::size_t slots_per_beat, GrooveTemplate& out, std::string& why) {
    if (slots_per_beat < 1 || slots_per_beat > kMaxGrooveSlots) {
        why = "slots per beat is 1 to 64";
        return false;
    }
    if (map.empty()) {
        why = "no tempo map";
        return false;
    }
    if (onsets.empty()) {
        why = "the reference has no onsets";
        return false;
    }
    std::vector<std::vector<double>> per_slot(slots_per_beat);
    for (Frames f : onsets) {
        Ticks t = 0;
        if (!map.tick_at(f, t)) {
            why = "an onset is outside the tempo map's range";
            return false;
        }
        const SlotPos p = slot_pos(map, t, slots_per_beat);
        per_slot[slot_in_beat(p.slot, slots_per_beat)].push_back(p.frac);
    }
    GrooveTemplate g;
    g.slots_per_beat = slots_per_beat;
    g.slots.assign(slots_per_beat, GrooveSlot{});
    for (std::size_t i = 0; i < slots_per_beat; ++i) {
        if (!per_slot[i].empty()) g.slots[i].offset = median(per_slot[i]);
    }
    out = std::move(g);
    return true;
}

bool groove_moves(const MusicalMap& map, const GrooveTemplate& tmpl,
                  const std::vector<model::Region>& regions, double strength,
                  GrooveResult& out, std::string& why) {
    if (!(strength > 0.0 && strength <= 100.0)) {
        why = "groove strength is above 0 and at most 100 percent";
        return false;
    }
    if (regions.empty()) {
        why = "no regions to groove";
        return false;
    }
    if (map.empty()) {
        why = "no tempo map";
        return false;
    }
    if (template_problem(tmpl, why)) return false;

    std::vector<const model::Region*> order;
    for (const auto& r : regions) order.push_back(&r);
    std::sort(order.begin(), order.end(),
              [](const auto* a, const auto* b) { return a->position < b->position; });

    GrooveResult res;
    Frames prev_end = 0;  // end of the neighbour before, where it will be
    bool have_prev = false;
    for (std::size_t i = 0; i < order.size(); ++i) {
        const model::Region& r = *order[i];
        Frames target = r.position;
        const bool ok = target_frame(map, tmpl, r.position, strength, target);
        // The next region has not moved when this one is placed; if it moves
        // it is checked against this one's final end in turn.
        const bool clear_before = !have_prev || target >= prev_end;
        const bool clear_after =
            i + 1 >= order.size() || target + r.length <= order[i + 1]->position;
        Frames final_pos = r.position;
        if (ok && target != r.position) {
            if (target >= 0 && clear_before && clear_after) {
                res.moves.push_back(GrooveMove{r.id, target});
                final_pos = target;
            } else {
                ++res.skipped;
            }
        }
        prev_end = final_pos + r.length;
        have_prev = true;
    }
    out = std::move(res);
    return true;
}

}  // namespace sonora::groove
