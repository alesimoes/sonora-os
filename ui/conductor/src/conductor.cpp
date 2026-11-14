// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/ui_conductor/conductor.h"

#include <cmath>

#include "sonora/model/musical_map.h"
#include "sonora/model/tempo_edit.h"

namespace sonora::ui::conductor {

using model::TempoEntry;

std::optional<std::size_t> hit_test(const std::vector<TempoEntry>& map, std::uint32_t sample_rate,
                                    double frames_per_pixel, double x) {
    if (map.empty() || sample_rate == 0 || !(frames_per_pixel > 0.0) || !std::isfinite(x)) {
        return std::nullopt;
    }
    std::optional<std::size_t> best;
    double best_d = kHitSlopPixels;
    for (std::size_t i = 0; i < map.size(); ++i) {
        const double d = std::fabs(static_cast<double>(map[i].at) / frames_per_pixel - x);
        // Strictly nearer wins; on a tie the earlier mark keeps it.
        if (d <= best_d && (!best || d < best_d)) {
            best = i;
            best_d = d;
        }
    }
    return best;
}

TempoDrag drag_tempo(const std::vector<TempoEntry>& map, std::size_t index, double dy_pixels,
                     bool whole_bpm) {
    TempoDrag r;
    if (index >= map.size() || !std::isfinite(dy_pixels)) {
        r.why = "there is no tempo mark to drag";
        return r;
    }
    double bpm = static_cast<double>(map[index].bpm) + dy_pixels * kBpmPerPixel;
    bpm = whole_bpm ? std::round(bpm) : std::round(bpm * 100.0) / 100.0;
    // Clamped after snapping so a bound is never snapped past.
    bpm = std::min<double>(std::max<double>(bpm, model::kMinBpm), model::kMaxBpm);
    r.bpm = static_cast<float>(bpm);
    r.ok = true;
    return r;
}

bool apply_tempo_drag(std::vector<TempoEntry>& map, std::uint32_t sample_rate, std::size_t index,
                      double dy_pixels, bool whole_bpm, std::string& why) {
    const TempoDrag d = drag_tempo(map, index, dy_pixels, whole_bpm);
    if (!d.ok) {
        why = d.why;
        return false;
    }
    return model::tempo_set_at(map, sample_rate, map[index].at, d.bpm, why);
}

bool remove_mark(std::vector<TempoEntry>& map, std::uint32_t sample_rate, std::size_t index,
                 std::string& why) {
    if (index >= map.size()) {
        why = "there is no tempo mark to remove";
        return false;
    }
    return model::tempo_remove_at(map, sample_rate, map[index].at, why);
}

bool apply_meter(std::vector<TempoEntry>& map, std::uint32_t sample_rate, model::Frames near,
                 std::uint32_t beats, std::uint32_t note, model::Frames& at, std::string& why) {
    return model::meter_set_near(map, sample_rate, near, beats, note, at, why);
}

MeterChoices meter_choices() {
    MeterChoices c;
    for (std::uint32_t b = 1; b <= 32; ++b) c.beats.push_back(b);
    c.notes = {1, 2, 4, 8, 16};
    return c;
}

}  // namespace sonora::ui::conductor
