// SPDX-License-Identifier: GPL-3.0-or-later
//
// Cut, Copy, Paste, Duplicate, Repeat and Clear (edit_clipboard.h): each gesture one
// compound and one labelled undo step; past model::kMaxCompoundMembers
// refused before anything is sent, in the operator's words, changing
// nothing; Shuffle rippling the regions after it. Then the Edit menu in the real QML,
// loaded on the offscreen platform: its undo text, and its items sending
// what the keys send.
#include <QGuiApplication>
#include <QKeyEvent>
#include <QQuickItem>
#include <QQuickView>
#include <QTest>
#include <algorithm>
#include <cstdio>
#include <set>
#include <string>

#include "edit_clipboard.h"
#include "edit_controller.h"
#include "key_router.h"
#include "peak_source.h"
#include "qt_keys.h"
#include "recording_adapter.h"
#include "register_types.h"
#include "session_model.h"
#include "shell_model.h"
#include "sonora/api/fake_engine.h"
#include "timeline_document.h"

using namespace sonora;
using namespace sonora::ui;
using namespace sonora::ui::model;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

constexpr uint32_t kRate = 48000;
constexpr Frames kBar = 96000;   // 120 bpm, 4/4

Region make_region(Id id, const char* name, Frames position, Frames length) {
    Region r;
    r.id = id;
    r.name = name;
    r.position = position;
    r.length = length;
    r.source_offset = position;   // the source lies along the timeline
    r.channels = {{100, 0}};
    return r;
}

// Two mono tracks over one 40-bar source (id 100). Track 1 holds `regions`,
// track 2 is empty. Ids below 1000 are the test's; new ones start there.
Snapshot session_with(std::vector<Region> regions, std::vector<Crossfade> crossfades = {}) {
    Snapshot s = make_empty_session(2, kRate);
    auto pool = std::make_shared<SourcePool>();
    Source take;
    take.id = 100;
    take.path = "Audio Files/Take 1.wav";
    take.length = 40 * kBar;
    pool->sources.push_back(take);
    s.sources = pool;
    s.next_id = 1000;
    auto edit = std::make_shared<EditState>(*s.edit);
    auto t = std::make_shared<TrackEdit>(*edit->tracks[0]);
    auto p = std::make_shared<Playlist>(*t->playlists[0]);
    p->regions = std::move(regions);
    p->crossfades = std::move(crossfades);
    t->playlists[0] = p;
    edit->tracks[0] = t;
    s.edit = edit;
    return s;
}

// A [1, 3) bars and B [4, 5) bars.
Snapshot two_regions() {
    return session_with({make_region(10, "A", kBar, 2 * kBar), make_region(11, "B", 4 * kBar, kBar)});
}

const Playlist& lane(const Snapshot& s, int track) {
    return *active_playlist(s.edit->tracks[static_cast<std::size_t>(track)].get());
}

bool valid(const Snapshot& s) {
    std::string error;
    const bool ok = check_invariants(s, error);
    if (!ok) std::fprintf(stderr, "  invariant: %s\n", error.c_str());
    return ok;
}

bool has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

void select_range(EditController& c, int track, Frames a, Frames b) {
    // The Selector, so a press on a region's edge is a range, not a trim.
    c.set_tool(Tool::Select);
    c.select_track(track);
    c.set_insertion(a);
    // A range is what a Selector drag leaves; set it as the release would.
    const double x0 = c.view().x_of(a), x1 = c.view().x_of(b);
    const double y = c.view().y_of_track(track) + 4;   // upper half: the Selector
    std::string why;
    c.press(x0, y, why);
    c.move(x1, y);
    c.release(x1, y, why);
    c.set_tool(Tool::Smart);
}

// --- copy and paste ---------------------------------------------------------

int test_copy_paste() {
    ModelDocument doc(two_regions());
    EditController c(doc);
    c.set_viewport(4000, 400);
    c.scroll_to_frame(0);
    std::string why;

    // Nothing selected: refused, by name.
    c.select_track(0);
    c.set_insertion(0);
    CHECK(!c.copy(why) && has(why, "nothing was copied"));
    CHECK(!c.paste(why) && has(why, "clipboard is empty"));

    // Copy is no edit: no revision, no undo step.
    CHECK(c.select_region(10));
    const uint64_t r0 = doc.revision();
    CHECK(c.copy(why));
    CHECK(doc.revision() == r0 && !doc.can_undo());
    CHECK(c.clipboard().lanes[0].regions.size() == 1 && c.clipboard().lanes[0].length == 2 * kBar);

    // Paste on track 2 at bar 8: one region, one undo step, "Paste".
    c.select_track(1);
    c.set_insertion(8 * kBar);
    CHECK(c.paste(why));
    const Playlist& two = lane(doc.snapshot(), 1);
    CHECK(two.regions.size() == 1);
    CHECK(two.regions[0].position == 8 * kBar && two.regions[0].length == 2 * kBar);
    CHECK(two.regions[0].source_offset == kBar);   // the same audio
    CHECK(two.regions[0].id >= 1000);
    CHECK(doc.history().undo_depth() == 1 && doc.undo_label() == "Paste");
    CHECK(c.selection().start == 8 * kBar && c.selection().end == 10 * kBar);
    CHECK(valid(doc.snapshot()));
    CHECK(c.undo(why));
    CHECK(lane(doc.snapshot(), 1).regions.empty() && !doc.can_undo());

    // A range across A's end, a gap and B's start keeps the gap, and a paste
    // of it over A clears what the gap covers: one step.
    select_range(c, 0, 2 * kBar, 4 * kBar + kBar / 2);
    CHECK(c.selection().has_range() && c.selection().region == kNoId);
    CHECK(c.copy(why));
    CHECK(c.clipboard().lanes[0].regions.size() == 2 && c.clipboard().lanes[0].length == 2 * kBar + kBar / 2);
    CHECK(c.clipboard().lanes[0].regions[1].position == 2 * kBar);
    select_range(c, 0, 0, 0);
    c.set_insertion(kBar);
    CHECK(c.paste(why));
    const Playlist& one = lane(doc.snapshot(), 0);
    // [1, 2) from A's second half, nothing in [2, 3), [3, 3.5) from B's
    // first half, then B as it was.
    CHECK(one.regions.size() == 3);
    CHECK(one.regions[0].position == kBar && one.regions[0].length == kBar);
    CHECK(one.regions[0].source_offset == 2 * kBar);
    CHECK(one.regions[1].position == 3 * kBar && one.regions[1].length == kBar / 2);
    CHECK(one.regions[1].source_offset == 4 * kBar);
    CHECK(one.regions[2].id == 11 && one.regions[2].position == 4 * kBar);
    CHECK(doc.history().undo_depth() == 1 && doc.undo_label() == "Paste");
    CHECK(valid(doc.snapshot()));
    CHECK(c.undo(why));
    CHECK(lane(doc.snapshot(), 0).regions.size() == 2);

    // A clipboard from another session names audio this one does not have.
    Snapshot other = two_regions();
    auto pool = std::make_shared<SourcePool>(*other.sources);
    pool->sources[0].path = "Audio Files/Someone else.wav";
    other.sources = pool;
    CHECK(doc.reset(other, why));
    c.document_replaced();
    c.select_track(1);
    c.set_insertion(0);
    CHECK(!c.paste(why) && has(why, "another session"));
    CHECK(lane(doc.snapshot(), 1).regions.empty());
    return 0;
}

// --- cut, clear, duplicate, separate ----------------------------------------

int test_cut_clear_duplicate() {
    ModelDocument doc(two_regions());
    EditController c(doc);
    c.set_viewport(4000, 400);
    c.scroll_to_frame(0);
    std::string why;

    // Cut inside A: the region either side stays, the middle goes to the
    // clipboard, one undo step brings A back whole.
    select_range(c, 0, kBar + kBar / 2, 2 * kBar + kBar / 2);
    CHECK(c.cut(why));
    const Playlist& p = lane(doc.snapshot(), 0);
    CHECK(p.regions.size() == 3);
    CHECK(p.regions[0].id == 10 && p.regions[0].end() == kBar + kBar / 2);
    CHECK(p.regions[1].position == 2 * kBar + kBar / 2 && p.regions[1].end() == 3 * kBar);
    CHECK(c.clipboard().lanes[0].regions.size() == 1 && c.clipboard().lanes[0].regions[0].source_offset == kBar + kBar / 2);
    CHECK(doc.history().undo_depth() == 1 && doc.undo_label() == "Cut");
    CHECK(valid(doc.snapshot()));
    CHECK(c.undo(why));
    CHECK(lane(doc.snapshot(), 0).regions.size() == 2 && !doc.can_undo());

    // Clear on a range: the same removal, labelled Clear, clipboard untouched.
    const uint64_t clip = c.clipboard_serial();
    select_range(c, 0, 2 * kBar, 5 * kBar);
    CHECK(c.clear(why));
    CHECK(lane(doc.snapshot(), 0).regions.size() == 1 && doc.undo_label() == "Clear");
    CHECK(c.clipboard_serial() == clip);
    CHECK(c.undo(why));

    // Duplicate B: a copy straight after it, which becomes the selection.
    CHECK(c.select_region(11));
    CHECK(c.duplicate(why));
    const Playlist& d = lane(doc.snapshot(), 0);
    CHECK(d.regions.size() == 3 && d.regions[2].position == 5 * kBar && d.regions[2].length == kBar);
    CHECK(c.selection().start == 5 * kBar && c.selection().end == 6 * kBar);
    CHECK(doc.undo_label() == "Duplicate" && c.clipboard_serial() == clip);
    CHECK(c.undo(why));

    // Separate at both ends of a range: one step, not two.
    select_range(c, 0, kBar + kBar / 4, 2 * kBar);
    CHECK(c.separate(why));
    CHECK(lane(doc.snapshot(), 0).regions.size() == 4);
    CHECK(doc.history().undo_depth() == 1 && doc.undo_label() == "Separate");
    CHECK(c.undo(why) && !doc.can_undo());

    // Spot and Grid paste as Slip does.
    c.set_mode(EditMode::Spot);
    c.select_track(1);
    c.set_insertion(0);
    CHECK(c.paste(why));
    return 0;
}

// --- Shuffle: every gap closes or opens (ShiftRegions) ----------------------

Frames start_of(const Snapshot& s, Id region, int track = 0) {
    for (const Region& r : lane(s, track).regions) {
        if (r.id == region) return r.position;
    }
    return -1;
}

