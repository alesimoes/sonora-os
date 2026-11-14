// SPDX-License-Identifier: GPL-3.0-or-later
//
// The EditController's clipboard: Cut, Copy, Paste, Duplicate and Repeat.
#include "edit_controller.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "edit_controller_detail.h"
#include "edit_shuffle_ops.h"

namespace sonora::ui {

using namespace model;
using namespace edit_detail;

// --- the clipboard ---
//
// Pro Tools Reference Guide, "Editing" chapter: "Cut, Copy, Paste and Clear
// Commands" (Cut moves the selection to the Clipboard; Paste puts it at the
// insertion point or the selection's start, replacing what lies beneath in
// Slip, Spot and Grid), "Duplicate Command" (a copy placed straight after
// the selection) and "Repeat Command" (Duplicate a number of times, the
// number asked in a dialog). The workflow only (ADR-0004).

namespace {

// "Gtr", "Gtr and Bass", "Gtr, Bass and Keys".
std::string spoken_list(const std::vector<std::string>& names) {
    std::string out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i > 0) out += i + 1 == names.size() ? " and " : ", ";
        out += names[i];
    }
    return out;
}

}  // namespace

bool EditController::submit_shuffle_trim(const TrimRegion& op, std::string& why) {
    const TrackEdit* t = nullptr;
    const Region* r = find_region(snapshot(), op.region, &t);
    if (r == nullptr) {
        why = "the region is gone";
        return false;
    }
    if (r->clip_group != 0) {
        why = "a grouped clip does not ripple in Shuffle mode; ungroup it or use Slip. "
              "Nothing was trimmed.";
        return false;
    }
    TrimRegion limited = op;
    if (!shuffle_limit_trim(*active(t), *r, limited)) return true;
    std::vector<Member> members;
    shuffle_trim(*active(t), *r, limited, members);
    const int at = track_index(snapshot(), t->id);
    if (!submit_compound(std::move(members), "Trim", "trimmed", why)) return false;
    note_shuffle_automation("Trim", at, 1);
    return true;
}

bool EditController::submit_shuffle_move(const MoveRegion& op, std::string& why) {
    const TrackEdit* t = nullptr;
    const Region* r = find_region(snapshot(), op.region, &t);
    if (r == nullptr) {
        why = "the region is gone";
        return false;
    }
    if (r->clip_group != 0) {
        why = "a grouped clip does not ripple in Shuffle mode; ungroup it or use Slip. "
              "Nothing was moved.";
        return false;
    }
    if (op.position == r->position) return true;
    std::vector<Member> members;
    shuffle_move(*active(t), *r, op.position, [this]() { return doc_.reserve_id(); }, members);
    const int at = track_index(snapshot(), t->id);
    if (!submit_compound(std::move(members), "Move", "moved", why)) return false;
    note_shuffle_automation("Move", at, 1);
    return true;
}

bool EditController::submit_trim(const TrimRegion& op, std::string& why) {
    return mode_ == EditMode::Shuffle ? submit_shuffle_trim(op, why) : submit(op, why);
}

bool EditController::submit_place(const MoveRegion& op, std::string& why) {
    return mode_ == EditMode::Shuffle ? submit_shuffle_move(op, why) : submit_move(op, why);
}

void EditController::note_shuffle_automation(const char* gesture, int first, int count) {
    // Pro Tools' Automation Follows Edit would carry the lanes along with the
    // ripple; ours stay put, and an edit that leaves a lane out of step with
    // its audio says so.
    std::vector<std::string> names;
    for (int i = 0; i < count; ++i) {
        const TrackEdit* t = track(first + i);
        if (t == nullptr) continue;
        const bool any = std::any_of(t->automation.begin(), t->automation.end(), [](const auto& l) {
            return l != nullptr && !l->points.empty();
        });
        if (any) names.push_back(t->name);
    }
    follow_notice_.clear();
    if (names.empty()) return;
    follow_notice_ = std::string("Automation did not follow the Shuffle ") + gesture + ": " +
                     spoken_list(names) + (names.size() == 1 ? " keeps" : " keep") +
                     " its automation where it was";
}

