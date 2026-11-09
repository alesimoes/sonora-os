// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/stretch/quantize.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace sonora::stretch {

using model::Frames;
using model::Ticks;

namespace {

// Frame of the grid line nearest to the session frame `f`; false if out of range.
bool nearest_line(const model::MusicalMap& map, double grid_beats, Frames f, Frames& out) {
    Ticks t = 0;
    if (!map.tick_at(f, t)) return false;
    const auto& e = map.entries()[map.entry_at_tick(t)];
    const double slot = static_cast<double>(e.beat_ticks) * grid_beats;
    if (slot < 1.0) return false;
    const double k = std::floor(static_cast<double>(t - e.tick) / slot + 0.5);
    const Ticks line = e.tick + static_cast<Ticks>(std::llround(k * slot));
    return map.frame_at(line, out);
}

}  // namespace

bool quantize_plan(const QuantizeRequest& req, const model::MusicalMap& map, WarpMap& out,
                   std::string& why) {
    if (map.empty()) {
        why = "the tempo map is empty";
        return false;
    }
    if (!(req.grid_beats > 0.0) || !std::isfinite(req.grid_beats)) {
        why = "the grid must be a positive number of beats";
        return false;
    }
    if (req.strength == 0 || req.strength > 100) {
        why = req.strength == 0 ? "strength 0 moves nothing" : "strength is over 100";
        return false;
    }
    if (req.region_length < 1) {
        why = "the region is empty";
        return false;
    }
    std::vector<std::int64_t> onsets = req.onsets;
    std::sort(onsets.begin(), onsets.end());
    std::vector<Marker> markers;
    Marker prev{0, 0};
    for (std::int64_t src : onsets) {
        if (src <= prev.source || src >= req.region_length) continue;
        Frames line = 0;
        if (!nearest_line(map, req.grid_beats, req.region_start + src, line)) {
            why = "an onset is outside the tempo map's range";
            return false;
        }
        const double moved = static_cast<double>(line - req.region_start - src) *
                             static_cast<double>(req.strength) / 100.0;
        const std::int64_t dst = src + static_cast<std::int64_t>(std::llround(moved));
        if (dst <= prev.target) continue;
        prev = Marker{src, dst};
        markers.push_back(prev);
    }
    if (markers.empty()) {
        why = "no onsets inside the region";
        return false;
    }
    WarpMap m;
    if (!m.set(req.region_length, std::move(markers), why)) return false;
    out = std::move(m);
    return true;
}

}  // namespace sonora::stretch
