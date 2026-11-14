// SPDX-License-Identifier: GPL-3.0-or-later
//
// The loop handle (ADR-0098 4a): the drag snaps to whole repeats, lays them
// as one undo step, and Esc leaves the session untouched.
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "edit_controller.h"
#include "edit_loop_handle.h"
#include "timeline_document.h"

using namespace sonora;
using namespace sonora::ui;
using namespace sonora::ui::model;

#define CHECK(cond)                                                          do {                                                                         if (!(cond)) {                                                               std::fprintf(stderr, "FAIL %s:%d: %s
", __FILE__, __LINE__,                          #cond);                                                     return 1;                                                            }                                                                    } while (0)

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

int test_snap() {
    CHECK(loop_handle_repeats(kBar, kBar * 12 / 5) == 2);
    CHECK(loop_handle_repeats(kBar, kBar / 3) == 0);
    CHECK(loop_handle_repeats(0, kBar) == 0);
    return 0;
}

// The loop handle (ADR-0098 4a): dragging a selected clip's bottom-right corner
// 2.4 lengths past its end lays two repeats as one undo step; Esc leaves the
// session untouched.
int test_loop_handle_drag() {
    ModelDocument doc(two_regions());
    EditController c(doc);
    c.set_viewport(4000, 400);
    c.scroll_to_frame(0);
    std::string why;
    const std::size_t before = lane(doc.snapshot(), 0).regions.size();
    const double low = c.view().track_height * 0.9;
    const double edge = c.view().x_of(5 * kBar) - 2;
    CHECK(c.select_region(11));
    CHECK(c.hit(edge, low).zone == HitZone::LoopHandle);
    const double to = c.view().x_of(5 * kBar + kBar * 12 / 5);

    CHECK(c.press(edge, low, why));
    c.move(to, low);
    CHECK(c.drag().kind == Drag::Kind::Loop && c.drag().loops == 2);
    c.cancel_drag();
    CHECK(c.release(to, low, why));
    CHECK(lane(doc.snapshot(), 0).regions.size() == before && !doc.can_undo());

    CHECK(c.select_region(11));
    CHECK(c.press(edge, low, why));
    c.move(to, low);
    CHECK(c.release(to, low, why));
    CHECK(lane(doc.snapshot(), 0).regions.size() == before + 2);
    CHECK(doc.history().undo_depth() == 1 && doc.undo_label() == "Loop Clip");
    CHECK(valid(doc.snapshot()));
    CHECK(c.undo(why) && !doc.can_undo());
    return 0;
}

}  // namespace

int main() {
    return test_snap() || test_loop_handle_drag() ? 1 : 0;
}