int test_shuffle() {
    ModelDocument doc(two_regions());   // A [1, 3) bars, B [4, 5)
    EditController c(doc);
    c.set_viewport(4000, 400);
    c.scroll_to_frame(0);
    c.set_mode(EditMode::Shuffle);
    std::string why;

    // Cut closes the gap A leaves: B moves up by A's length, in one step.
    CHECK(c.select_region(10));
    CHECK(c.cut(why));
    CHECK(lane(doc.snapshot(), 0).regions.size() == 1 && start_of(doc.snapshot(), 11) == 2 * kBar);
    CHECK(doc.history().undo_depth() == 1 && doc.undo_label() == "Cut");
    CHECK(valid(doc.snapshot()));
    // Paste at the insertion point opens a gap the clipboard's length (2 bars).
    c.select_track(0);
    c.set_insertion(0);
    CHECK(c.paste(why));
    CHECK(start_of(doc.snapshot(), 11) == 4 * kBar);
    CHECK(lane(doc.snapshot(), 0).regions.size() == 2 && lane(doc.snapshot(), 0).regions[0].position == 0);
    CHECK(valid(doc.snapshot()));
    CHECK(c.undo(why) && c.undo(why));
    CHECK(start_of(doc.snapshot(), 10) == kBar && start_of(doc.snapshot(), 11) == 4 * kBar);

    // Clear on a region closes its gap too.
    CHECK(c.select_region(10));
    CHECK(c.clear(why));
    CHECK(start_of(doc.snapshot(), 11) == 2 * kBar && doc.undo_label() == "Clear");
    CHECK(c.undo(why));

    // Clear on a range inside A: the part after it moves up by the range's
    // length, and so does B.
    select_range(c, 0, kBar + kBar / 2, 2 * kBar + kBar / 2);
    CHECK(c.clear(why));
    {
        const Playlist& p = lane(doc.snapshot(), 0);
        CHECK(p.regions.size() == 3);
        CHECK(p.regions[0].position == kBar && p.regions[0].end() == kBar + kBar / 2);
        CHECK(p.regions[1].position == kBar + kBar / 2 && p.regions[1].end() == 2 * kBar);
        CHECK(p.regions[2].id == 11 && p.regions[2].position == 3 * kBar);
    }
    CHECK(valid(doc.snapshot()));
    CHECK(c.undo(why));

    // Paste at an insertion point inside A splits it and opens the gap.
    CHECK(c.select_region(10));
    CHECK(c.copy(why));
    c.select_track(0);
    c.set_insertion(2 * kBar);
    CHECK(c.paste(why));
    {
        const Playlist& p = lane(doc.snapshot(), 0);
        CHECK(p.regions.size() == 4);
        CHECK(p.regions[0].end() == 2 * kBar && p.regions[1].position == 2 * kBar);
        CHECK(p.regions[1].end() == 4 * kBar && p.regions[2].position == 4 * kBar);
        CHECK(p.regions[3].id == 11 && p.regions[3].position == 6 * kBar);
    }
    CHECK(valid(doc.snapshot()));
    CHECK(c.undo(why));

    // Paste over a range replaces it: B (1 bar) for A's 2 bars pulls B up by one.
    CHECK(c.select_region(11));
    CHECK(c.copy(why));
    select_range(c, 0, kBar, 3 * kBar);
    CHECK(c.paste(why));
    {
        const Playlist& p = lane(doc.snapshot(), 0);
        CHECK(p.regions.size() == 2);
        CHECK(p.regions[0].position == kBar && p.regions[0].length == kBar);
        CHECK(p.regions[1].id == 11 && p.regions[1].position == 3 * kBar);
    }
    CHECK(valid(doc.snapshot()));
    CHECK(c.undo(why));

    // Duplicate and Repeat push what follows by what they add.
    CHECK(c.select_region(10));
    CHECK(c.duplicate(why));
    CHECK(lane(doc.snapshot(), 0).regions.size() == 3 && start_of(doc.snapshot(), 11) == 6 * kBar);
    CHECK(valid(doc.snapshot()));
    CHECK(c.undo(why));
    CHECK(c.select_region(10));
    CHECK(c.repeat(2, why));
    CHECK(lane(doc.snapshot(), 0).regions.size() == 4 && start_of(doc.snapshot(), 11) == 8 * kBar);
    CHECK(valid(doc.snapshot()));
    CHECK(c.undo(why));

    // The last region has nothing after it to move: no ripple member.
    CHECK(c.select_region(11));
    CHECK(c.cut(why));
    CHECK(lane(doc.snapshot(), 0).regions.size() == 1 && start_of(doc.snapshot(), 10) == kBar);
    CHECK(c.undo(why));

    CHECK(c.take_follow_notice().empty());
    return 0;
}

// --- Shuffle: move, trim, nudge, spot and Loop ripple (ADR-0153) -------------

Frames end_of(const Snapshot& s, Id region) {
    for (const Region& r : lane(s, 0).regions) {
        if (r.id == region) return r.end();
    }
    return -1;
}

// A [1, 3) bars, B [4, 5), C [6, 7): a gap of a bar before B and before C.
Snapshot three_regions() {
    return session_with({make_region(10, "A", kBar, 2 * kBar), make_region(11, "B", 4 * kBar, kBar),
                         make_region(12, "C", 6 * kBar, kBar)});
}

int test_shuffle_trim_and_spot() {
    ModelDocument doc(three_regions());
    EditController c(doc);
    c.set_viewport(4000, 400);
    c.scroll_to_frame(0);
    c.set_mode(EditMode::Shuffle);
    std::string why;
    const Snapshot original = doc.snapshot();

    // Trim A's end to 2 bars: every later region moves up by the bar lost.
    c.select_track(0);
    c.set_insertion(2 * kBar);
    CHECK(c.trim_end_to_insertion(why));
    CHECK(end_of(doc.snapshot(), 10) == 2 * kBar);
    CHECK(start_of(doc.snapshot(), 11) == 3 * kBar && start_of(doc.snapshot(), 12) == 5 * kBar);
    CHECK(doc.history().undo_depth() == 1 && doc.undo_label() == "Trim");
    CHECK(valid(doc.snapshot()));
    CHECK(c.undo(why) && !doc.can_undo());
    CHECK(start_of(doc.snapshot(), 11) == 4 * kBar && end_of(doc.snapshot(), 10) == 3 * kBar);

    // Trim A's start: it keeps its place, its audio starts later, the rest follows.
    c.select_track(0);
    c.set_insertion(2 * kBar);
    CHECK(c.trim_start_to_insertion(why));
    CHECK(start_of(doc.snapshot(), 10) == kBar && end_of(doc.snapshot(), 10) == 2 * kBar);
    CHECK(lane(doc.snapshot(), 0).regions[0].source_offset == 2 * kBar);
    CHECK(start_of(doc.snapshot(), 11) == 3 * kBar && start_of(doc.snapshot(), 12) == 5 * kBar);
    CHECK(doc.history().undo_depth() == 1 && valid(doc.snapshot()));
    CHECK(c.undo(why) && !doc.can_undo());

    // Spot A at 5 bars: it lands there, the gap it left closes (B and C move
    // up two bars), in one step.
    CHECK(c.select_region(10));
    CHECK(c.spot_region(5 * kBar, why));
    CHECK(start_of(doc.snapshot(), 10) == 5 * kBar);
    CHECK(start_of(doc.snapshot(), 11) == 2 * kBar && start_of(doc.snapshot(), 12) == 4 * kBar);
    CHECK(doc.history().undo_depth() == 1 && doc.undo_label() == "Move" && valid(doc.snapshot()));
    CHECK(c.undo(why) && !doc.can_undo());
    CHECK(start_of(doc.snapshot(), 10) == kBar && start_of(doc.snapshot(), 12) == 6 * kBar);

    // Spot C at 2 bars, inside A: A is split and room opens the width of C.
    CHECK(c.select_region(12));
    CHECK(c.spot_region(2 * kBar, why));
    CHECK(start_of(doc.snapshot(), 12) == 2 * kBar);
    CHECK(lane(doc.snapshot(), 0).regions.size() == 4);
    CHECK(start_of(doc.snapshot(), 11) == 5 * kBar && valid(doc.snapshot()));
    CHECK(doc.history().undo_depth() == 1);
    CHECK(c.undo(why) && lane(doc.snapshot(), 0).regions.size() == 3 && !doc.can_undo());

    // No-ops send nothing: spot where it already is, a trim to the same edge.
    CHECK(c.select_region(10));
    const uint64_t r0 = doc.revision();
    CHECK(c.spot_region(kBar, why));
    c.select_track(0);
    c.set_insertion(3 * kBar);   // on A's end, not inside it
    CHECK(!c.trim_end_to_insertion(why));
    CHECK(doc.revision() == r0 && !doc.can_undo());
    CHECK(lane(doc.snapshot(), 0).regions.size() == original.edit->tracks[0]->playlists[0]->regions.size());
    return 0;
}

int test_shuffle_nudge_and_drag() {
    ModelDocument doc(three_regions());
    EditController c(doc);
    c.set_viewport(4000, 400);
    c.scroll_to_frame(0);
    c.set_mode(EditMode::Shuffle);
    c.set_grid(GridValue::Bar);
    std::string why;

    // Nudge B a bar earlier: room opens at 3 bars, B lands there, the gap it
    // left closes, so C ends where it was.
    CHECK(c.select_region(11));
    CHECK(c.nudge(-1, why));
    CHECK(start_of(doc.snapshot(), 11) == 3 * kBar);   // a bar earlier
    CHECK(start_of(doc.snapshot(), 12) == 6 * kBar && doc.history().undo_depth() == 1);
    CHECK(valid(doc.snapshot()));
    CHECK(c.undo(why) && !doc.can_undo());

    // Drag B two bars later: C closes up over the place B left.
    const double th = c.view().track_height;
    const double lower = th * 0.75;
    const double x = c.view().x_of(4 * kBar + kBar / 2);
    CHECK(c.press(x, lower, why));
    c.move(x + (c.view().x_of(6 * kBar) - c.view().x_of(4 * kBar)), lower);
    CHECK(c.release(x + (c.view().x_of(6 * kBar) - c.view().x_of(4 * kBar)), lower, why));
    CHECK(start_of(doc.snapshot(), 12) == 5 * kBar);
    CHECK(start_of(doc.snapshot(), 11) >= end_of(doc.snapshot(), 12));
    CHECK(doc.history().undo_depth() == 1 && valid(doc.snapshot()));
    CHECK(c.undo(why) && !doc.can_undo());

    // Drag B's end half a bar earlier: C keeps the gap it had to B.
    const double edge = c.view().x_of(5 * kBar) - 2;
    CHECK(c.hit(edge, lower).zone == HitZone::TrimEnd);
    CHECK(c.press(edge, lower, why));
    const double to = edge - (c.view().x_of(5 * kBar) - c.view().x_of(4 * kBar + kBar / 2));
    c.move(to, lower);
    CHECK(c.release(to, lower, why));
    CHECK(end_of(doc.snapshot(), 11) < 5 * kBar);
    CHECK(start_of(doc.snapshot(), 12) - end_of(doc.snapshot(), 11) == kBar);
    CHECK(doc.history().undo_depth() == 1 && valid(doc.snapshot()));
    CHECK(c.undo(why) && !doc.can_undo());

    // A click without a move changes nothing.
    CHECK(c.press(x, lower, why) && c.release(x, lower, why));
    CHECK(!doc.can_undo());
    return 0;
}

