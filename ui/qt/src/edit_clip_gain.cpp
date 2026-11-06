// SPDX-License-Identifier: GPL-3.0-or-later
//
// Clip gain (Pro Tools' Clip Gain, Logic's region gain): the level of the
// selected region and the line of breakpoints on it. Each change is one
// SetRegionGain and one undo step; the engine plays the result and a render
// bakes it into a new file (engine/control/consolidate.h).
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "edit_controller.h"

namespace sonora::ui {

using namespace model;

namespace {

std::string db_text(float db) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%+.1f dB", static_cast<double>(db));
    return buf;
}

// The line's level at source frame `src`: linear in amplitude between its
// points, as the engine plays it, held outside them. 0 dB with no line.
float line_db_at(const std::vector<GainPoint>& line, Frames src) {
    if (line.empty()) return 0.0f;
    if (src <= line.front().at) return line.front().db;
    if (src >= line.back().at) return line.back().db;
    std::size_t hi = 1;
    while (line[hi].at <= src) ++hi;
    const GainPoint& a = line[hi - 1];
    const GainPoint& b = line[hi];
    const double ga = std::pow(10.0, a.db / 20.0), gb = std::pow(10.0, b.db / 20.0);
    const double x = static_cast<double>(src - a.at) / static_cast<double>(b.at - a.at);
    return static_cast<float>(20.0 * std::log10(ga + x * (gb - ga)));
}

}  // namespace

float EditController::clip_line_db_at(const Region& r, Frames timeline_frame) {
    return line_db_at(r.gain_line, r.source_offset + (timeline_frame - r.position));
}

bool EditController::set_clip_gain(float db, std::string& why) {
    const TrackEdit* t = nullptr;
    if (selected_region(why) == nullptr) return false;
    const Region* r = find_region(snapshot(), sel_.region, &t);
    if (!(db >= kMinGainDb && db <= kMaxGainDb)) {
        why = "clip gain goes from " + std::to_string(static_cast<int>(kMinGainDb)) + " to +" +
              std::to_string(static_cast<int>(kMaxGainDb)) + " dB";
        return false;
    }
    if (r->gain_db == db) return true;
    return submit(SetRegionGain{t->active_playlist, r->id, db, r->gain_line}, why);
}

bool EditController::nudge_clip_gain(float delta_db, std::string& why) {
    if (selected_region(why) == nullptr) return false;
    const Region* r = find_region(snapshot(), sel_.region);
    const float to = std::clamp(r->gain_db + delta_db, kMinGainDb, kMaxGainDb);
    if (to == r->gain_db) {
        why = "the clip is already at " + db_text(r->gain_db) + ", the limit of clip gain";
        return false;
    }
    return set_clip_gain(to, why);
}

bool EditController::set_gain_point(Frames at, float db, std::string& why) {
    const TrackEdit* t = nullptr;
    if (selected_region(why) == nullptr) return false;
    const Region* r = find_region(snapshot(), sel_.region, &t);
    if (at < r->position || at >= r->end()) {
        why = "the insertion point is outside the selected region; click inside it first";
        return false;
    }
    if (!(db >= kMinGainDb && db <= kMaxGainDb)) {
        why = "a gain point goes from " + std::to_string(static_cast<int>(kMinGainDb)) + " to +" +
              std::to_string(static_cast<int>(kMaxGainDb)) + " dB";
        return false;
    }
    std::vector<GainPoint> line = r->gain_line;
    const Frames src = r->source_offset + (at - r->position);
    if (line.empty()) {
        // The first point is a dip or a lift: the clip stays as it was at both
        // ends, as Pro Tools' first Clip Gain breakpoint leaves it.
        line.push_back({r->source_offset, 0.0f});
        line.push_back({r->source_offset + r->length, 0.0f});
    }
    const auto next = std::lower_bound(line.begin(), line.end(), src,
                                       [](const GainPoint& p, Frames f) { return p.at < f; });
    if (next != line.end() && next->at == src) {
        next->db = db;
    } else {
        if (line.size() >= kMaxGainPoints) {
            why = "a clip's gain line holds at most " + std::to_string(kMaxGainPoints) + " points";
            return false;
        }
        line.insert(next, GainPoint{src, db});
    }
    return submit(SetRegionGain{t->active_playlist, r->id, r->gain_db, std::move(line)}, why);
}

bool EditController::remove_gain_point(Frames at, std::string& why) {
    const TrackEdit* t = nullptr;
    if (selected_region(why) == nullptr) return false;
    const Region* r = find_region(snapshot(), sel_.region, &t);
    if (r->gain_line.empty()) {
        why = "the selected clip has no gain line";
        return false;
    }
    // The nearest point to `at`, within a tenth of a second of source.
    const Frames src = r->source_offset + (at - r->position);
    const Frames reach = std::max<Frames>(1, snapshot().settings.sample_rate / 10);
    std::size_t best = r->gain_line.size();
    Frames best_d = reach + 1;
    for (std::size_t i = 0; i < r->gain_line.size(); ++i) {
        const Frames d = std::llabs(r->gain_line[i].at - src);
        if (d < best_d) {
            best = i;
            best_d = d;
        }
    }
    if (best == r->gain_line.size()) {
        why = "there is no gain point at the insertion point";
        return false;
    }
    std::vector<GainPoint> line = r->gain_line;
    line.erase(line.begin() + static_cast<std::ptrdiff_t>(best));
    return submit(SetRegionGain{t->active_playlist, r->id, r->gain_db, std::move(line)}, why);
}

bool EditController::clear_gain_line(std::string& why) {
    const TrackEdit* t = nullptr;
    if (selected_region(why) == nullptr) return false;
    const Region* r = find_region(snapshot(), sel_.region, &t);
    if (r->gain_line.empty()) return true;
    return submit(SetRegionGain{t->active_playlist, r->id, r->gain_db, {}}, why);
}

}  // namespace sonora::ui