int EditController::span() const {
    const int n = static_cast<int>(snapshot().edit->tracks.size());
    if (sel_.track < 0 || sel_.track >= n) return 0;
    return std::clamp(sel_.tracks, 1, n - sel_.track);
}

bool EditController::refuse_several(const char* gesture, std::string& why) const {
    if (sel_.region != kNoId || span() <= 1) return false;
    why = std::string(gesture) + " on several tracks is not built yet; select one track";
    return true;
}

bool EditController::edit_range(Span& out, const char* nothing_was, std::string& why) const {
    if (sel_.region != kNoId) {
        const TrackEdit* t = nullptr;
        const Region* r = find_region(snapshot(), sel_.region, &t);
        if (r == nullptr) {
            why = std::string("the selected region is gone; nothing was ") + nothing_was;
            return false;
        }
        out = Span{track_index(snapshot(), t->id), 1, r->position, r->end()};
        return true;
    }
    if (track(sel_.track) == nullptr) {
        why = std::string("select a track first; nothing was ") + nothing_was;
        return false;
    }
    if (!sel_.has_range()) {
        why = std::string("select a region or a time range first; nothing was ") + nothing_was;
        return false;
    }
    out = Span{sel_.track, span(), sel_.start, sel_.end};
    return true;
}

bool EditController::copy_lanes(const Span& e, Clipboard& out, const char* nothing_was,
                                std::string& why) const {
    Clipboard clip;
    bool any = false;
    for (int i = 0; i < e.count; ++i) {
        Clip lane;
        any = copy_range(snapshot(), *track(e.first + i), e.a, e.b, lane) || any;
        clip.lanes.push_back(std::move(lane));
    }
    if (!any) {
        why = e.count == 1 ? "there is no region in the selection on " + track(e.first)->name
                           : "there is no region in the selection on any of its " +
                                 std::to_string(e.count) + " tracks";
        why += std::string("; nothing was ") + nothing_was;
        return false;
    }
    out = std::move(clip);
    return true;
}

void EditController::select_span(int first, int count, Frames a, Frames b) {
    sel_.track = first;
    sel_.tracks = count;
    sel_.region = kNoId;
    sel_.autolane = sel_.point = -1;
    sel_.start = sel_.insertion = a;
    sel_.end = b;
}

bool EditController::submit_compound(std::vector<Member> members, const char* gesture,
                                     const char* nothing_was, std::string& why) {
    const std::string too_many = over_limit(gesture, nothing_was, members.size());
    if (!too_many.empty()) {
        why = too_many;
        return false;
    }
    return submit(Compound{std::move(members), gesture}, why);
}

bool EditController::submit_following(Member op, std::vector<EditAutomation> follow,
                                      const char* gesture, const char* nothing_was,
                                      std::string& why) {
    if (follow.empty()) return submit(to_operation(op), why);
    std::vector<Member> members{std::move(op)};
    for (EditAutomation& f : follow) members.push_back(std::move(f));
    return submit_compound(std::move(members), gesture, nothing_was, why);
}

bool EditController::submit_move(const MoveRegion& op, std::string& why) {
    // A clip in a group takes its group along (ADR-0052), and with Automation
    // Follows Edit each member's lane span goes too, in the same Compound so
    // one undo reverts clips and lanes together (ADR-0098 follow-up c).
    std::vector<Member> group;
    if (group_move_ops(op, group)) {
        std::vector<Member> moves = group;
        std::vector<std::string> left_all;
        if (automation_follows_edit_) {
            for (const Member& m : moves) {
                const MoveRegion* step = get_if<MoveRegion>(&m);
                const TrackEdit* gt = nullptr;
                const Region* gr = step ? find_region(snapshot(), step->region, &gt) : nullptr;
                if (gr == nullptr) continue;
                std::vector<const AutomationLane*> left;
                for (EditAutomation& f :
                     follow_move_ops(*gt, gr->position, gr->end(), step->position, &left)) {
                    group.push_back(std::move(f));
                }
                for (std::string& n : lane_labels(*gt, left)) left_all.push_back(std::move(n));
            }
        }
        if (!submit_compound(std::move(group), "Move Group", "moved", why)) return false;
        note_left_behind("Move", left_all);
        return true;
    }
    std::vector<EditAutomation> follow;
    std::vector<std::string> left_names;
    const TrackEdit* t = nullptr;
    const Region* r = automation_follows_edit_ ? find_region(snapshot(), op.region, &t) : nullptr;
    if (r != nullptr) {
        std::vector<const AutomationLane*> left;
        follow = follow_move_ops(*t, r->position, r->end(), op.position, &left);
        left_names = lane_labels(*t, left);
    }
    if (!submit_following(op, std::move(follow), "Move", "moved", why)) return false;
    note_left_behind("Move", left_names);
    return true;
}