int test_shuffle_loop_ripples() {
    ModelDocument doc(three_regions());
    EditController c(doc);
    c.set_mode(EditMode::Shuffle);
    std::string why;
    // Loop B twice: two bars of room open, C moves up behind the repeats.
    CHECK(c.select_region(11));
    CHECK(c.loop_clip(2, why));
    CHECK(lane(doc.snapshot(), 0).regions.size() == 5);
    CHECK(start_of(doc.snapshot(), 12) == 8 * kBar && valid(doc.snapshot()));
    CHECK(doc.history().undo_depth() == 1 && doc.undo_label() == "Loop Clip");
    // Looping again with one repeat gives a bar back.
    CHECK(c.select_region(11));
    CHECK(c.loop_clip(1, why));
    CHECK(start_of(doc.snapshot(), 12) == 7 * kBar && valid(doc.snapshot()));
    return 0;
}

// Shuffle moves regions, not the automation under them: the notice says so,
// naming the track.
int test_shuffle_automation_notice() {
    ModelDocument doc(two_regions());
    EditController c(doc);
    c.set_viewport(4000, 400);
    std::string why;
    c.set_mode(EditMode::Shuffle);
    c.select_track(0);
    // A volume breakpoint, through the Pencil's own operation.
    model::EditAutomation e;
    e.track = doc.snapshot().edit->tracks[0]->id;
    e.param = AutomationParam::Volume;
    e.from = 0;
    e.to = 8 * kBar;
    e.points = {{kBar, -6.0f}};
    CHECK(doc.submit(e, why));
    CHECK(c.select_region(10));
    CHECK(c.clear(why));
    const std::string notice = c.take_follow_notice();
    CHECK(has(notice, "Shuffle Clear") && has(notice, doc.snapshot().edit->tracks[0]->name.c_str()));
    CHECK(c.take_follow_notice().empty());
    return 0;
}

// --- fades and crossfades travel with what holds them -----------------------

int test_fades_and_crossfades() {
    Region a = make_region(10, "A", kBar, 2 * kBar);
    a.fade_in = Fade{1000, FadeShape::Linear};
    a.fade_out = Fade{2000, FadeShape::SCurve};
    Region b = make_region(11, "B", 3 * kBar, kBar);   // butts A
    Crossfade x;
    x.left = 10;
    x.right = 11;
    x.length = 4800;
    ModelDocument doc(session_with({a, b}, {x}));
    CHECK(valid(doc.snapshot()));
    EditController c(doc);
    c.set_viewport(4000, 400);
    c.scroll_to_frame(0);
    std::string why;

    // From A's middle to B's middle: A's fade-in is cut away, the crossfade
    // is held whole.
    select_range(c, 0, 2 * kBar, 3 * kBar + kBar / 2);
    CHECK(c.copy(why));
    CHECK(c.clipboard().lanes[0].regions[0].fade_in.length == 0);
    CHECK(c.clipboard().lanes[0].crossfades.size() == 1);
    c.select_track(1);
    c.set_insertion(10 * kBar);
    CHECK(c.paste(why));
    const Playlist& p = lane(doc.snapshot(), 1);
    CHECK(p.regions.size() == 2 && p.crossfades.size() == 1);
    CHECK(p.crossfades[0].left == p.regions[0].id && p.crossfades[0].right == p.regions[1].id);
    CHECK(p.crossfades[0].length == 4800);
    CHECK(valid(doc.snapshot()));

    // A range that ends inside the crossfade does not take it.
    select_range(c, 0, 2 * kBar, 3 * kBar + 100);
    CHECK(c.copy(why));
    CHECK(c.clipboard().lanes[0].crossfades.empty());
    return 0;
}

// A region the range cuts at one end loses the fade on the new edge, as
// overwrite and copy_range do: a fade left there would fade to silence
// audio that used to play at full level. Its other fade is kept.
int check_cut_edges(const char* gesture, bool tail) {
    Region a = make_region(10, "A", kBar, 2 * kBar);   // [1, 3) bars
    a.fade_in = Fade{1000, FadeShape::Linear};
    a.fade_out = Fade{2000, FadeShape::SCurve};
    Region src = make_region(11, "S", 10 * kBar, kBar);   // what Paste brings
    ModelDocument doc(session_with({a, src}));
    EditController c(doc);
    c.set_viewport(4000, 400);
    c.scroll_to_frame(0);
    std::string why;
    const std::string g = gesture;
    // The range covers A's tail (from bar 2) or its head (up to bar 2).
    const Frames from = tail ? 2 * kBar : 0, to = tail ? 4 * kBar : 2 * kBar;
    if (g == "paste") {
        CHECK(c.select_region(11));
        CHECK(c.copy(why));
        // One bar pasted over A's tail [2, 3) or its head [1, 2).
        c.select_track(0);
        c.set_insertion(tail ? 2 * kBar : kBar);
        CHECK(c.paste(why));
    } else {
        select_range(c, 0, from, to);
        CHECK(g == "cut" ? c.cut(why) : c.clear(why));
    }
    CHECK(doc.history().undo_depth() == 1);
    const Playlist& p = lane(doc.snapshot(), 0);
    const auto it = std::find_if(p.regions.begin(), p.regions.end(),
                                 [](const Region& r) { return r.id == 10; });
    CHECK(it != p.regions.end());
    if (tail) {
        CHECK(it->end() == 2 * kBar);
        CHECK(it->fade_out.length == 0);
        CHECK(it->fade_in.length == 1000);
    } else {
        CHECK(it->position == 2 * kBar);
        CHECK(it->fade_in.length == 0);
        CHECK(it->fade_out.length == 2000);
    }
    CHECK(valid(doc.snapshot()));
    CHECK(c.undo(why));
    const Playlist& back = lane(doc.snapshot(), 0);
    CHECK(back.regions[0].fade_in.length == 1000 && back.regions[0].fade_out.length == 2000);
    return 0;
}

// PR #86 second review: the trim clamps fades against each other, so the
// cut edge's fade must be gone before it, or the kept fade is shortened by
// one that no longer exists. A 10000-frame region, fade-in 4000 and
// fade-out 3000, cut to 5000 frames keeps all of its uncut fade.
int check_kept_fade_whole(bool tail) {
    Region a = make_region(10, "A", kBar, 10000);
    a.fade_in = Fade{4000, FadeShape::Linear};
    a.fade_out = Fade{3000, FadeShape::Linear};
    ModelDocument doc(session_with({a}));
    EditController c(doc);
    c.set_viewport(4000, 400);
    c.scroll_to_frame(0);
    std::string why;
    if (tail) {
        c.select_range(0, kBar + 5000, 2 * kBar);
    } else {
        c.select_range(0, 0, kBar + 5000);
    }
    CHECK(c.clear(why));
    const Region& r = lane(doc.snapshot(), 0).regions.at(0);
    CHECK(r.length == 5000);
    if (tail) {
        CHECK(r.fade_out.length == 0 && r.fade_in.length == 4000);
    } else {
        CHECK(r.fade_in.length == 0 && r.fade_out.length == 3000);
    }
    CHECK(valid(doc.snapshot()));
    return 0;
}

int test_cut_edges_lose_fades() {
    if (check_kept_fade_whole(false) || check_kept_fade_whole(true)) return 1;
    for (const char* g : {"clear", "cut", "paste"}) {
        for (bool tail : {false, true}) {
            if (check_cut_edges(g, tail)) {
                std::fprintf(stderr, "  in %s at the %s\n", g, tail ? "tail" : "head");
                return 1;
            }
        }
    }
    return 0;
}

// --- repeat --------------------------------------------------------------------

int test_repeat() {
    ModelDocument doc(two_regions());
    EditController c(doc);
    c.set_viewport(4000, 400);
    c.scroll_to_frame(0);
    std::string why;

    // Nothing to repeat: refused before any count is asked for.
    c.select_track(0);
    c.set_insertion(0);
    CHECK(!c.can_repeat(why) && has(why, "nothing was repeated"));

    // B [4, 5) three times: copies at bars 5, 6 and 7, one undo step, and
    // the copies become the selection.
    CHECK(c.select_region(11));
    std::string name;
    Frames length = 0;
    CHECK(c.can_repeat(why, &name, &length) && length == kBar);
    CHECK(name == doc.snapshot().edit->tracks[0]->name);
    const uint64_t clip = c.clipboard_serial();
    CHECK(c.repeat(3, why));
    {
        const Playlist& p = lane(doc.snapshot(), 0);
        CHECK(p.regions.size() == 5);
        for (int i = 0; i < 3; ++i) {
            const Region& r = p.regions[static_cast<std::size_t>(2 + i)];
            CHECK(r.position == (5 + i) * kBar && r.length == kBar && r.source_offset == 4 * kBar);
            CHECK(r.id != 11);
        }
    }
    CHECK(doc.history().undo_depth() == 1 && doc.undo_label() == "Repeat");
    CHECK(c.selection().start == 5 * kBar && c.selection().end == 8 * kBar);
    CHECK(c.repeat_count() == 3 && c.clipboard_serial() == clip);
    CHECK(valid(doc.snapshot()));
    CHECK(c.undo(why) && lane(doc.snapshot(), 0).regions.size() == 2 && !doc.can_undo());

    // A [1, 3) twice: the copies cover [3, 7) and replace B under them.
    CHECK(c.select_region(10));
    CHECK(c.repeat(2, why));
    {
        const Playlist& p = lane(doc.snapshot(), 0);
        CHECK(p.regions.size() == 3);
        CHECK(std::none_of(p.regions.begin(), p.regions.end(),
                           [](const Region& r) { return r.id == 11; }));
        CHECK(p.regions[1].position == 3 * kBar && p.regions[2].position == 5 * kBar);
    }
    CHECK(valid(doc.snapshot()));
    CHECK(c.undo(why));

    // A count below one, and Shuffle: refused by name, nothing sent, the
    // count offered next unchanged.
    const uint64_t r0 = doc.revision();
    CHECK(c.select_region(11));
    CHECK(!c.repeat(0, why) && has(why, "1 or more") && has(why, "nothing was repeated"));
    CHECK(!c.repeat(-2, why) && has(why, "1 or more"));
    CHECK(doc.revision() == r0 && c.repeat_count() == 2);

    // A crossfade held in the range comes with every copy, between that
    // copy's own regions.
    Region a = make_region(20, "A", kBar, kBar);
    Region b = make_region(21, "B", 2 * kBar, kBar);   // butts A
    Crossfade x;
    x.left = 20;
    x.right = 21;
    x.length = 4800;
    ModelDocument xdoc(session_with({a, b}, {x}));
    EditController xc(xdoc);
    xc.set_viewport(4000, 400);
    xc.scroll_to_frame(0);
    select_range(xc, 0, kBar, 3 * kBar);
    CHECK(xc.repeat(2, why));
    const Playlist& xp = lane(xdoc.snapshot(), 0);
    CHECK(xp.regions.size() == 6 && xp.crossfades.size() == 3);
    for (const Crossfade& f : xp.crossfades) {
        const auto left = std::find_if(xp.regions.begin(), xp.regions.end(),
                                       [&f](const Region& r) { return r.id == f.left; });
        const auto right = std::find_if(xp.regions.begin(), xp.regions.end(),
                                        [&f](const Region& r) { return r.id == f.right; });
        CHECK(left != xp.regions.end() && right != xp.regions.end());
        CHECK(left->end() == right->position);
    }
    CHECK(valid(xdoc.snapshot()));
    return 0;
}

