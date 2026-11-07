// SPDX-License-Identifier: GPL-3.0-or-later
//
// Region fades and crossfades under the Smart Tool and by command.
#include "edit_controller.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "sonora/fade_curve.h"
#include "edit_controller_detail.h"

namespace sonora::ui {

using namespace model;
using namespace edit_detail;

// --- fades ---

bool EditController::set_region_fade(Id region, FadeEnd end, Frames length, std::string& why) {
    const TrackEdit* t = nullptr;
    const Region* r = find_region(snapshot(), region, &t);
    if (r == nullptr) {
        why = "the region is gone";
        return false;
    }
    const Playlist& p = *active(t);
    const RegionFades f = region_fades(p, *r);
    const bool in = end == FadeEnd::In;
    if (const Crossfade* x = in ? f.in_crossfade : f.out_crossfade) {
        why = crossfaded_refusal(p, *r, *x, in);
        return false;
    }
    const Frames room = r->length - (in ? taken_at_end(*r, f) : taken_at_start(*r, f));
    if (length < 0 || length > room) {
        const char* self = in ? "in" : "out";
        const char* other = in ? "out" : "in";
        why = std::string("a fade-") + self + " that long would run into " + named(*r) +
              "'s fade-" + other;
        const Crossfade* hiding = in ? f.out_crossfade : f.in_crossfade;
        const Frames stored = in ? r->fade_out.length : r->fade_in.length;
        const Frames heard = in ? reach_out(f) : reach_in(f);
        if (hiding != nullptr && length >= 0 && length <= r->length - heard && stored > heard) {
            // The limit is a fade nobody can see: say where it is and how to
            // reach it, rather than name a fade that is not drawn.
            const Region* n = region_on(p, in ? hiding->right : hiding->left);
            why += std::string(", kept under its crossfade with ") +
                   (n ? named(*n) : std::string("its neighbour")) +
                   "; clear the crossfade, then that fade-" + other + ", to make room";
        }
        return false;
    }
    const Fade& now = in ? r->fade_in : r->fade_out;
    if (now.length == length) return true;
    // A fade being reshaped in length keeps its curve; a new one takes the
    // last one used.
    const Fade fade{length, now.length > 0 ? now.shape : fade_shape_};
    return submit(SetFade{p.id, r->id, end, fade}, why);
}

bool EditController::fade_to_start(std::string& why) {
    if (refuse_several("Fade to Start", why)) return false;
    int index = -1;
    const Region* r = target_region(index);
    if (r == nullptr || sel_.insertion <= r->position) {
        why = "the insertion point is not inside a region on the selected track";
        return false;
    }
    return set_region_fade(r->id, FadeEnd::In, sel_.insertion - r->position, why);
}

bool EditController::fade_to_end(std::string& why) {
    if (refuse_several("Fade to End", why)) return false;
    int index = -1;
    const Region* r = target_region(index);
    if (r == nullptr || sel_.insertion <= r->position) {
        why = "the insertion point is not inside a region on the selected track";
        return false;
    }
    return set_region_fade(r->id, FadeEnd::Out, r->end() - sel_.insertion, why);
}

bool EditController::fade_selection(std::string& why) {
    if (refuse_several("Fading a selection", why)) return false;
    const TrackEdit* t = track(sel_.track);
    if (t == nullptr) {
        why = "select a track first";
        return false;
    }
    if (!sel_.has_range()) {
        why = "select a time range across a region's start, its end, or the cut between two "
              "regions";
        return false;
    }
    const Playlist& p = *active(t);
    const Frames a = sel_.start, b = sel_.end;

    // Cuts: two regions that butt, inside the range or at either end of it.
    std::vector<std::size_t> cuts;  // the left region's index
    for (std::size_t i = 0; i + 1 < p.regions.size(); ++i) {
        const Frames cut = p.regions[i].end();
        if (cut == p.regions[i + 1].position && cut >= a && cut <= b) cuts.push_back(i);
    }
    if (cuts.size() > 1) {
        why = "the selection crosses " + std::to_string(cuts.size()) +
              " cuts; batch fades are not built yet, so select across one";
        return false;
    }
    if (cuts.size() == 1) {
        const Region& l = p.regions[cuts[0]];
        const Region& r = p.regions[cuts[0] + 1];
        const Frames cut = l.end();
        Crossfade x;
        x.left = l.id;
        x.right = r.id;
        x.length = b - a;
        x.shape = crossfade_shape_;
        // Pre and post when the range stops at the cut, as Pro Tools does.
        // Otherwise centred on the cut: the model has no other alignment, so
        // a lopsided range becomes a centred crossfade of its length, and
        // the selection is redrawn to show exactly what was made.
        x.align = b == cut ? Align::Pre : a == cut ? Align::Post : Align::Centre;
        // Short handles shorten it to the audio there is, keeping that
        // placement, as Pro Tools' Adjust Bounds does; the selection below
        // shows what was made, and the note says why (ADR-0021).
        Frames handle_before = 0, handle_after = 0;
        crossfade_handles(l, r, *snapshot().sources, handle_before, handle_after);
        const Crossfade asked = x;
        if (!fit_crossfade(x, handle_before, handle_after)) {
            why = no_handle_refusal(l, r, x, handle_before, handle_after);
            return false;
        }
        Frames before = 0, after = 0;
        crossfade_extent(x, before, after);
        // Not into the fade at either region's other end. The model refuses
        // two crossfades that meet inside a region but not a crossfade over
        // the region's own fade, which the engine would multiply: two fades
        // at once over the overlap, heard as a dip nobody drew. Refused, not
        // shortened, as the model refuses a missing handle. A fade kept
        // under a crossfade at the other end counts too: clearing that
        // crossfade brings it back, over this one.
        const RegionFades lf = region_fades(p, l), rf = region_fades(p, r);
        const bool left = before > l.length - taken_at_start(l, lf);
        if (left || after > r.length - taken_at_end(r, rf)) {
            const Region& n = left ? l : r;
            const Crossfade* other = left ? lf.in_crossfade : rf.out_crossfade;
            const Frames heard = left ? reach_in(lf) : reach_out(rf);
            const Frames stored = left ? l.fade_in.length : r.fade_out.length;
            const std::string fade = named(n) + "'s fade-" + (left ? "in" : "out");
            const char* end = left ? "start" : "end";
            if (other != nullptr && stored > heard &&
                (left ? before <= l.length - heard : after <= r.length - heard)) {
                // The limit is a fade nobody can see: say where it is.
                const Region* m = region_on(p, left ? other->left : other->right);
                why = "the crossfade would overlap " + fade + ", kept under its crossfade with " +
                      (m ? named(*m) : std::string("its neighbour")) +
                      "; clear that crossfade, then that fade-" + (left ? "in" : "out") +
                      ", or select less of " + named(n);
            } else {
                why = "the crossfade would overlap " +
                      (other != nullptr ? "the crossfade at " + named(n) + "'s " + end : fade) +
                      "; shorten that first, or select less of " + named(n);
            }
            return false;
        }
        // Before the submit: l and r live in the snapshot it replaces.
        std::string note;
        if (!(x == asked)) note = shortened_note(l, r, asked, x, handle_before, handle_after);
        // The fades at the cut go with the crossfade, as in Pro Tools: kept
        // under it, nobody can see them, yet they come back on Clear and
        // still take room from a fade at the regions' other ends. One
        // compound, so the gesture stays one undo step that brings them back.
        std::vector<Member> members;
        if (l.fade_out.length > 0) members.push_back(SetFade{p.id, l.id, FadeEnd::Out, Fade{}});
        if (r.fade_in.length > 0) members.push_back(SetFade{p.id, r.id, FadeEnd::In, Fade{}});
        if (members.empty()) {
            if (!submit(SetCrossfade{p.id, x}, why)) return false;
        } else {
            members.push_back(SetCrossfade{p.id, x});
            if (!submit_compound(std::move(members), "Crossfade", "crossfaded", why)) return false;
        }
        sel_.region = kNoId;
        sel_.start = sel_.insertion = cut - before;
        sel_.end = cut + after;
        why = note;
        return true;
    }

    std::vector<const Region*> under;
    for (const Region& r : p.regions) {
        if (r.position < b && r.end() > a) under.push_back(&r);
    }
    if (under.empty()) {
        why = "there is no region in the selection on " + t->name;
        return false;
    }
    if (under.size() > 1) {
        why = "the selection covers " + std::to_string(under.size()) +
              " regions; batch fades are not built yet, so select across one region's start "
              "or end";
        return false;
    }
    const Region& r = *under[0];
    const bool has_start = a <= r.position, has_end = b >= r.end();
    if (has_start && has_end) {
        why = "the selection covers the whole of " + named(r) +
              "; select across its start for a fade-in or its end for a fade-out";
        return false;
    }
    if (!has_start && !has_end) {
        why = "a fade needs the selection to include " + named(r) +
              "'s start or end, or a cut between two regions";
        return false;
    }
    const Frames length = has_start ? b - r.position : r.end() - a;
    const Frames from = has_start ? r.position : a;
    if (!set_region_fade(r.id, has_start ? FadeEnd::In : FadeEnd::Out, length, why)) return false;
    sel_.region = kNoId;
    sel_.start = sel_.insertion = from;
    sel_.end = from + length;
    return true;
}

bool EditController::clear_fade_at_selection(bool& done, std::string& why) {
    done = false;
    const TrackEdit* t = track(sel_.track);
    // A fade is on one track; on several, Clear clears the range.
    if (t == nullptr || sel_.region != kNoId || !sel_.has_range() || span() > 1) return true;
    const Playlist& p = *active(t);
    for (const Crossfade& x : p.crossfades) {
        const Region* r = region_on(p, x.right);
        if (r == nullptr) continue;
        Frames before = 0, after = 0;
        crossfade_extent(x, before, after);
        if (sel_.start == r->position - before && sel_.end == r->position + after) {
            done = true;
            // Removing it brings back any fade a region keeps under it: none
            // for a crossfade made here, which drops them, but a document
            // from elsewhere or an older build can have them. One that would run into what is at its region's other end
            // comes back shortened to meet it: the model does that
            // (RemoveCrossfade, PR #69), because the operator cannot see
            // or edit that fade while the other end is crossfaded.
            return submit(RemoveCrossfade{p.id, x.left, x.right}, why);
        }
    }
    for (const Region& r : p.regions) {
        const RegionFades f = region_fades(p, r);
        const bool in = f.in > 0 && sel_.start == r.position && sel_.end == r.position + f.in;
        const bool out = f.out > 0 && sel_.start == r.end() - f.out && sel_.end == r.end();
        if (!in && !out) continue;
        done = true;
        Fade gone = in ? r.fade_in : r.fade_out;
        gone.length = 0;
        return submit(SetFade{p.id, r.id, in ? FadeEnd::In : FadeEnd::Out, gone}, why);
    }
    return true;
}

}  // namespace sonora::ui