void EditController::note_left_behind(const char* gesture, const std::vector<std::string>& lanes) {
    follow_notice_.clear();
    if (lanes.empty()) return;
    std::string names;
    for (const std::string& n : lanes) names += (names.empty() ? "" : ", ") + n;
    const bool one = lanes.size() == 1;
    follow_notice_ = std::string("Automation did not follow the ") + gesture + ": " + names +
                     (one ? " holds " : " hold ") + std::to_string(kMaxAutomationPoints) +
                     " points, the most a lane holds, and " + (one ? "stays" : "stay") +
                     " as it was";
}

void EditController::set_session_folder_source(std::function<std::string()> folder) {
    session_folder_ = std::move(folder);
}

void EditController::set_bring_in(BringInFn bring_in) { bring_in_ = std::move(bring_in); }

void EditController::stamp_origin(Clipboard& clip) const {
    clip.origin_folder = session_folder_ ? session_folder_() : std::string();
    clip.origin_sample_rate = snapshot().settings.sample_rate;
}

// Cross-session paste (ADR-0142): the clipboard's audio is brought into this
// session before anything is sent. `use` is the clipboard with its source ids
// mapped onto this session's; `ops` the AddSource members that go first.
bool EditController::bring_clipboard_in(Clipboard& use, std::vector<Member>& ops,
                                        std::string& why) {
    if (recording_) {
        why = "Stop recording first: a paste that brings audio in from another session copies "
              "files while a take is written; nothing was pasted";
        return false;
    }
    if (!bring_in_) {
        why = "the clipboard's audio was copied from another session and this window cannot "
              "bring it in; nothing was pasted";
        return false;
    }
    std::vector<Source> sources;
    for (const Clip& lane : clipboard_.lanes) {
        for (const Source& s : lane.sources) {
            const bool known = std::any_of(sources.begin(), sources.end(),
                                           [&s](const Source& x) { return x.id == s.id; });
            if (!known) sources.push_back(s);
        }
    }
    BroughtIn in;
    if (!bring_in_(snapshot(), sources, clipboard_.origin_folder, clipboard_.origin_sample_rate,
                   in, why)) {
        return false;
    }
    // The ids the AddSources took are this document's from now on; the paste's
    // own members start past them.
    if (!in.ops.empty()) {
        while (doc_.reserve_id() + 1 < in.next_id) {}
    }
    for (Clip& lane : use.lanes) {
        for (Region& r : lane.regions) {
            for (SourceRef& ch : r.channels) {
                const auto it = in.remap.find(ch.source);
                if (it != in.remap.end()) ch.source = it->second;
            }
        }
        lane.sources.clear();
    }
    ops = std::move(in.ops);
    return true;
}

bool EditController::copy(std::string& why) {
    if (autolane_selected()) return automation_copy(false, why);
    Span e;
    Clipboard clip;
    if (!edit_range(e, "copied", why) || !copy_lanes(e, clip, "copied", why)) return false;
    stamp_origin(clip);
    clipboard_ = std::move(clip);
    ++clipboard_serial_;
    return true;
}