int test_repeat_count_text() {
    int n = 0;
    CHECK(parse_repeat_count("3", n) && n == 3);
    CHECK(parse_repeat_count(" 12 ", n) && n == 12);
    CHECK(parse_repeat_count("+4", n) && n == 4);
    CHECK(parse_repeat_count("0", n) && n == 0);
    CHECK(parse_repeat_count("-1", n) && n == -1);
    CHECK(parse_repeat_count("2147483647", n) && n == 2147483647);
    CHECK(!parse_repeat_count("", n));
    CHECK(!parse_repeat_count(" ", n));
    CHECK(!parse_repeat_count("-", n));
    CHECK(!parse_repeat_count("3x", n));
    CHECK(!parse_repeat_count("2.5", n));
    CHECK(!parse_repeat_count("1 2", n));
    CHECK(!parse_repeat_count("2147483648", n));
    CHECK(!parse_repeat_count("99999999999999999999", n));
    return 0;
}

// --- the member limit -------------------------------------------------------
//
// Track 1 holds kMaxCompoundMembers + 44 one-thousand-frame regions, butted.
// Each gesture is taken to the limit, which it reaches, and one past, which
// it refuses unsent with the counts and changes nothing.

constexpr Frames kStep = 1000;
constexpr std::size_t kLimit = kMaxCompoundMembers;

Snapshot dense() {
    std::vector<Region> rs;
    for (std::size_t i = 0; i < kLimit + 44; ++i) {
        rs.push_back(make_region(static_cast<Id>(200 + i), "", static_cast<Frames>(i) * kStep, kStep));
    }
    Snapshot s = session_with(std::move(rs));
    s.next_id = 2000;
    return s;
}

Frames span(std::size_t regions) { return static_cast<Frames>(regions) * kStep; }

int test_limit() {
    ModelDocument doc(dense());
    CHECK(valid(doc.snapshot()));
    EditController c(doc);
    c.set_viewport(4000, 400);
    c.scroll_to_frame(0);
    std::string why;
    const std::string limit = std::to_string(kLimit);

    // Paste: one PlaceRegion a region onto an empty track.
    select_range(c, 0, 0, span(kLimit));
    CHECK(c.copy(why));
    c.select_track(1);
    c.set_insertion(0);
    CHECK(c.paste(why));
    CHECK(lane(doc.snapshot(), 1).regions.size() == kLimit && doc.history().undo_depth() == 1);
    CHECK(c.undo(why));
    select_range(c, 0, 0, span(kLimit + 1));
    CHECK(c.copy(why));
    c.select_track(1);
    c.set_insertion(0);
    uint64_t r0 = doc.revision();
    CHECK(!c.paste(why));
    CHECK(why == "Paste needs " + std::to_string(kLimit + 1) + " changes; at most " + limit +
                     " in one edit. Nothing was pasted.");
    CHECK(doc.revision() == r0 && lane(doc.snapshot(), 1).regions.empty());

    // One lane onto two tracks counts both tracks' placements: half the
    // limit each fits, one more each does not.
    const Frames far = 2 * span(kLimit + 44);
    select_range(c, 0, 0, span(kLimit / 2));
    CHECK(c.copy(why));
    c.select_range(0, far, far + kStep, 2);
    CHECK(c.paste(why));
    CHECK(lane(doc.snapshot(), 1).regions.size() == kLimit / 2 && doc.history().undo_depth() == 1);
    CHECK(c.undo(why));
    select_range(c, 0, 0, span(kLimit / 2 + 1));
    CHECK(c.copy(why));
    c.select_range(0, far, far + kStep, 2);
    r0 = doc.revision();
    CHECK(!c.paste(why));
    CHECK(why == "Paste needs " + std::to_string(kLimit + 2) + " changes; at most " + limit +
                     " in one edit. Nothing was pasted.");
    CHECK(doc.revision() == r0 && lane(doc.snapshot(), 1).regions.empty());

    // Cut: one RemoveRegion a region. A refused Cut leaves the clipboard.
    select_range(c, 0, 0, span(kLimit));
    CHECK(c.cut(why));
    CHECK(lane(doc.snapshot(), 0).regions.size() == 44);
    CHECK(c.undo(why));
    const uint64_t clip = c.clipboard_serial();
    select_range(c, 0, 0, span(kLimit + 1));
    r0 = doc.revision();
    CHECK(!c.cut(why) && has(why, "Cut needs ") && has(why, "Nothing was cut."));
    CHECK(doc.revision() == r0 && c.clipboard_serial() == clip);

    // Clear, the same.
    select_range(c, 0, 0, span(kLimit));
    CHECK(c.clear(why));
    CHECK(c.undo(why));
    select_range(c, 0, 0, span(kLimit + 1));
    r0 = doc.revision();
    CHECK(!c.clear(why) && has(why, "Nothing was cleared."));
    CHECK(doc.revision() == r0);

    // Duplicate onto regions: one removal and one placement a region.
    select_range(c, 0, 0, span(kLimit / 2));
    CHECK(c.duplicate(why));
    CHECK(doc.history().undo_depth() == 1);
    CHECK(valid(doc.snapshot()));
    CHECK(c.undo(why));
    select_range(c, 0, 0, span(kLimit / 2 + 1));
    r0 = doc.revision();
    CHECK(!c.duplicate(why));
    CHECK(why == "Duplicate needs " + std::to_string(kLimit + 2) + " changes; at most " + limit +
                     " in one edit. Nothing was duplicated.");
    CHECK(doc.revision() == r0 && !doc.can_undo());

    // Repeat of the first region over the rest: a removal and a placement a
    // copy, counted before anything is built, so even an int's worth of
    // copies is refused by its size rather than allocated.
    c.select_range(0, 0, span(1));   // under a pixel: set as a drag leaves it
    CHECK(c.repeat(static_cast<int>(kLimit / 2), why));
    CHECK(doc.history().undo_depth() == 1 && valid(doc.snapshot()));
    CHECK(c.undo(why));
    c.select_range(0, 0, span(1));   // under a pixel: set as a drag leaves it
    r0 = doc.revision();
    CHECK(!c.repeat(static_cast<int>(kLimit / 2 + 1), why));
    CHECK(why == "Repeat needs " + std::to_string(kLimit + 2) + " changes; at most " + limit +
                     " in one edit. Nothing was repeated.");
    CHECK(!c.repeat(2147483647, why));
    CHECK(why == "Repeat needs " + std::to_string(kLimit + 43 + 2147483647ull) +
                     " changes; at most " + limit + " in one edit. Nothing was repeated.");
    CHECK(doc.revision() == r0 && !doc.can_undo());
    return 0;
}

// --- several tracks ---------------------------------------------------------

// Three mono tracks: A [1, 3) bars on the first, nothing on the second, C
// [1, 2) bars on the third, all over source 100.
Snapshot three_tracks() {
    Snapshot s = make_empty_session(3, kRate);
    auto pool = std::make_shared<SourcePool>();
    Source take;
    take.id = 100;
    take.path = "Audio Files/Take 1.wav";
    take.length = 40 * kBar;
    pool->sources.push_back(take);
    s.sources = pool;
    s.next_id = 1000;
    auto edit = std::make_shared<EditState>(*s.edit);
    const auto fill = [&edit](std::size_t i, std::vector<Region> regions) {
        auto t = std::make_shared<TrackEdit>(*edit->tracks[i]);
        auto p = std::make_shared<Playlist>(*t->playlists[0]);
        p->regions = std::move(regions);
        t->playlists[0] = p;
        edit->tracks[i] = t;
    };
    fill(0, {make_region(10, "A", kBar, 2 * kBar)});
    fill(2, {make_region(12, "C", kBar, kBar)});
    auto keys = std::make_shared<TrackEdit>(*edit->tracks[2]);
    keys->name = "Keys";
    edit->tracks[2] = keys;
    s.edit = edit;
    return s;
}

const std::string& name_of(const Snapshot& s, int track) {
    return s.edit->tracks[static_cast<std::size_t>(track)]->name;
}

