// SPDX-License-Identifier: GPL-3.0-or-later
//
// Strip Silence as an edit: each quiet stretch becomes separations and a
// removal, all regions in one Compound so it is one undo step.
#include <algorithm>
#include <cmath>

#include "edit_controller.h"
#include "edit_controller_detail.h"
#include "strip_silence.h"

namespace sonora::ui {

using namespace model;

bool EditController::strip_silence(const PeakSource& peaks, const StripParams& params,
                                   std::string& why) {
    // The selected region, else every region the selected range covers whole.
    std::vector<std::pair<Id, const Region*>> targets;  // playlist, region
    if (sel_.region != kNoId) {
        const TrackEdit* t = nullptr;
        const Region* r = find_region(snapshot(), sel_.region, &t);
        if (r == nullptr) {
            why = "the selected region is gone";
            return false;
        }
        targets.emplace_back(t->active_playlist, r);
    } else if (sel_.has_range()) {
        Span e;
        if (!edit_range(e, "stripped", why)) return false;
        for (int i = 0; i < e.count; ++i) {
            const TrackEdit* t = track(e.first + i);
            const Playlist* p = active_playlist(t);
            if (p == nullptr) continue;
            for (const Region& r : p->regions) {
                if (r.position >= e.a && r.end() <= e.b) {
                    targets.emplace_back(t->active_playlist, &r);
                }
            }
        }
    } else {
        why = "select a region, or a range that covers whole regions, to strip";
        return false;
    }
    if (targets.empty()) {
        why = "the range covers no region whole; select the regions or drag a wider range";
        return false;
    }

    // A source whose overview is still being read would answer silence for
    // everything, and every region would look quiet.
    for (const auto& [playlist, r] : targets) {
        for (const SourceRef& c : r->channels) {
            if (!peaks.ready(c.source)) {
                why = "the waveform overview of " + r->name +
                      " is still being read; try again in a moment. Nothing was stripped.";
                return false;
            }
        }
    }

    const uint32_t rate = snapshot().settings.sample_rate;
    std::vector<Member> members;
    // Shuffle closes each stripped stretch: shifted after every split, right
    // to left so the coordinates stay the originals'.
    struct Closing {
        const Playlist* playlist;
        Frames from, to;
    };
    std::vector<Closing> closing;
    const bool shuffle = mode_ == EditMode::Shuffle;
    int whole_quiet = 0;
    for (const auto& [playlist, r] : targets) {
        const std::vector<SilentSpan> spans = find_silence(peaks, *r, rate, params);
        if (spans.empty()) continue;
        // A region quiet from end to end is left for Clear: stripping it
        // would leave nothing, and that is never what a threshold typo meant.
        if (spans.size() == 1 && spans[0].from == r->position && spans[0].to == r->end()) {
            ++whole_quiet;
            continue;
        }
        // Right to left, so the left part keeps the region's id.
        for (auto it = spans.rbegin(); it != spans.rend(); ++it) {
            Id gap = r->id;
            if (it->to < r->end()) {
                members.push_back(SplitRegion{playlist, r->id, it->to, doc_.reserve_id()});
            }
            if (it->from > r->position) {
                gap = doc_.reserve_id();
                members.push_back(SplitRegion{playlist, r->id, it->from, gap});
            }
            members.push_back(RemoveRegion{playlist, gap});
            if (shuffle) {
                const TrackEdit* owner = nullptr;
                find_region(snapshot(), r->id, &owner);
                closing.push_back({active(owner), it->from, it->to});
            }
        }
    }
    std::sort(closing.begin(), closing.end(),
              [](const Closing& a, const Closing& b) { return a.from > b.from; });
    for (const Closing& c : closing) {
        // Something must start at or after the stretch's end, or be cut there.
        const bool tail = std::any_of(c.playlist->regions.begin(), c.playlist->regions.end(),
                                      [&](const Region& o) { return o.end() > c.to; });
        if (tail) members.push_back(ShiftRegions{c.playlist->id, c.to, -(c.to - c.from)});
    }
    if (members.empty()) {
        const std::string level = std::to_string(static_cast<int>(std::lround(params.threshold_db)));
        if (whole_quiet > 0) {
            why = "everything selected is below " + level +
                  " dB; lower the threshold, or Clear it. Nothing was stripped.";
        } else {
            why = "no stretch of the selection stays below " + level + " dB for " +
                  std::to_string(static_cast<int>(std::lround(params.min_silence_ms))) +
                  " ms. Nothing was stripped.";
        }
        return false;
    }
    if (!submit_compound(std::move(members), "Strip Silence", "stripped", why)) return false;
    sel_.region = kNoId;
    return true;
}

}  // namespace sonora::ui