bool EditController::cut(std::string& why) {
    if (autolane_selected()) return automation_copy(true, why);
    Span e;
    Clipboard clip;
    if (!edit_range(e, "cut", why) || !copy_lanes(e, clip, "cut", why)) return false;
    std::vector<Member> members;
    for (int i = 0; i < e.count; ++i) {
        clear_range(*active(track(e.first + i)), e.a, e.b, [this]() { return doc_.reserve_id(); },
                    members);
        if (mode_ == EditMode::Shuffle) {
            shuffle_shift(*active(track(e.first + i)), e.b, -(e.b - e.a), members);
        }
    }
    // The clipboard changes only with the edit: a refused Cut leaves both.
    if (!submit_compound(std::move(members), "Cut", "cut", why)) return false;
    if (mode_ == EditMode::Shuffle) note_shuffle_automation("Cut", e.first, e.count);
    stamp_origin(clip);
    clipboard_ = std::move(clip);
    ++clipboard_serial_;
    select_span(e.first, e.count, e.a, e.b);
    return true;
}

bool EditController::paste(std::string& why) {
    if (autolane_selected()) return automation_paste(why);
    if (track(sel_.track) == nullptr) {
        why = "select a track first; nothing was pasted";
        return false;
    }
    if (clipboard_.empty()) {
        why = "the clipboard is empty; cut or copy a selection first";
        return false;
    }
    // Pro Tools pastes each lane as many tracks below the selected one as it
    // was copied from below the first, and leaves out lanes that run past
    // the last track. What is left out is said, never silent.
    const int n = static_cast<int>(snapshot().edit->tracks.size());
    const int lanes = static_cast<int>(clipboard_.lanes.size());
    // One track's material pasted into a selection on several tracks fills
    // every one of them, as Pro Tools does; a clipboard of several lanes
    // keeps their layout, so it never repeats onto a selection's extra tracks.
    const int targets = lanes == 1 && sel_.region == kNoId ? span() : 1;
    Clipboard brought;   // the clipboard with another session's audio mapped in
    const Clipboard* src = &clipboard_;
    const auto lane_for = [&src, targets](int i) -> const Clip& {
        return src->lanes[targets > 1 ? 0 : static_cast<std::size_t>(i)];
    };
    const int fit = targets > 1 ? targets : std::min(lanes, n - sel_.track);
    bool any = false;
    bool foreign = false;   // audio of another session, to bring in
    const std::string here = session_folder_ ? session_folder_() : std::string();
    const bool other_session = clipboard_.origin_folder != here;
    for (int i = 0; i < fit; ++i) {
        const Clip& lane = lane_for(i);
        if (lane.empty()) continue;
        any = true;
        const bool missing = sources_missing(snapshot(), lane);
        const std::string problem = missing && other_session
                                        ? channel_problem(*track(sel_.track + i), lane)
                                        : paste_problem(snapshot(), *track(sel_.track + i), lane);
        if (!problem.empty()) {
            why = problem;
            return false;
        }
        foreign = foreign || (missing && other_session);
    }
    // Only lanes that hold regions are lost by being left out: an empty one
    // would have cleared a track that is not there.
    std::vector<std::string> left_out;
    for (int i = fit; i < lanes; ++i) {
        const Clip& lane = clipboard_.lanes[static_cast<std::size_t>(i)];
        if (!lane.empty()) left_out.push_back(lane.track);
    }
    const std::string last = track(n - 1)->name;
    if (!any) {
        // Only the lanes' gaps would land: a paste that clears and places
        // nothing is not what the operator asked for.
        why = "the clipboard's regions were copied from " + spoken_list(left_out) +
              ", and there is no track below " + last + " to paste them on; nothing was pasted";
        return false;
    }
    std::vector<Member> members;
    if (foreign) {
        brought = clipboard_;
        if (!bring_clipboard_in(brought, members, why)) return false;
        src = &brought;
    }
    const Frames at = sel_.has_range() ? sel_.start : sel_.insertion;
    const bool shuffle = mode_ == EditMode::Shuffle;
    for (int i = 0; i < fit; ++i) {
        const Playlist& p = *active(track(sel_.track + i));
        const auto next = [this]() { return doc_.reserve_id(); };
        if (shuffle && sel_.has_range()) {
            // The selection is replaced: it goes, and what follows moves by
            // the difference between the clipboard's length and its own.
            clear_range(p, sel_.start, sel_.end, next, members);
            shuffle_shift(p, sel_.end, clipboard_.length() - (sel_.end - sel_.start), members);
        } else if (shuffle) {
            shuffle_open(p, at, clipboard_.length(), next, members);
        }
        paste_clip(p, at, lane_for(i), next, members, !shuffle);
    }
    if (!submit_compound(std::move(members), "Paste", "pasted", why)) return false;
    if (shuffle) note_shuffle_automation("Paste", sel_.track, fit);
    select_span(sel_.track, fit, at, at + clipboard_.length());
    why.clear();
    if (!left_out.empty()) {
        why = "Pasted " + std::to_string(fit) + " of the clipboard's " + std::to_string(lanes) +
              " tracks: there is no track below " + last + " for what was copied from " +
              spoken_list(left_out) + ".";
    }
    return true;
}