int test_several_tracks() {
    ModelDocument doc(three_tracks());
    CHECK(valid(doc.snapshot()));
    EditController c(doc);
    c.set_viewport(4000, 400);
    c.scroll_to_frame(0);
    std::string why;

    // A Selector drag from the third lane up to the first covers all three,
    // whichever way it went.
    c.set_tool(Tool::Select);
    const double x0 = c.view().x_of(kBar + kBar / 2), x1 = c.view().x_of(2 * kBar + kBar / 2);
    CHECK(c.press(x0, c.view().y_of_track(2) + 4, why));
    c.move(x1, c.view().y_of_track(0) + 4);
    CHECK(c.drag().track_end == 0);
    CHECK(c.release(x1, c.view().y_of_track(0) + 4, why));
    c.set_tool(Tool::Smart);
    CHECK(c.selection().track == 0 && c.selection().tracks == 3);
    CHECK(c.selection().start == kBar + kBar / 2 && c.selection().end == 2 * kBar + kBar / 2);
    // A click on one track is one track again.
    c.select_track(1);
    CHECK(c.selection().tracks == 1 && !c.selection().has_range());

    // Copy: one lane a track, the empty one keeping its place.
    c.select_range(0, kBar + kBar / 2, 2 * kBar + kBar / 2, 3);
    CHECK(c.copy(why));
    CHECK(c.clipboard().lanes.size() == 3 && c.clipboard().length() == kBar);
    CHECK(c.clipboard().lanes[0].regions.size() == 1 && c.clipboard().lanes[1].empty());
    CHECK(c.clipboard().lanes[2].regions.size() == 1 &&
          c.clipboard().lanes[2].regions[0].length == kBar / 2);
    CHECK(c.clipboard().lanes[2].track == "Keys");

    // Paste at bar 8 from the first track: each lane on its own track, one
    // compound, one undo step.
    c.select_track(0);
    c.set_insertion(8 * kBar);
    CHECK(c.paste(why) && why.empty());
    CHECK(lane(doc.snapshot(), 0).regions.size() == 2 && lane(doc.snapshot(), 0).regions[1].position == 8 * kBar);
    CHECK(lane(doc.snapshot(), 1).regions.empty());
    CHECK(lane(doc.snapshot(), 2).regions.size() == 2 && lane(doc.snapshot(), 2).regions[1].position == 8 * kBar);
    CHECK(doc.history().undo_depth() == 1 && doc.undo_label() == "Paste");
    CHECK(c.selection().track == 0 && c.selection().tracks == 3 &&
          c.selection().start == 8 * kBar && c.selection().end == 9 * kBar);
    CHECK(valid(doc.snapshot()));
    CHECK(c.undo(why) && !doc.can_undo());
    CHECK(lane(doc.snapshot(), 0).regions.size() == 1 && lane(doc.snapshot(), 2).regions.size() == 1);

    // From the second track, the third lane has no track to land on: Pro
    // Tools leaves it out and pastes the rest; the paste says what it left.
    // The empty lane clears what it lands on, as its gap would.
    c.select_track(1);
    c.set_insertion(kBar);
    CHECK(c.paste(why));
    CHECK(has(why, "Pasted 2 of the clipboard's 3 tracks") && has(why, "no track below Keys") &&
          has(why, "copied from Keys"));
    CHECK(lane(doc.snapshot(), 1).regions.size() == 1 && lane(doc.snapshot(), 1).regions[0].position == kBar);
    CHECK(lane(doc.snapshot(), 2).regions.empty());   // C was under the empty lane
    CHECK(c.selection().track == 1 && c.selection().tracks == 2);
    CHECK(doc.history().undo_depth() == 1 && valid(doc.snapshot()));
    CHECK(c.undo(why));

    // From the third track the empty second lane is left out too, but it
    // held nothing, so only Keys' lane is named.
    c.select_track(2);
    c.set_insertion(12 * kBar);
    CHECK(c.paste(why));
    CHECK(has(why, "Pasted 1 of the clipboard's 3 tracks") && has(why, "copied from Keys"));
    CHECK(!has(why, name_of(doc.snapshot(), 1).c_str()));
    CHECK(c.undo(why));

    // One track's clipboard into a selection on several fills each of them,
    // as Pro Tools does: one compound, one undo step, the selection kept.
    c.select_range(0, kBar, 2 * kBar, 1);
    CHECK(c.copy(why) && c.clipboard().lanes.size() == 1);
    c.select_range(0, 10 * kBar, 11 * kBar, 3);
    CHECK(c.paste(why) && why.empty());
    for (int t = 0; t < 3; ++t) {
        CHECK(lane(doc.snapshot(), t).regions.back().position == 10 * kBar);
        CHECK(lane(doc.snapshot(), t).regions.back().length == kBar);
    }
    CHECK(lane(doc.snapshot(), 1).regions.size() == 1);
    CHECK(doc.history().undo_depth() == 1 && doc.undo_label() == "Paste");
    CHECK(c.selection().track == 0 && c.selection().tracks == 3 &&
          c.selection().start == 10 * kBar && c.selection().end == 11 * kBar);
    CHECK(valid(doc.snapshot()));
    CHECK(c.undo(why) && !doc.can_undo());
    // From an insertion point on two tracks, the two only.
    c.select_range(1, 12 * kBar, 12 * kBar, 2);
    CHECK(c.paste(why));
    CHECK(lane(doc.snapshot(), 0).regions.size() == 1);
    CHECK(lane(doc.snapshot(), 1).regions.size() == 1 && lane(doc.snapshot(), 2).regions.size() == 2);
    CHECK(c.undo(why));
    // A clipboard of several lanes keeps its layout on a wider selection.
    c.select_range(0, kBar, 2 * kBar, 2);
    CHECK(c.copy(why) && c.clipboard().lanes.size() == 2);
    c.select_range(0, 10 * kBar, 11 * kBar, 3);
    CHECK(c.paste(why));
    CHECK(lane(doc.snapshot(), 0).regions.size() == 2 && lane(doc.snapshot(), 2).regions.size() == 1);
    CHECK(c.undo(why));
    // A selected region is one track, whatever the clipboard.
    c.select_range(0, kBar, 2 * kBar, 1);
    CHECK(c.copy(why));
    CHECK(c.select_region(10));
    CHECK(c.paste(why) && c.selection().tracks == 1);
    CHECK(lane(doc.snapshot(), 2).regions.size() == 1);
    CHECK(c.undo(why) && !doc.can_undo());

    // Only empty lanes left out: nothing was lost, so nothing to say.
    c.select_range(0, kBar, 2 * kBar, 2);
    CHECK(c.copy(why) && c.clipboard().lanes.size() == 2 && c.clipboard().lanes[1].empty());
    c.select_track(2);
    c.set_insertion(12 * kBar);
    CHECK(c.paste(why) && why.empty());
    CHECK(c.undo(why));

    // A clipboard whose regions would all fall past the last track pastes
    // nothing, rather than only clearing.
    c.select_range(1, kBar, 2 * kBar, 2);
    CHECK(c.copy(why) && c.clipboard().lanes.size() == 2 && c.clipboard().lanes[0].empty());
    c.select_track(2);
    c.set_insertion(6 * kBar);
    uint64_t r0 = doc.revision();
    CHECK(!c.paste(why) && has(why, "copied from Keys") && has(why, "nothing was pasted"));
    CHECK(doc.revision() == r0);

    // Cut on the three: both regions lose the range, one step.
    c.select_range(0, kBar + kBar / 2, 2 * kBar + kBar / 2, 3);
    CHECK(c.cut(why));
    CHECK(doc.history().undo_depth() == 1 && doc.undo_label() == "Cut");
    CHECK(lane(doc.snapshot(), 0).regions.size() == 2);
    CHECK(lane(doc.snapshot(), 2).regions[0].end() == kBar + kBar / 2);
    CHECK(c.clipboard().lanes.size() == 3 && c.selection().tracks == 3);
    CHECK(c.undo(why) && !doc.can_undo());

    // Clear on the three, one step; on three empty lanes, refused.
    c.select_range(0, kBar + kBar / 2, 2 * kBar + kBar / 2, 3);
    CHECK(c.clear(why) && doc.history().undo_depth() == 1 && doc.undo_label() == "Clear");
    CHECK(lane(doc.snapshot(), 0).regions.size() == 2 && lane(doc.snapshot(), 2).regions[0].end() == kBar + kBar / 2);
    CHECK(c.undo(why));
    c.select_range(0, 20 * kBar, 21 * kBar, 3);
    CHECK(!c.clear(why) && has(why, "any of its 3 tracks") && has(why, "nothing was cleared"));
    CHECK(!c.copy(why) && has(why, "any of its 3 tracks"));

    // Duplicate: each track's part straight after the range, one step.
    c.select_range(0, kBar, 2 * kBar, 3);
    CHECK(c.duplicate(why) && doc.history().undo_depth() == 1 && doc.undo_label() == "Duplicate");
    CHECK(lane(doc.snapshot(), 2).regions.size() == 2 && lane(doc.snapshot(), 2).regions[1].position == 2 * kBar);
    CHECK(c.selection().tracks == 3 && c.selection().start == 2 * kBar && c.selection().end == 3 * kBar);
    CHECK(valid(doc.snapshot()));
    CHECK(c.undo(why));

    // Repeat twice: one step; the dialog names every track it repeats on.
    c.select_range(0, kBar, 2 * kBar, 3);
    std::string names;
    CHECK(c.can_repeat(why, &names) && names == name_of(doc.snapshot(), 0) + ", " +
                                                    name_of(doc.snapshot(), 1) + " and Keys");
    CHECK(c.repeat(2, why) && doc.history().undo_depth() == 1 && doc.undo_label() == "Repeat");
    CHECK(lane(doc.snapshot(), 2).regions.size() == 3 && lane(doc.snapshot(), 2).regions[2].position == 3 * kBar);
    CHECK(valid(doc.snapshot()));
    CHECK(c.undo(why));

    // The limit is the whole gesture's: 150 repeats fit one track's edit,
    // not two tracks' in one.
    c.select_range(0, kBar, 2 * kBar, 1);
    CHECK(c.repeat(150, why));
    CHECK(c.undo(why));
    c.select_range(0, kBar, 2 * kBar, 3);
    r0 = doc.revision();
    CHECK(!c.repeat(150, why) && has(why, "Repeat needs ") && has(why, "Nothing was repeated."));
    CHECK(doc.revision() == r0);

    // Separate on several tracks splits each at both ends, one step.
    c.select_range(0, kBar + kBar / 2, 2 * kBar + kBar / 2, 3);
    CHECK(c.separate(why) && doc.history().undo_depth() == 1);
    CHECK(lane(doc.snapshot(), 0).regions.size() == 3 && lane(doc.snapshot(), 2).regions.size() == 2);
    CHECK(c.undo(why));

    // One track's commands say so on several, rather than edit the top one.
    c.select_range(0, kBar / 2, kBar + kBar / 2, 3);
    r0 = doc.revision();
    CHECK(!c.fade_selection(why) && has(why, "on several tracks is not built yet"));
    CHECK(!c.fade_to_start(why) && has(why, "on several tracks"));
    CHECK(!c.trim_start_to_insertion(why) && has(why, "on several tracks"));
    CHECK(doc.revision() == r0);

    // Select All: every track, from the start to the last region's end.
    CHECK(c.select_all(why));
    CHECK(c.selection().track == 0 && c.selection().tracks == 3 && c.selection().start == 0 &&
          c.selection().end == 3 * kBar);
    return 0;
}

// --- the Edit menu, in the real QML -----------------------------------------

void key(QQuickView& view, int k, Qt::KeyboardModifiers m) {
    QKeyEvent press(QEvent::KeyPress, k, m, 0, 0, 0);
    QCoreApplication::sendEvent(&view, &press);
    QKeyEvent release(QEvent::KeyRelease, k, m, 0, 0, 0);
    QCoreApplication::sendEvent(&view, &release);
}

