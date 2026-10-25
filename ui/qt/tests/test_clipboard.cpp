// SPDX-License-Identifier: GPL-3.0-or-later
//
// Cut, Copy, Paste, Duplicate, Repeat and Clear (edit_clipboard.h): each gesture one
// compound and one labelled undo step; past model::kMaxCompoundMembers
// refused before anything is sent, in the operator's words, changing
// nothing; Shuffle refused by name. Then the Edit menu in the real QML,
// loaded on the offscreen platform: its undo text, and its items sending
// what the keys send.
#include <QGuiApplication>
#include <QKeyEvent>
#include <QQuickItem>
#include <QQuickView>
#include <QTest>
#include <algorithm>
#include <cstdio>
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

    // Shuffle would ripple: refused by name, nothing changed. Copy changes
    // no region, so Shuffle allows it.
    c.set_mode(EditMode::Shuffle);
    const uint64_t r0 = doc.revision();
    CHECK(c.select_region(10));
    CHECK(!c.cut(why) && has(why, "Shuffle") && has(why, "ShiftRegions") && has(why, "Nothing was cut."));
    CHECK(!c.clear(why) && has(why, "Nothing was cleared."));
    CHECK(!c.duplicate(why) && has(why, "Nothing was duplicated."));
    CHECK(c.copy(why));
    CHECK(!c.paste(why) && has(why, "Nothing was pasted."));
    CHECK(doc.revision() == r0);
    // Spot and Grid paste as Slip does.
    c.set_mode(EditMode::Spot);
    c.select_track(1);
    c.set_insertion(0);
    CHECK(c.paste(why));
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
    c.set_mode(EditMode::Shuffle);
    CHECK(!c.can_repeat(why) && has(why, "Shuffle") && has(why, "Nothing was repeated."));
    CHECK(!c.repeat(2, why) && has(why, "Shuffle"));
    c.set_mode(EditMode::Slip);
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

    // Shuffle refuses on several tracks as on one.
    c.set_mode(EditMode::Shuffle);
    CHECK(!c.paste(why) && has(why, "Shuffle"));
    c.set_mode(EditMode::Slip);

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

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    register_types();
    if (test_copy_paste() || test_cut_clear_duplicate() || test_repeat() ||
        test_repeat_count_text() || test_fades_and_crossfades() ||
        test_cut_edges_lose_fades() ||
        test_limit() || test_several_tracks() || test_menu()) {
        return 1;
    }
    std::printf("ui clipboard: ok\n");
    return 0;
}