bool EditController::duplicate(std::string& why) {
    Span e;
    Clipboard clip;
    if (!edit_range(e, "duplicated", why) || !copy_lanes(e, clip, "duplicated", why)) {
        return false;
    }
    std::vector<Member> members;
    const bool shuffle = mode_ == EditMode::Shuffle;
    for (int i = 0; i < e.count; ++i) {
        const Playlist& p = *active(track(e.first + i));
        const auto next = [this]() { return doc_.reserve_id(); };
        if (shuffle) shuffle_open(p, e.b, clip.length(), next, members);
        paste_clip(p, e.b, clip.lanes[static_cast<std::size_t>(i)], next, members, !shuffle);
    }
    if (!submit_compound(std::move(members), "Duplicate", "duplicated", why)) return false;
    if (shuffle) note_shuffle_automation("Duplicate", e.first, e.count);
    select_span(e.first, e.count, e.b, e.b + clip.length());
    return true;
}

bool EditController::can_repeat(std::string& why, std::string* track_out, Frames* length) const {
    Span e;
    Clipboard clip;
    if (!edit_range(e, "repeated", why) || !copy_lanes(e, clip, "repeated", why)) return false;
    if (track_out != nullptr) {
        std::vector<std::string> names;
        for (int i = 0; i < e.count; ++i) names.push_back(track(e.first + i)->name);
        *track_out = spoken_list(names);
    }
    if (length != nullptr) *length = e.b - e.a;
    return true;
}

bool EditController::repeat(int count, std::string& why) {
    if (!can_repeat(why)) return false;
    if (count < 1) {
        why = "Repeat needs a number of repeats of 1 or more; nothing was repeated";
        return false;
    }
    Span e;
    Clipboard clip;
    edit_range(e, "repeated", why);
    copy_lanes(e, clip, "repeated", why);
    // Counted before it is built: a typed 100000 is refused by its size,
    // not by allocating it. A lane with no region costs only its clear, but
    // one lane at least holds a region, so a count that passes is at most
    // the member limit.
    std::size_t total = 0;
    for (int i = 0; i < e.count; ++i) {
        total += repeat_members(*active(track(e.first + i)), e.b,
                                clip.lanes[static_cast<std::size_t>(i)],
                                static_cast<std::size_t>(count));
        // A split and a shift a track, which Shuffle adds in place of the clear.
        if (mode_ == EditMode::Shuffle) total += 2;
    }
    const std::string too_many = over_limit("Repeat", "repeated", total);
    if (!too_many.empty()) {
        why = too_many;
        return false;
    }
    std::vector<Member> members;
    const bool shuffle = mode_ == EditMode::Shuffle;
    for (int i = 0; i < e.count; ++i) {
        const Playlist& p = *active(track(e.first + i));
        const auto next = [this]() { return doc_.reserve_id(); };
        if (shuffle) shuffle_open(p, e.b, clip.length() * count, next, members);
        repeat_clip(p, e.b, clip.lanes[static_cast<std::size_t>(i)], count, next, members,
                    !shuffle);
    }
    if (!submit_compound(std::move(members), "Repeat", "repeated", why)) return false;
    if (shuffle) note_shuffle_automation("Repeat", e.first, e.count);
    repeat_count_ = count;
    select_span(e.first, e.count, e.b, e.b + clip.length() * count);
    return true;
}

}  // namespace sonora::ui