QQuickItem* named(QQuickItem* item, const QString& name) {
    if (item->objectName() == name) return item;
    for (QQuickItem* c : item->childItems()) {
        if (QQuickItem* f = named(c, name)) return f;
    }
    return nullptr;
}

// An Edit menu item below the first, once it is where it is drawn. The
// menu's Repeater rebuilds its rows when the undo text changes, and its
// Column places them at the next polish: until then every new row sits on
// the first, and a click meant for Duplicate lands on Undo.
QQuickItem* menu_item(QQuickItem* root, const QString& name) {
    QQuickItem* item = nullptr;
    QTest::qWaitFor([&]() {
        item = named(root, name);
        return item != nullptr && item->parentItem() != nullptr && item->parentItem()->y() > 0;
    });
    return item;
}

void click(QQuickView& view, QQuickItem* item) {
    const QPointF p = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
    QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, p.toPoint());
}

int test_menu() {
    KeyMap map;
    std::string err;
    CHECK(KeyMap::load_file(std::string(SONORA_KEYMAP_DIR) + "/protools.json", map, err));
    test::RecordingAdapter engine(2, api::testing::FakeEngine::engine_commands());
    ModelDocument document(two_regions());
    SyntheticPeakSource peaks(kRate, 120.0);
    SessionModel session(document, peaks, true);
    KeyRouter router(map, engine.info().implemented_commands,
                     [&engine](const std::string& c, const CommandContext& ctx) {
                         return engine.send(c, ctx);
                     });
    session.installCommands(router.local());
    router.setSelection([&session]() { return session.selectedTrackForCommands(); });
    router.prepare(qt_device_chords(true));
    ShellModel model(engine, router);

    QQuickView view;
    view.resize(1280, 720);
    view.setInitialProperties({{QStringLiteral("shell"), QVariant::fromValue(&model)},
                               {QStringLiteral("session"), QVariant::fromValue(&session)}});
    view.setSource(QUrl(QStringLiteral("qrc:/sonora/qml/Main.qml")));
    for (const auto& e : view.errors()) std::fprintf(stderr, "%s\n", qPrintable(e.toString()));
    CHECK(view.status() == QQuickView::Ready);
    model.routeKeysFrom(&view);
    view.show();
    CHECK(QTest::qWaitForWindowExposed(&view));

    CHECK(session.undoText() == QStringLiteral("Can't Undo"));
    CHECK(!session.canPaste());

    // Pro Tools' keys: Ctrl+C on A, then Ctrl+V on track 2.
    CHECK(session.controller().select_region(10));
    key(view, Qt::Key_C, Qt::ControlModifier);
    CHECK(session.canPaste());
    session.controller().select_track(1);
    session.controller().set_insertion(8 * kBar);
    key(view, Qt::Key_V, Qt::ControlModifier);
    CHECK(lane(document.snapshot(), 1).regions.size() == 1);
    CHECK(session.undoText() == QStringLiteral("Undo Paste"));

    // The menu says so, and its Undo sends edit.undo.
    QQuickItem* root = view.rootObject();
    QQuickItem* button = named(root, QStringLiteral("editMenuButton"));
    QQuickItem* menu = named(root, QStringLiteral("editMenu"));
    CHECK(button != nullptr && menu != nullptr);
    click(view, button);
    CHECK(menu->isVisible());
    QQuickItem* undo = named(root, QStringLiteral("editUndo"));
    CHECK(undo != nullptr && undo->property("text").toString() == QStringLiteral("Undo Paste"));
    click(view, undo);
    CHECK(!menu->isVisible());
    CHECK(lane(document.snapshot(), 1).regions.empty());
    CHECK(session.redoText() == QStringLiteral("Redo Paste"));

    // A single operation's step is said as its gesture, not its type.
    session.controller().set_insertion(kBar + kBar / 2);
    session.controller().select_track(0);
    std::string why;
    CHECK(session.controller().trim_start_to_insertion(why));
    session.documentUpdated();
    CHECK(session.undoText() == QStringLiteral("Undo Trim"));

    // Duplicate from the menu: what Ctrl+D sends.
    CHECK(session.controller().select_region(11));
    click(view, button);
    click(view, menu_item(root, QStringLiteral("editDuplicate")));
    CHECK(lane(document.snapshot(), 0).regions.size() == 3);
    CHECK(session.undoText() == QStringLiteral("Undo Duplicate"));
    CHECK(session.controller().undo(why));
    session.documentUpdated();

    // Repeat, Pro Tools' Alt+R: with nothing to repeat it says why and asks
    // for no number.
    QQuickItem* panel = named(root, QStringLiteral("repeatPanel"));
    QQuickItem* input = named(root, QStringLiteral("repeatInput"));
    CHECK(panel != nullptr && input != nullptr && !panel->isVisible());
    session.controller().select_track(1);
    session.controller().set_insertion(0);
    key(view, Qt::Key_R, Qt::AltModifier);
    CHECK(!panel->isVisible());
    CHECK(model.statusText().contains(QStringLiteral("nothing was repeated")));

    // On B it asks, offering the last count, and says what one copy is.
    CHECK(session.controller().select_region(11));
    key(view, Qt::Key_R, Qt::AltModifier);
    CHECK(panel->isVisible() && input->hasActiveFocus() && model.textEntry());
    CHECK(input->property("text").toString() == QStringLiteral("1"));
    CHECK(named(root, QStringLiteral("repeatWhat"))->property("text").toString()
              .contains(QString::fromStdString(" of " + document.snapshot().edit->tracks[0]->name)));
    // While the field has the keyboard, typing is the number, not keys.
    // Text that is no count leaves the dialog open with the reason.
    const uint64_t r0 = document.revision();
    QTest::keyClick(&view, 'x');
    QTest::keyClick(&view, Qt::Key_Return);
    CHECK(panel->isVisible() && document.revision() == r0);
    CHECK(model.statusText().contains(QStringLiteral("is not a number of repeats")));
    input->setProperty("text", QString());
    QTest::keyClick(&view, '3');
    QTest::keyClick(&view, Qt::Key_Return);
    CHECK(!panel->isVisible() && !model.textEntry());
    CHECK(lane(document.snapshot(), 0).regions.size() == 5);
    CHECK(session.undoText() == QStringLiteral("Undo Repeat"));

    // From the menu, which offers 3 now; Escape leaves it unsent.
    CHECK(session.controller().undo(why));
    session.documentUpdated();
    CHECK(session.controller().select_region(11));
    click(view, button);
    click(view, menu_item(root, QStringLiteral("editRepeat")));
    CHECK(panel->isVisible() && input->property("text").toString() == QStringLiteral("3"));
    QTest::keyClick(&view, Qt::Key_Escape);
    CHECK(!panel->isVisible() && !model.textEntry());
    CHECK(lane(document.snapshot(), 0).regions.size() == 2);

    // Spot: edit.spot with no time asks where the selected region starts,
    // offering it in the counter's format; a time that is none leaves the
    // dialog open, and the one typed places the region exactly there.
    QQuickItem* spot = named(root, QStringLiteral("spotPanel"));
    QQuickItem* spot_input = named(root, QStringLiteral("spotInput"));
    CHECK(spot != nullptr && spot_input != nullptr && !spot->isVisible());
    session.controller().set_mode(EditMode::Spot);
    CHECK(session.controller().select_region(11));
    model.invoke(QStringLiteral("edit.spot"), -1);
    CHECK(QTest::qWaitFor([&]() { return spot->isVisible(); }));
    CHECK(spot_input->hasActiveFocus() && model.textEntry());
    CHECK(spot_input->property("text").toString().startsWith(QStringLiteral("5|1")));
    const uint64_t spot_rev = document.revision();
    spot_input->setProperty("text", QStringLiteral("soon"));
    QTest::keyClick(&view, Qt::Key_Return);
    CHECK(spot->isVisible() && document.revision() == spot_rev);
    spot_input->setProperty("text", QStringLiteral("10|1"));
    QTest::keyClick(&view, Qt::Key_Return);
    CHECK(!spot->isVisible() && !model.textEntry());
    CHECK(lane(document.snapshot(), 0).regions.back().position == 9 * kBar);
    CHECK(session.controller().undo(why));
    session.documentUpdated();
    session.controller().set_mode(EditMode::Slip);

    // Ctrl+A, Pro Tools' Select All: both tracks. Copied, then pasted from
    // the second: the second lane, empty, is left out and nothing is said.
    key(view, Qt::Key_A, Qt::ControlModifier);
    CHECK(session.controller().selection().track == 0 &&
          session.controller().selection().tracks == 2);
    key(view, Qt::Key_C, Qt::ControlModifier);
    CHECK(session.controller().clipboard().lanes.size() == 2);
    session.controller().select_track(1);
    session.controller().set_insertion(20 * kBar);
    key(view, Qt::Key_V, Qt::ControlModifier);
    CHECK(lane(document.snapshot(), 1).regions.size() == 2);
    QTest::qWait(20);
    CHECK(!model.statusText().contains(QStringLiteral("Pasted")));
    // Again, now that the second track holds regions: the lane with no track
    // below is said on the status line.
    key(view, Qt::Key_A, Qt::ControlModifier);
    key(view, Qt::Key_C, Qt::ControlModifier);
    session.controller().select_track(1);
    session.controller().set_insertion(60 * kBar);
    key(view, Qt::Key_V, Qt::ControlModifier);
    CHECK(lane(document.snapshot(), 1).regions.size() == 4);
    CHECK(QTest::qWaitFor([&model]() {
        return model.statusText().contains(QStringLiteral("Pasted 1 of the clipboard's 2 tracks"));
    }));
    // Select All is on the Edit menu too.
    session.controller().select_track(1);
    click(view, button);
    click(view, menu_item(root, QStringLiteral("editSelectAll")));
    CHECK(session.controller().selection().tracks == 2);
    return 0;
}

}  // namespace

// --- Insert Time and Delete Time -------------------------------------------

// two_regions() with markers at 0.5, 3 and 4.5 bars and a Volume lane on
// track 1 running 0 dB at 0, -6 dB at 4 bars, 0 dB at 6 bars.
Snapshot with_markers_and_lane() {
    Snapshot s = two_regions();
    auto edit = std::make_shared<EditState>(*s.edit);
    MarkerList list;
    const std::vector<std::pair<Id, Frames>> where{
        {500, kBar / 2}, {501, 3 * kBar}, {502, 4 * kBar + kBar / 2}};
    for (const auto& [id, at] : where) {
        Marker m;
        m.id = id;
        m.position = at;
        m.name = "M" + std::to_string(id);
        list.markers.push_back(m);
    }
    edit->markers = std::make_shared<const MarkerList>(std::move(list));
    auto t = std::make_shared<TrackEdit>(*edit->tracks[0]);
    auto volume = std::make_shared<AutomationLane>();
    volume->param = AutomationParam::Volume;
    volume->points = {{0, 0.0f}, {4 * kBar, -6.0f}, {6 * kBar, 0.0f}};
    t->automation = {volume};
    edit->tracks[0] = t;
    s.edit = edit;
    s.next_id = 2000;
    return s;
}

