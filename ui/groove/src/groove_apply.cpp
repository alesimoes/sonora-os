// SPDX-License-Identifier: GPL-3.0-or-later
//
// Groove as an edit (ADR-0115): every target region's move in one Compound,
// so one Undo puts them all back. groove_moves() only knows the regions it
// is given; the regions of the same playlist that are not grooved are
// checked here, since MoveRegion overwrites what it lands on.
#include <algorithm>
#include <map>
#include <utility>
#include <vector>

#include "edit_controller.h"
#include "groove_detail.h"
#include "model_access.h"
#include "sonora/model/musical_map.h"
#include "sonora/model/operations.h"

namespace sonora::ui {

using model::Frames;
using model::Id;
using model::Region;

namespace {

struct PlaylistTargets {
    Id playlist = 0;
    const model::Playlist* list = nullptr;
    std::vector<Region> regions;
};

bool selected_region(const EditController& ec, std::vector<PlaylistTargets>& out,
                     std::size_t& grouped, std::string& why) {
    const model::TrackEdit* t = nullptr;
    const Region* r = find_region(ec.snapshot(), ec.selection().region, &t);
    const model::Playlist* p = t != nullptr ? active_playlist(t) : nullptr;
    if (r == nullptr || p == nullptr) {
        why = "the selected region is gone";
        return false;
    }
    if (r->clip_group != 0) {
        ++grouped;
        return true;
    }
    out.push_back(PlaylistTargets{t->active_playlist, p, {*r}});
    return true;
}

// The regions that start inside the selected range, per playlist.
bool range_regions(const EditController& ec, groove_detail::Targets which,
                   std::vector<PlaylistTargets>& out, std::size_t& grouped, std::string& why) {
    const Selection& sel = ec.selection();
    const bool below = which == groove_detail::Targets::BelowReference;
    if (!sel.has_range()) {
        why = below ? "select a range from the reference track down over the tracks to groove"
                    : "select the regions to groove: a region, or a range over them";
        return false;
    }
    const auto& tracks = ec.snapshot().edit->tracks;
    const int first = below ? sel.track + 1 : sel.track;
    const int last = std::min(sel.track + sel.tracks, static_cast<int>(tracks.size()));
    if (first >= last) {
        why = "select a range from the reference track down over at least one more track: "
              "the top one is the reference, the ones below it are grooved";
        return false;
    }
    for (int i = std::max(first, 0); i < last; ++i) {
        const model::TrackEdit* t = tracks[static_cast<std::size_t>(i)].get();
        const model::Playlist* p = active_playlist(t);
        if (p == nullptr) continue;
        PlaylistTargets pt{t->active_playlist, p, {}};
        for (const Region& r : p->regions) {
            if (r.position < sel.start || r.position >= sel.end) continue;
            if (r.clip_group != 0) {
                ++grouped;
                continue;
            }
            pt.regions.push_back(r);
        }
        if (!pt.regions.empty()) out.push_back(std::move(pt));
    }
    return true;
}

// Drops every move that would land on another region of the playlist where
// that one ends up, until none does: dropping one keeps its region where it
// was, which can block another move in turn.
std::size_t drop_overlaps(const model::Playlist& p, std::vector<groove::GrooveMove>& moves) {
    std::map<Id, Frames> to;
    for (const auto& m : moves) to[m.region] = m.position;
    std::map<Id, const Region*> by_id;
    for (const Region& r : p.regions) by_id[r.id] = &r;
    std::size_t dropped = 0;
    for (bool changed = true; changed;) {
        changed = false;
        for (auto it = moves.begin(); it != moves.end();) {
            const Frames a = it->position, b = a + by_id[it->region]->length;
            bool clash = false;
            for (const Region& o : p.regions) {
                if (o.id == it->region) continue;
                const auto f = to.find(o.id);
                const Frames oa = f != to.end() ? f->second : o.position;
                if (a < oa + o.length && oa < b) {
                    clash = true;
                    break;
                }
            }
            if (!clash) {
                ++it;
                continue;
            }
            to.erase(it->region);
            it = moves.erase(it);
            ++dropped;
            changed = true;
        }
    }
    return dropped;
}

std::string plural(std::size_t n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

std::string report_text(const GrooveReport& r) {
    std::string s = "Groove moved " + plural(r.moved, "region", "regions");
    if (r.overlapping > 0) {
        s += "; " + plural(r.overlapping, "was", "were") +
             " left in place because moving would overlap a neighbour";
    }
    if (r.grouped > 0) {
        s += "; " + plural(r.grouped, "clip in a group was", "clips in groups were") +
             " left in place (ungroup to groove them)";
    }
    return s;
}

}  // namespace

namespace groove_detail {

bool apply(EditController& ec, const groove::GrooveTemplate& tmpl, double strength,
           Targets which, GrooveReport& report, std::string& why) {
    report = GrooveReport{};
    if (ec.mode() == EditMode::Shuffle) {
        why = "Groove moves regions apart, which Shuffle would close up again; "
              "switch to Slip or Grid. Nothing was moved.";
        return false;
    }
    std::vector<PlaylistTargets> targets;
    const bool one = which == Targets::Selection && ec.selection().region != kNoId;
    if (!(one ? selected_region(ec, targets, report.grouped, why)
              : range_regions(ec, which, targets, report.grouped, why))) {
        return false;
    }
    if (targets.empty()) {
        why = report.grouped > 0
                  ? "every selected region is in a clip group, which moves as one; ungroup "
                    "them to groove them. Nothing was moved."
                  : "no region starts inside the selected range. Nothing was moved.";
        return false;
    }
    model::MusicalMap map;
    if (!map.build(ec.snapshot().settings, why)) return false;

    std::vector<model::Member> members;
    for (const PlaylistTargets& pt : targets) {
        groove::GrooveResult res;
        if (!groove::groove_moves(map, tmpl, pt.regions, strength, res, why)) return false;
        report.overlapping += res.skipped + drop_overlaps(*pt.list, res.moves);
        for (const auto& m : res.moves) {
            members.push_back(model::MoveRegion{pt.playlist, m.region, m.position, kNoId});
        }
    }
    report.moved = members.size();
    report.message = report_text(report);
    if (members.empty()) {
        why = report.overlapping > 0 ? report.message + ". Nothing was moved."
                                     : "every selected region already sits on the groove. "
                                       "Nothing was moved.";
        return false;
    }
    if (members.size() > model::kMaxCompoundMembers) {
        why = "Groove would move " + std::to_string(members.size()) +
              " regions at once; select at most " + std::to_string(model::kMaxCompoundMembers) +
              ". Nothing was moved.";
        return false;
    }
    // Through the document: the controller's own Compound helpers are not
    // public, and a groove removes no region the selection could point at.
    return ec.document().submit(model::Operation{model::Compound{std::move(members), "Groove"}},
                                why);
}

}  // namespace groove_detail

bool groove_apply(EditController& ec, const groove::GrooveTemplate& tmpl, double strength,
                  GrooveReport& report, std::string& why) {
    return groove_detail::apply(ec, tmpl, strength, groove_detail::Targets::Selection, report,
                                why);
}

}  // namespace sonora::ui