Frames marker_at(const Snapshot& s, Id id) {
    for (const Marker& m : s.edit->markers->markers) {
        if (m.id == id) return m.position;
    }
    return -1;
}

int test_insert_delete_time() {
    ModelDocument doc(with_markers_and_lane());
    CHECK(valid(doc.snapshot()));
    EditController c(doc);
    c.set_viewport(4000, 400);
    c.scroll_to_frame(0);
    std::string why;
    const Frames a = 2 * kBar + kBar / 2, b = 3 * kBar + kBar / 2;

    // Nothing selected: refused by name, nothing sent.
    CHECK(!c.insert_time(why) && !why.empty() && doc.history().undo_depth() == 0);

    // Insert: A is split at the range's start; its right part, B, the
    // markers after the range and the lane all move up by a bar.
    c.select_range(0, a, b, 1);
    CHECK(c.insert_time(why));
    {
        const Snapshot& s = doc.snapshot();
        const Playlist& p = lane(s, 0);
        CHECK(p.regions.size() == 3);
        CHECK(p.regions[0].id == 10 && p.regions[0].end() == a);
        CHECK(p.regions[1].position == b && p.regions[1].end() == 4 * kBar);
        CHECK(p.regions[2].id == 11 && p.regions[2].position == 5 * kBar);
        CHECK(marker_at(s, 500) == kBar / 2);
        CHECK(marker_at(s, 501) == 4 * kBar);
        CHECK(marker_at(s, 502) == 5 * kBar + kBar / 2);
        const auto& pts = s.edit->tracks[0]->automation[0]->points;
        CHECK(pts.size() == 5);
        CHECK(pts[1].at == a && pts[2].at == b && pts[1].value == pts[2].value);
        CHECK(pts[3].at == 5 * kBar && pts[3].value == -6.0f && pts[4].at == 7 * kBar);
        CHECK(valid(s));
    }
    CHECK(doc.history().undo_depth() == 1 && doc.undo_label() == "Insert Time");
    // One undo puts back the regions, the markers and the lane together.
    CHECK(c.undo(why));
    CHECK(lane(doc.snapshot(), 0).regions.size() == 2 && start_of(doc.snapshot(), 11) == 4 * kBar);
    CHECK(marker_at(doc.snapshot(), 502) == 4 * kBar + kBar / 2);
    CHECK(doc.snapshot().edit->tracks[0]->automation[0]->points.size() == 3);

    // Delete: what the range covers goes, everything after closes up by it;
    // a marker inside the range lands on its start.
    c.select_range(0, a, b, 1);
    CHECK(c.delete_time(why));
    {
        const Snapshot& s = doc.snapshot();
        const Playlist& p = lane(s, 0);
        CHECK(p.regions.size() == 2);
        CHECK(p.regions[0].id == 10 && p.regions[0].end() == a);
        CHECK(p.regions[1].id == 11 && p.regions[1].position == 3 * kBar);
        CHECK(marker_at(s, 500) == kBar / 2);
        CHECK(marker_at(s, 501) == a);
        CHECK(marker_at(s, 502) == 3 * kBar + kBar / 2);
        const auto& pts = s.edit->tracks[0]->automation[0]->points;
        CHECK(pts.size() == 5);
        // The two sides join with a step at the range's start.
        CHECK(pts[1].at == a && pts[2].at == a && pts[1].value > pts[2].value);
        CHECK(pts[3].at == 3 * kBar && pts[3].value == -6.0f);
        CHECK(valid(s));
        CHECK(doc.undo_label() == "Delete Time");
    }
    CHECK(c.undo(why));
    CHECK(lane(doc.snapshot(), 0).regions.size() == 2 && start_of(doc.snapshot(), 11) == 4 * kBar);
    CHECK(marker_at(doc.snapshot(), 501) == 3 * kBar);

    // Two tracks: the other track's regions move too, in the same step.
    {
        Snapshot two = with_markers_and_lane();
        auto edit = std::make_shared<EditState>(*two.edit);
        auto t = std::make_shared<TrackEdit>(*edit->tracks[1]);
        auto p = std::make_shared<Playlist>(*t->playlists[0]);
        p->regions = {make_region(20, "C", 4 * kBar, kBar)};
        t->playlists[0] = p;
        edit->tracks[1] = t;
        two.edit = edit;
        ModelDocument d2(two);
        EditController c2(d2);
        c2.set_viewport(4000, 400);
        c2.scroll_to_frame(0);
        c2.select_range(0, a, b, 2);
        CHECK(c2.insert_time(why));
        CHECK(start_of(d2.snapshot(), 20, 1) == 5 * kBar);
        CHECK(d2.history().undo_depth() == 1);
    }
    return 0;
}

// clip.loop as the Loop Clip dialog will call it (ADR-0098): a typed count lays
// that many repeats as one compound and one "Loop Clip" undo step; text that is
// not a count is refused with the notice and changes nothing; an empty argument
// loops with the count last used.
int test_clip_loop_command() {
    ModelDocument doc(two_regions());
    SyntheticPeakSource peaks(kRate, 120.0);
    SessionModel session(doc, peaks, true);
    CommandRegistry local;
    session.installCommands(local);
    EditController& c = session.controller();
    std::string why;
    CommandContext none, arg;
    const std::size_t before = lane(doc.snapshot(), 0).regions.size();

    CHECK(c.select_region(11));
    arg.argument = "4";
    CHECK(local.dispatch("clip.loop", arg, why) == DispatchResult::Ok);
    CHECK(lane(doc.snapshot(), 0).regions.size() == before + 4);
    CHECK(doc.history().undo_depth() == 1 && doc.undo_label() == "Loop Clip");
    CHECK(c.undo(why));
    CHECK(lane(doc.snapshot(), 0).regions.size() == before && !doc.can_undo());

    arg.argument = "many";
    CHECK(local.dispatch("clip.loop", arg, why) == DispatchResult::Failed);
    CHECK(has(why, "not a number of repeats") && has(why, "Nothing was looped."));
    CHECK(lane(doc.snapshot(), 0).regions.size() == before && !doc.can_undo());

    CHECK(c.select_region(11));
    CHECK(local.dispatch("clip.loop", none, why) == DispatchResult::Ok);
    CHECK(lane(doc.snapshot(), 0).regions.size() == before + 4);
    CHECK(doc.history().undo_depth() == 1 && doc.undo_label() == "Loop Clip");
    return 0;
}

// Paste across sessions (ADR-0142): the clipboard keeps its origin through a
// session open; the audio is brought in first, as AddSource members ahead of
// the paste in the same Compound; refusals send nothing.
int test_paste_across_sessions() {
    ModelDocument doc(two_regions());
    EditController c(doc);
    std::string folder = "/music/Song A";
    c.set_session_folder_source([&folder] { return folder; });
    c.set_viewport(4000, 400);
    c.scroll_to_frame(0);
    std::string why;
    CHECK(c.select_region(10));
    CHECK(c.copy(why));
    CHECK(c.clipboard().origin_folder == "/music/Song A" && c.clipboard().origin_sample_rate == kRate);

    // Another session, without source 100.
    Snapshot other = make_empty_session(2, kRate);
    other.next_id = 500;
    CHECK(doc.reset(other, why));
    c.document_replaced();
    folder = "/music/Song B";
    CHECK(c.clipboard().origin_folder == "/music/Song A");   // kept

    int calls = 0;
    bool refuse = false;
    c.set_bring_in([&](const Snapshot& dest, const std::vector<Source>& sources,
                       const std::string& origin, std::uint32_t, BroughtIn& out, std::string& w) {
        ++calls;
        if (refuse) {
            w = "Take.wav is not readable in " + origin + "; nothing was pasted";
            return false;
        }
        Source s = sources.at(0);
        s.id = dest.next_id;
        out.ops.push_back(AddSource{s});
        out.remap[sources[0].id] = s.id;
        out.next_id = s.id + 1;
        return true;
    });
    c.select_track(1);
    c.set_insertion(0);
    refuse = true;
    CHECK(!c.paste(why) && has(why, "not readable") && calls == 1);
    CHECK(doc.history().undo_depth() == 0 && doc.snapshot().sources->sources.empty());
    refuse = false;
    c.set_recording(true);
    const int before = calls;
    CHECK(!c.paste(why) && has(why, "Stop recording") && calls == before);
    c.set_recording(false);
    CHECK(c.paste(why));
    CHECK(doc.snapshot().sources->sources.size() == 1);
    const Playlist& pl = lane(doc.snapshot(), 1);
    CHECK(pl.regions.size() == 1 && pl.regions[0].channels[0].source == 500);
    CHECK(pl.regions[0].id > 500);
    CHECK(doc.history().undo_depth() == 1 && doc.undo_label() == "Paste");
    CHECK(valid(doc.snapshot()));
    CHECK(c.undo(why));
    CHECK(lane(doc.snapshot(), 1).regions.empty());
    CHECK(doc.snapshot().sources->sources.size() == 1);   // the audio stays
    return 0;
}

// --- cross-session paste: the fixture and its fake copier ---------------------
// The planner and the disk copies are tested in common (test_paste_sources);
// these cases pin what the controller does with what the seam answers. The fake
// follows the planner's rules (umid reuse, " 2" on a taken name, origin gone
// refuses) over an in-memory file list, so nothing touches a disk or a clock.
// A separate test file for this waits for ui/qt/CMakeLists.txt to free up.

struct CrossSession {
    ModelDocument doc;
    EditController c;
    std::string folder = "/music/Song A";
    std::set<std::string> dest_files;
    bool origin_exists = true;
    std::vector<std::string> copies;   // files the fake copier wrote
    int calls = 0;
    std::string why;

    explicit CrossSession(Snapshot start = umid_session(two_regions()))
        : doc(std::move(start)), c(doc) {
        c.set_viewport(4000, 400);
        c.scroll_to_frame(0);
        c.set_session_folder_source([this] { return folder; });
        c.set_bring_in([this](const Snapshot& dest, const std::vector<Source>& sources,
                              const std::string&, std::uint32_t, BroughtIn& out, std::string& w) {
            return plan(dest, sources, out, w);
        });
    }

    static Snapshot umid_session(Snapshot s) {
        auto pool = std::make_shared<SourcePool>(*s.sources);
        pool->sources[0].umid = std::string(64, 'a');
        s.sources = pool;
        return s;
    }

    bool plan(const Snapshot& dest, const std::vector<Source>& sources, BroughtIn& out,
              std::string& w) {
        ++calls;
        if (!origin_exists) {
            w = "the folder " + folder + " is gone; nothing was pasted";
            return false;
        }
        Id next = dest.next_id;
        for (const Source& s : sources) {
            const auto same = std::find_if(
                dest.sources->sources.begin(), dest.sources->sources.end(),
                [&s](const Source& d) { return !s.umid.empty() && d.umid == s.umid; });
            if (same != dest.sources->sources.end()) {   // reused: no copy, no AddSource
                out.remap[s.id] = same->id;
                continue;
            }
            Source n = s;
            n.id = next++;
            while (dest_files.count(n.path) != 0) n.path.insert(n.path.size() - 4, " 2");
            dest_files.insert(n.path);
            copies.push_back(n.path);
            out.ops.push_back(AddSource{n});
            out.remap[s.id] = n.id;
        }
        out.next_id = next;
        return true;
    }

    // Open `other` in place of this session (the clipboard stays), ready to paste on track 2.
    bool open_other(Snapshot other) {
        other.next_id = 500;
        if (!doc.reset(other, why)) return false;
        c.document_replaced();
        folder = "/music/Song B";
        c.select_track(1);
        c.set_insertion(0);
        return true;
    }
    bool copy_region_and_open(Snapshot other) {
        return c.select_region(10) && c.copy(why) && open_other(std::move(other));
    }
};

Snapshot empty_other() { return make_empty_session(2, kRate); }

int test_paste_across_sessions_arrive() {
    CrossSession x;
    CHECK(x.copy_region_and_open(empty_other()));
    CHECK(x.c.paste(x.why));
    CHECK(x.copies.size() == 1 && x.copies[0] == "Audio Files/Take 1.wav");
    CHECK(x.doc.snapshot().sources->sources.size() == 1);
    const Playlist& pl = lane(x.doc.snapshot(), 1);
    CHECK(pl.regions.size() == 1 && pl.regions[0].channels[0].source == 500);
    CHECK(x.doc.history().undo_depth() == 1 && x.doc.undo_label() == "Paste");
    CHECK(valid(x.doc.snapshot()));
    CHECK(x.c.undo(x.why));
    CHECK(lane(x.doc.snapshot(), 1).regions.empty());
    CHECK(x.doc.snapshot().sources->sources.size() == 1);   // the audio stays
    CHECK(!x.doc.can_undo());
    return 0;
}

int test_paste_across_sessions_reuse_and_rename() {
    {   // The same umid is already here: reused, nothing copied or added.
        CrossSession x;
        Snapshot other = empty_other();
        auto pool = std::make_shared<SourcePool>();
        Source have;
        have.id = 77;
        have.path = "Audio Files/Elsewhere.wav";
        have.length = 40 * kBar;
        have.umid = std::string(64, 'a');
        pool->sources.push_back(have);
        other.sources = pool;
        CHECK(x.copy_region_and_open(other));
        CHECK(x.c.paste(x.why));
        CHECK(x.copies.empty() && x.doc.snapshot().sources->sources.size() == 1);
        CHECK(lane(x.doc.snapshot(), 1).regions.at(0).channels[0].source == 77);
        CHECK(valid(x.doc.snapshot()));
    }
    {   // A different file already holds the name: the arrival is "... 2".
        CrossSession x;
        x.dest_files.insert("Audio Files/Take 1.wav");
        CHECK(x.copy_region_and_open(empty_other()));
        CHECK(x.c.paste(x.why));
        CHECK(x.copies.size() == 1 && x.copies[0] == "Audio Files/Take 1 2.wav");
        CHECK(x.doc.snapshot().sources->sources.at(0).path == "Audio Files/Take 1 2.wav");
    }
    return 0;
}

int test_paste_across_sessions_refusals() {
    {   // The origin folder is gone: held refusal, nothing sent, nothing copied.
        CrossSession x;
        CHECK(x.copy_region_and_open(empty_other()));
        x.origin_exists = false;
        const uint64_t r0 = x.doc.revision();
        CHECK(!x.c.paste(x.why) && has(x.why, "is gone") && has(x.why, "nothing was pasted"));
        CHECK(x.calls == 1 && x.copies.empty());
        CHECK(x.doc.revision() == r0 && !x.doc.can_undo());
        CHECK(x.doc.snapshot().sources->sources.empty());
    }
    {   // A mono clip on a stereo track: refused before the seam is asked.
        CrossSession x;
        Snapshot other = empty_other();
        auto edit = std::make_shared<EditState>(*other.edit);
        auto t = std::make_shared<TrackEdit>(*edit->tracks[1]);
        t->channels = 2;
        edit->tracks[1] = t;
        other.edit = edit;
        CHECK(x.copy_region_and_open(other));
        CHECK(!x.c.paste(x.why) && has(x.why, "nothing was pasted"));
        CHECK(x.calls == 0 && x.copies.empty() && !x.doc.can_undo());
    }
    return 0;
}

// The AddSource member counts against the limit: kLimit regions fit alone (see
// test_limit) and not with the source that has to arrive with them.
int test_paste_across_sessions_limit() {
    CrossSession x(CrossSession::umid_session(dense()));
    select_range(x.c, 0, 0, span(kLimit));
    CHECK(x.c.copy(x.why));
    CHECK(x.open_other(empty_other()));
    const uint64_t r0 = x.doc.revision();
    CHECK(!x.c.paste(x.why));
    CHECK(x.why == "Paste needs " + std::to_string(kLimit + 1) + " changes; at most " +
                       std::to_string(kLimit) + " in one edit. Nothing was pasted.");
    CHECK(x.doc.revision() == r0 && lane(x.doc.snapshot(), 1).regions.empty());
    return 0;
}

// Shuffle ripples what follows by the clipboard's length, as inside one session.
int test_paste_across_sessions_shuffle() {
    CrossSession x;
    Snapshot other = session_with({make_region(30, "X", 4 * kBar, kBar)});
    {
        auto pool = std::make_shared<SourcePool>(*other.sources);
        pool->sources[0].id = 400;  // below open_other's next_id: the pool stays sorted by id
        pool->sources[0].path = "Audio Files/Other.wav";
        other.sources = pool;
        auto edit = std::make_shared<EditState>(*other.edit);
        auto t = std::make_shared<TrackEdit>(*edit->tracks[0]);
        auto p = std::make_shared<Playlist>(*t->playlists[0]);
        p->regions[0].channels = {{400, 0}};
        t->playlists[0] = p;
        edit->tracks[0] = t;
        other.edit = edit;
    }
    CHECK(x.copy_region_and_open(other));
    x.c.set_mode(EditMode::Shuffle);
    x.c.select_track(0);
    x.c.set_insertion(0);
    const bool ok = x.c.paste(x.why);
    if (!ok) std::fprintf(stderr, "WHY: %s\n", x.why.c_str());
    CHECK(ok);
    CHECK(start_of(x.doc.snapshot(), 30) == 6 * kBar);   // moved by the 2 bars pasted
    CHECK(lane(x.doc.snapshot(), 0).regions.size() == 2 &&
          lane(x.doc.snapshot(), 0).regions[0].position == 0);
    CHECK(x.doc.history().undo_depth() == 1 && valid(x.doc.snapshot()));
    CHECK(x.c.undo(x.why));
    CHECK(start_of(x.doc.snapshot(), 30) == 4 * kBar);
    return 0;
}

// ADR-0098 follow-up (c): a group move carries each member's lane points.
// A [1,3) and B [4,5) bars grouped; Volume points sit inside A, inside B and
// after both (the last belongs to neither and stays).
int test_group_move_automation_follows() {
    const std::vector<AutomationPoint> before{
        {kBar + kBar / 2, -1.0f}, {4 * kBar + kBar / 2, -2.0f}, {7 * kBar, -3.0f}};
    for (const bool follows : {true, false}) {
        Snapshot s = with_markers_and_lane();
        auto edit = std::make_shared<EditState>(*s.edit);
        auto t = std::make_shared<TrackEdit>(*edit->tracks[0]);
        auto lane_copy = std::make_shared<AutomationLane>(*t->automation[0]);
        lane_copy->points = before;
        t->automation = {lane_copy};
        auto p = std::make_shared<Playlist>(*t->playlists[0]);
        for (Region& r : p->regions) r.clip_group = 10;
        t->playlists[0] = p;
        edit->tracks[0] = t;
        s.edit = edit;
        ModelDocument doc(std::move(s));
        EditController c(doc);
        c.set_automation_follows_edit(follows);
        std::string why;
        CHECK(c.select_region(10));
        const Frames a0 = start_of(doc.snapshot(), 10), b0 = start_of(doc.snapshot(), 11);
        CHECK(c.nudge(1, why));
        const Frames delta = start_of(doc.snapshot(), 10) - a0;
        CHECK(delta != 0 && start_of(doc.snapshot(), 11) - b0 == delta);
        CHECK(doc.undo_label() == "Move Group");
        const auto& pts = doc.snapshot().edit->tracks[0]->automation[0]->points;
        CHECK(pts.size() == 3);
        CHECK(pts[0].at == before[0].at + (follows ? delta : 0));
        CHECK(pts[1].at == before[1].at + (follows ? delta : 0));
        CHECK(pts[2].at == before[2].at);
        CHECK(doc.history().undo_depth() == 1);
        CHECK(c.undo(why));
        const auto& back = doc.snapshot().edit->tracks[0]->automation[0]->points;
        CHECK(start_of(doc.snapshot(), 10) == a0 && start_of(doc.snapshot(), 11) == b0);
        CHECK(back.size() == 3 && back[0].at == before[0].at && back[1].at == before[1].at);
    }
    return 0;
}

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    register_types();
    if (test_group_move_automation_follows() ||test_copy_paste() || test_cut_clear_duplicate() || test_repeat() || test_shuffle() || test_shuffle_trim_and_spot() ||
        test_shuffle_nudge_and_drag() || test_shuffle_loop_ripples() ||
        test_shuffle_automation_notice() || test_insert_delete_time() ||
        test_repeat_count_text() || test_clip_loop_command() || test_fades_and_crossfades() ||
        test_cut_edges_lose_fades() || test_paste_across_sessions() ||
        test_paste_across_sessions_arrive() || test_paste_across_sessions_reuse_and_rename() ||
        test_paste_across_sessions_refusals() || test_paste_across_sessions_limit() ||
        test_paste_across_sessions_shuffle() ||
        test_limit() || test_several_tracks() || test_menu()) {
        return 1;
    }
    std::printf("ui clipboard: ok\n");
    return 0;
}
