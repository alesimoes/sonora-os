// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Edit and Mix windows' logic, without a window: common/model's
// operations and History behind the TimelineDocument seam, mouse gestures
// and keys turned into operations, the zoom and scroll mapping, the fader
// taper, the time formats and the ruler, which rectangle the timeline draws
// where, and File > Save / Open through SessionModel. Nothing is rendered.
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

#include "edit_controller.h"
#include "fader_taper.h"
#include "key_router.h"
#include "peak_source.h"
#include "qt_keys.h"
#include "session_files.h"
#include "engine_saver.h"
#include "session_model.h"
#include "sonora/api/fake_engine.h"
#include "sonora/fade_curve.h"
#include "sonora/model/session_file.h"
#include "theme.h"
#include "time_format.h"
#include "timeline_document.h"
#include "timeline_scene.h"

using namespace sonora;
using namespace sonora::ui;
using namespace sonora::ui::model;
using sonora::ui::testing::engine_saver;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

int checks = 0;
#define OK(cond)          \
    do {                  \
        CHECK(cond);      \
        ++checks;         \
    } while (0)

constexpr uint32_t kRate = 48000;
constexpr Frames kBar = 96000;   // 120 bpm, 4/4
constexpr Frames kBeat = 24000;

// A peak source with a flat envelope, so a pixel test knows where the
// waveform is without trusting the synthetic one.
class FlatPeaks final : public PeakSource {
public:
    void peaks(Id, uint16_t, Frames, double, int count, PeakPair* out) const override {
        for (int i = 0; i < count; ++i) out[i] = {-0.5f, 0.5f};
    }
};

// Two tracks. Track 1 has regions A [1 bar, 3 bars) and B [4, 5) over one
// 20-bar source; track 2 is empty. Built by hand, with fixed ids the tests
// name, and checked against common/model's invariants by test_model.
Snapshot two_tracks() {
    Snapshot s = make_empty_session(2, kRate);
    auto pool = std::make_shared<SourcePool>();
    Source take;
    take.id = 100;
    take.path = "Audio Files/Take 1.wav";
    take.length = 20 * kBar;
    pool->sources.push_back(take);
    s.sources = pool;
    s.next_id = 101;  // every id above is taken; a new one is 101 or more
    auto edit = std::make_shared<EditState>(*s.edit);
    auto t = std::make_shared<TrackEdit>(*edit->tracks[0]);
    auto p = std::make_shared<Playlist>(*t->playlists[0]);
    Region a;
    a.id = 10;
    a.name = "A";
    a.position = kBar;
    a.length = 2 * kBar;
    a.source_offset = kBar;
    a.channels = {{100, 0}};
    Region b = a;
    b.id = 11;
    b.name = "B";
    b.position = 4 * kBar;
    b.length = kBar;
    b.source_offset = 4 * kBar;
    p->regions = {a, b};
    t->playlists[0] = p;
    edit->tracks[0] = t;
    s.edit = edit;
    return s;
}

Id playlist_of(const Snapshot& s, int track) { return s.edit->tracks[track]->active_playlist; }

const Region* region(const Snapshot& s, Id id) { return find_region(s, id); }

bool valid(const Snapshot& s) {
    std::string error;
    const bool ok = check_invariants(s, error);
    if (!ok) std::fprintf(stderr, "  invariant: %s\n", error.c_str());
    return ok;
}

// --- the model, through the seam ------------------------------------------------
//
// common/model has its own tests of apply() and History; these check what
// the interface relies on through ModelDocument, and the session builders.

int test_model() {
    {
        const Snapshot demo = make_demo_session(18, kRate);
        OK(valid(demo));
        OK(demo.edit->tracks.size() == 18);
        OK(demo.settings.timecode.origin == 3600LL * kRate);  // 01:00:00:00
        OK(demo.revision == 0);
        OK(demo.mix.tracks.size() == 18 && demo.mix.find(demo.edit->tracks[17]->id)->mute);
        const Snapshot empty = make_empty_session(4, 44100);
        OK(valid(empty));
        OK(empty.edit->tracks[3]->name == "Audio 4");
        OK(empty.mix.find(empty.edit->tracks[3]->id)->input == std::vector<uint16_t>{3});
    }

    OK(valid(two_tracks()));
    ModelDocument doc(two_tracks());
    const Snapshot s0 = doc.snapshot();
    const Id pl = playlist_of(s0, 0);
    std::string why;

    // Move: the region moves, and the untouched track is the same object.
    OK(doc.submit(MoveRegion{pl, 10, 8 * kBar, kNoId}, why));
    OK(region(doc.snapshot(), 10)->position == 8 * kBar);
    OK(doc.snapshot().edit->tracks[1] == s0.edit->tracks[1]);
    OK(doc.snapshot().revision == s0.revision + 1);
    OK(valid(doc.snapshot()));

    // Undo is a pointer: the EditState from before, not a reconstruction.
    OK(doc.undo(why));
    OK(doc.snapshot().edit == s0.edit);
    OK(doc.redo(why));
    OK(region(doc.snapshot(), 10)->position == 8 * kBar);
    OK(doc.undo(why));

    // Moving B (1 bar) into the middle of A ([1, 3) bars) splits A; the
    // right half takes the id the operation carries. Without one, or with
    // one below next_id, the move is refused rather than an id invented.
    Snapshot out;
    OK(!apply(doc.snapshot(), MoveRegion{pl, 11, kBar + kBar / 2, kNoId}, out, why));
    OK(!why.empty());
    OK(!doc.submit(MoveRegion{pl, 11, kBar + kBar / 2, 10}, why));  // taken
    OK(!doc.submit(MoveRegion{pl, 11, kBar + kBar / 2, 60}, why));  // below next_id
    const Id spare = doc.reserve_id();
    OK(spare >= 101);
    OK(doc.submit(MoveRegion{pl, 11, kBar + kBar / 2, spare}, why));
    OK(region(doc.snapshot(), 10)->end() == kBar + kBar / 2);
    OK(region(doc.snapshot(), spare)->position == 2 * kBar + kBar / 2);
    OK(region(doc.snapshot(), spare)->source_offset == 2 * kBar + kBar / 2);
    OK(region(doc.snapshot(), spare)->end() == 3 * kBar);
    OK(valid(doc.snapshot()));
    OK(doc.undo(why));
    // Over A's tail only, A is trimmed and nothing is split.
    OK(doc.submit(MoveRegion{pl, 11, 2 * kBar + kBar / 2, kNoId}, why));
    OK(region(doc.snapshot(), 10)->end() == 2 * kBar + kBar / 2);
    OK(valid(doc.snapshot()));
    OK(doc.undo(why));
    OK(doc.snapshot().edit == s0.edit);

    // Trim: the offset follows the start; the source bounds hold.
    OK(doc.submit(TrimRegion{pl, 10, Edge::Start, kBar + kBeat}, why));
    OK(region(doc.snapshot(), 10)->source_offset == kBar + kBeat);
    OK(region(doc.snapshot(), 10)->end() == 3 * kBar);
    OK(!doc.submit(TrimRegion{pl, 10, Edge::Start, -1}, why));
    OK(!doc.submit(TrimRegion{pl, 11, Edge::End, 30 * kBar}, why));  // past the source
    OK(doc.undo(why));

    // Trimming over a neighbour overwrites it (timeline-model.md,
    // "Overwrite"). The stand-in refused this; common/model, like Pro Tools
    // in Slip, trims what is beneath.
    OK(doc.submit(TrimRegion{pl, 10, Edge::End, 4 * kBar + kBeat}, why));  // into B
    OK(region(doc.snapshot(), 10)->end() == 4 * kBar + kBeat);
    OK(region(doc.snapshot(), 11)->position == 4 * kBar + kBeat);
    OK(region(doc.snapshot(), 11)->source_offset == 4 * kBar + kBeat);  // B's audio stays put
    OK(region(doc.snapshot(), 11)->end() == 5 * kBar);
    OK(valid(doc.snapshot()));
    OK(doc.undo(why));
    OK(doc.submit(TrimRegion{pl, 10, Edge::End, 6 * kBar}, why));  // over all of B
    OK(region(doc.snapshot(), 11) == nullptr);
    OK(doc.undo(why));
    OK(region(doc.snapshot(), 11)->position == 4 * kBar);  // undo brings B back whole
    OK(doc.snapshot().edit == s0.edit);

    // Split: the left keeps the id, the right gets the new one.
    OK(!doc.submit(SplitRegion{pl, 10, 2 * kBar, 11}, why));
    const Id right = doc.reserve_id();
    OK(right != spare && right > spare);  // an id is never handed out twice
    OK(!doc.submit(SplitRegion{pl, 10, 5 * kBar, right}, why));  // outside
    OK(doc.submit(SplitRegion{pl, 10, 2 * kBar, right}, why));
    OK(region(doc.snapshot(), 10)->end() == 2 * kBar);
    OK(region(doc.snapshot(), right)->position == 2 * kBar);
    OK(region(doc.snapshot(), right)->source_offset == 2 * kBar);
    OK(valid(doc.snapshot()));
    OK(doc.snapshot().next_id == right + 1);
    OK(doc.reserve_id() == doc.snapshot().next_id);  // honours what apply() raised

    // Remove, then undo twice is back to the start.
    OK(doc.submit(RemoveRegion{pl, right}, why));
    OK(region(doc.snapshot(), right) == nullptr);
    OK(doc.undo(why) && doc.undo(why));
    OK(doc.snapshot().edit == s0.edit);
    OK(!doc.undo(why) && why == "nothing to undo");

    // A new edit after an undo drops the redo branch.
    OK(doc.submit(RemoveRegion{pl, 11}, why));
    OK(doc.undo(why));
    OK(doc.can_redo());
    OK(doc.submit(RemoveRegion{pl, 10}, why));
    OK(!doc.can_redo());
    OK(!doc.redo(why) && why == "nothing to redo");
    OK(doc.undo(why));

    // Mix is not an edit: undo does not move a fader. SetMix replaces the
    // whole TrackMix; the controller's clamping is in test_controller.
    const Id t1 = s0.edit->tracks[0]->id;
    TrackMix m = *doc.snapshot().mix.find(t1);
    m.gain_db = -6.0f;
    OK(doc.submit(SetMix{m}, why));
    OK(doc.snapshot().mix.find(t1)->gain_db == -6.0f);
    OK(!doc.can_undo());
    m.track = 999;
    OK(!doc.submit(SetMix{m}, why));
    m.track = t1;
    m.pan = NAN;
    OK(!doc.submit(SetMix{m}, why));
    return 0;
}

// Undo through common/model's History: bounded by bytes, with a floor, and
// a new history when another session is opened.
int test_history() {
    std::string why;
    // A budget too small for any step: the newest state and the one before
    // it are always kept, so exactly one undo remains however many edits.
    ModelDocument tight(two_tracks(), 1);
    const Id pl = playlist_of(tight.snapshot(), 0);
    for (int i = 1; i <= 4; ++i) {
        OK(tight.submit(MoveRegion{pl, 11, (5 + i) * kBar, kNoId}, why));
    }
    OK(tight.history().step_count() == 2);
    OK(tight.history().floor() > 0);  // the steps below it were evicted
    OK(tight.undo(why));
    OK(region(tight.snapshot(), 11)->position == 8 * kBar);
    OK(!tight.undo(why));  // at the floor
    OK(tight.redo(why));
    OK(region(tight.snapshot(), 11)->position == 9 * kBar);

    // The default budget keeps every step of an editing session this size.
    ModelDocument roomy(two_tracks());
    for (int i = 1; i <= 50; ++i) {
        OK(roomy.submit(MoveRegion{pl, 11, (5 + i % 10) * kBar, kNoId}, why));
    }
    OK(roomy.history().undo_depth() == 50);
    OK(roomy.history().charged_bytes() > 0);
    OK(roomy.history().charged_bytes() <= roomy.history().budget());
    for (int i = 0; i < 50; ++i) OK(roomy.undo(why));
    OK(roomy.snapshot().edit->tracks[0]->playlists[0]->regions == two_tracks().edit->tracks[0]->playlists[0]->regions);

    // Each undo and redo bumps the revision, so the windows redraw.
    const uint64_t r = roomy.revision();
    OK(roomy.redo(why) && roomy.revision() == r + 1);

    // reset: another session, and no undo into the old one.
    OK(roomy.reset(make_empty_session(3, kRate), why));
    OK(roomy.snapshot().edit->tracks.size() == 3);
    OK(!roomy.can_undo() && !roomy.can_redo());
    OK(roomy.reserve_id() == roomy.snapshot().next_id);
    return 0;
}


// --- gestures and commands -----------------------------------------------------

int test_controller() {
    ModelDocument doc(two_tracks());
    std::string why;
    {
    // --- the mapping ---
    EditController c(doc);
    c.set_viewport(1000, 100);
    ViewState v = c.view();
    OK(v.fpp == kRate / 40.0);               // 40 px a second
    OK(v.x_of(kRate) == 40.0);
    OK(v.frame_at(40.0) == kRate);
    OK(v.track_at(0) == 0 && v.track_at(v.track_height + 1) == 1);

    // Zooming keeps the anchor frame under the same pixel.
    c.set_insertion(2 * kRate);              // x = 80
    c.zoom_in();
    OK(c.view().fpp == kRate / 80.0);
    OK(std::abs(c.view().x_of(2 * kRate) - 80.0) < 1e-6);
    c.zoom_out();
    OK(c.view().fpp == kRate / 40.0);
    c.zoom_by(0.5, 500);
    OK(std::abs(c.view().frame_at(500) - v.frame_at(500)) <= 1);
    c.zoom_by(2.0, 500);

    // Scrolling clamps at the session start and at the last lane.
    c.scroll_by(-1000, 0);
    OK(c.view().scroll == 0.0);
    c.scroll_by(100, 0);
    OK(c.view().scroll == 100 * c.view().fpp);
    c.scroll_by(0, 1e6);
    OK(c.view().scroll_y == c.max_scroll_y());
    OK(c.max_scroll_y() == 2 * c.view().track_height - 100);
    c.scroll_by(-100, -1e6);
    OK(c.view().scroll == 0.0 && c.view().scroll_y == 0.0);

    OK(c.zoom_fit_session(why));
    OK(c.view().x_of(c.session_end()) <= 1000 && c.view().x_of(c.session_end()) > 900);
    const double fit = c.view().fpp;
    OK(c.zoom_toggle(why));  // nothing selected: the whole session, then back
    OK(c.zoom_toggle(why));
    OK(c.view().fpp == fit);
    }

    // --- gestures, at 40 px a second ---
    EditController c(doc);
    c.set_viewport(1000, 200);
    const double th = c.view().track_height;
    const double lower = th * 0.75, upper = th * 0.25;
    const double a_mid = c.view().x_of(2 * kBar);  // inside A, away from its edges

    // --- Smart Tool, lower half: grab and move, in Slip ---
    OK(c.hit(a_mid, lower).zone == HitZone::Body);
    OK(c.hit(a_mid, upper).zone == HitZone::Select);
    OK(c.hit(c.view().x_of(kBar) + 2, lower).zone == HitZone::TrimStart);
    OK(c.hit(c.view().x_of(3 * kBar) - 2, lower).zone == HitZone::TrimEnd);
    OK(c.press(a_mid, lower, why));
    OK(c.selection().region == 10);
    c.move(a_mid + 20, lower);  // a drag in progress is not an operation
    OK(doc.snapshot().revision == 0);
    OK(c.drag().start == kBar + static_cast<Frames>(20 * c.view().fpp));
    OK(c.release(a_mid + 20, lower, why));
    OK(region(doc.snapshot(), 10)->position == kBar + static_cast<Frames>(20 * c.view().fpp));
    OK(doc.can_undo());
    OK(c.undo(why));
    OK(region(doc.snapshot(), 10)->position == kBar);

    // A click without a drag selects and does not edit.
    OK(c.press(a_mid, lower, why) && c.release(a_mid + 1, lower, why));
    OK(doc.snapshot().revision == 2);  // the move and its undo, nothing more

    // --- Grid: the start snaps to the grid value ---
    c.set_mode(EditMode::Grid);
    c.set_grid(GridValue::Bar);
    OK(c.press(a_mid, lower, why));
    c.move(a_mid + 50, lower);  // 1.25 s = 0.625 bar: nearest bar is +1
    OK(c.release(a_mid + 50, lower, why));
    OK(region(doc.snapshot(), 10)->position == 2 * kBar);
    OK(c.undo(why));
    c.set_mode(EditMode::Slip);

    // --- trim the end with the Smart Tool's edge zone ---
    const double a_end = c.view().x_of(3 * kBar) - 2;
    OK(c.press(a_end, lower, why));
    c.move(a_end - 40, lower);  // one second earlier
    OK(c.release(a_end - 40, lower, why));
    OK(region(doc.snapshot(), 10)->end() == 3 * kBar - kRate);
    // ...and dragging it into B overwrites B, as a Slip-mode trim does in
    // Pro Tools. (On the stand-in the drag stopped at B, because its model
    // refused the overlap; common/model overwrites, and the resolver no
    // longer clamps at a neighbour.) 140 px is 168000 frames: one beat in.
    const double a_end2 = c.view().x_of(3 * kBar - kRate) - 2;
    OK(c.press(a_end2, lower, why));
    c.move(a_end2 + 140, lower);
    // While the edge is in the hand, the preview shows B cut back and the
    // document still has B whole: only the drop is an operation.
    OK(c.drag().preview != nullptr);
    OK(c.drag().end == 4 * kBar + kBeat);
    OK(c.drag().preview->regions.back().id == 11 &&
       c.drag().preview->regions.back().position == 4 * kBar + kBeat);
    OK(region(doc.snapshot(), 11)->position == 4 * kBar);
    OK(c.release(a_end2 + 140, lower, why));
    OK(region(doc.snapshot(), 10)->end() == 4 * kBar + kBeat);
    OK(region(doc.snapshot(), 11)->position == 4 * kBar + kBeat);
    OK(region(doc.snapshot(), 11)->source_offset == 4 * kBar + kBeat);
    OK(c.undo(why));
    // Far past B: B is covered, so it goes; the source's end is the limit.
    OK(c.press(a_end2, lower, why));
    c.move(a_end2 + 400, lower);
    OK(c.release(a_end2 + 400, lower, why));
    OK(region(doc.snapshot(), 10)->end() == 8 * kBar - kRate);
    OK(region(doc.snapshot(), 11) == nullptr);
    OK(c.selection().region == 10);
    OK(c.undo(why));
    OK(region(doc.snapshot(), 11) != nullptr);  // undo restores it whole
    OK(c.press(a_end2, lower, why));
    c.move(a_end2 + 40000, lower);
    OK(c.release(a_end2 + 40000, lower, why));
    OK(region(doc.snapshot(), 10)->end() == 20 * kBar);  // where Take 1 runs out
    OK(c.undo(why) && c.undo(why));

    // --- upper half: the insertion point, then separate there ---
    OK(c.press(a_mid, upper, why) && c.release(a_mid, upper, why));
    OK(c.selection().region == kNoId);
    OK(c.selection().insertion == c.view().frame_at(a_mid));
    OK(c.separate(why));
    OK(doc.snapshot().edit->tracks[0]->playlists[0]->regions.size() == 3);
    OK(c.undo(why));
    OK(doc.snapshot().edit->tracks[0]->playlists[0]->regions.size() == 2);

    // A range separates at both ends.
    OK(c.press(c.view().x_of(kBar + kBeat), upper, why));
    c.move(c.view().x_of(2 * kBar), upper);
    OK(c.release(c.view().x_of(2 * kBar), upper, why));
    OK(c.selection().has_range());
    OK(c.separate(why));
    OK(doc.snapshot().edit->tracks[0]->playlists[0]->regions.size() == 4);
    OK(c.undo(why));   // both ends are one step (one Compound, "Separate")
    OK(doc.snapshot().edit->tracks[0]->playlists[0]->regions.size() == 2);

    // Trim to the insertion point (Pro Tools' A and S).
    c.set_insertion(kBar + kBar / 2);
    OK(c.trim_start_to_insertion(why));
    OK(region(doc.snapshot(), 10)->position == kBar + kBar / 2);
    OK(c.undo(why));
    OK(c.trim_end_to_insertion(why));
    OK(region(doc.snapshot(), 10)->end() == kBar + kBar / 2);
    OK(c.undo(why));
    c.set_insertion(10 * kBar);
    OK(!c.trim_start_to_insertion(why) && !why.empty());

    // --- clear and nudge act on the selected region ---
    OK(c.press(a_mid, lower, why) && c.release(a_mid, lower, why));
    c.set_grid(GridValue::Beat);
    OK(c.nudge(1, why));
    OK(region(doc.snapshot(), 10)->position == kBar + kBeat);
    OK(c.nudge(-1, why));
    OK(c.clear(why));
    OK(region(doc.snapshot(), 10) == nullptr);
    OK(c.selection().region == kNoId);
    OK(c.undo(why));
    OK(!c.clear(why));  // the selection went with the region

    // --- Shuffle and Spot refuse what they would do differently ---
    c.set_mode(EditMode::Shuffle);
    OK(!c.press(a_mid, lower, why));
    OK(why.find("Shuffle") != std::string::npos);
    OK(c.drag().kind == Drag::Kind::None);
    c.set_mode(EditMode::Spot);
    OK(!c.trim_start_to_insertion(why));
    c.set_mode(EditMode::Slip);
    c.next_mode();
    OK(c.mode() == EditMode::Spot);
    c.set_mode(EditMode::Slip);

    // --- a click on another lane selects that track ---
    OK(c.press(10, th * 1.5, why) && c.release(10, th * 1.5, why));
    OK(c.selection().track == 1);
    OK(c.select_next_track(-1, why) && c.selection().track == 0);
    OK(c.select_region(11) && c.selection().region == 11);
    OK(c.selection().start == 4 * kBar && c.selection().end == 5 * kBar);
    OK(!c.select_region(999));

    // --- the playhead pages the view while rolling ---
    c.scroll_to_frame(0);
    OK(!c.follow(kRate));  // on screen
    OK(c.follow(static_cast<Frames>(1000 * c.view().fpp)));
    OK(c.view().x_of(static_cast<Frames>(1000 * c.view().fpp)) < 150);

    // --- mix: each one a whole-TrackMix operation, never an undo step ---
    // The fader's travel bounds a gain (the stand-in's model clamped it;
    // common/model accepts any finite number, so the controller does).
    const bool could_undo = doc.can_undo();
    const Id t1 = doc.snapshot().edit->tracks[0]->id;
    OK(c.set_gain(0, 40.0f, why));
    OK(doc.snapshot().mix.find(t1)->gain_db == kFaderMaxDb);
    OK(c.set_gain(0, -500.0f, why));
    OK(doc.snapshot().mix.find(t1)->gain_db == kFaderSilenceDb);
    OK(!c.set_gain(0, NAN, why) && !why.empty());
    OK(c.set_pan(0, 3.0f, why) && doc.snapshot().mix.find(t1)->pan == 1.0f);
    OK(c.toggle_mute(0, why) && doc.snapshot().mix.find(t1)->mute);
    OK(doc.snapshot().mix.find(t1)->gain_db == kFaderSilenceDb);  // the rest kept
    OK(c.toggle_mute(0, why) && !doc.snapshot().mix.find(t1)->mute);
    OK(c.set_bus_gain(-3.0f, why) && doc.snapshot().mix.bus.gain_db == -3.0f);
    OK(!c.set_gain(5, 0.0f, why) && why == "there is no track 6");

    // A gain beyond the fader's travel (a loaded file at +18 dB) is not
    // pulled to +12 by a touch: it stays until the operator drags it down.
    {
        TrackMix hot = *doc.snapshot().mix.find(t1);
        hot.gain_db = 18.0f;
        OK(doc.submit(SetMix{hot}, why));
        const auto gain = [&] { return doc.snapshot().mix.find(t1)->gain_db; };
        OK(fader_drag(18.0f, 0.0f, 200.0f, false) == 18.0f);
        OK(fader_drag(18.0f, -30.0f, 200.0f, false) == 18.0f);   // up: already past the top
        OK(fader_drag(-200.0f, 30.0f, 200.0f, false) == -200.0f);  // down: already past the bottom
        OK(c.set_gain(0, fader_drag(gain(), -30.0f, 200.0f, false), why) && gain() == 18.0f);
        OK(c.set_gain(0, 18.0f, why) && gain() == 18.0f);
        OK(c.set_gain(0, 40.0f, why) && gain() == 18.0f);  // never further out than it was
        OK(c.set_gain(0, fader_drag(gain(), 20.0f, 200.0f, false), why));
        OK(gain() < kFaderMaxDb && gain() > 0.0f);  // dragged down: back on the travel
        OK(c.set_gain(0, 40.0f, why) && gain() == kFaderMaxDb);
        hot.gain_db = 18.0f;
        OK(doc.submit(SetMix{hot}, why));
        OK(c.set_gain(0, 3.0f, why) && gain() == 3.0f);  // a deliberate value is taken
    }
    OK(doc.can_undo() == could_undo);
    return 0;
}

// --- fader taper ---------------------------------------------------------------

int test_taper() {
    OK(fader_position_from_db(12.0f) == 1.0f);
    OK(std::abs(fader_position_from_db(0.0f) - 0.78f) < 1e-6f);
    OK(fader_position_from_db(-200.0f) == 0.0f);
    OK(fader_position_from_db(NAN) == 0.0f);
    OK(fader_position_from_db(30.0f) == 1.0f);
    OK(fader_db_from_position(0.0f) == kFaderSilenceDb);
    OK(fader_db_from_position(1.0f) == 12.0f);
    // Monotonic and invertible over the whole travel.
    float prev = -1.0f;
    for (float db = -150.0f; db <= 12.0f; db += 0.25f) {
        const float p = fader_position_from_db(db);
        OK(p >= prev);
        prev = p;
        if (db > kFaderSilenceDb) OK(std::abs(fader_db_from_position(p) - db) < 0.01f);
    }
    // The travel goes where an engineer rides: -20..+6 dB is about half of it.
    OK(fader_position_from_db(6.0f) - fader_position_from_db(-20.0f) > 0.45f);
    OK(gain_text(0.0f) == "0.0");
    OK(gain_text(-0.04f) == "0.0");
    OK(gain_text(3.46f) == "+3.5");
    OK(gain_text(-12.0f) == "-12.0");
    OK(gain_text(kFaderSilenceDb) == "-inf");
    OK(pan_text(0.0f) == "C");
    OK(pan_text(-1.0f) == "<100");
    OK(pan_text(0.25f) == "25>");
    // A drag of the whole travel goes end to end; fine moves a tenth.
    OK(fader_drag(0.0f, -1000.0f, 100.0f, false) == 12.0f);
    OK(fader_drag(0.0f, 1000.0f, 100.0f, false) == kFaderSilenceDb);
    const float coarse = fader_drag(0.0f, -10.0f, 100.0f, false);
    const float fine = fader_drag(0.0f, -10.0f, 100.0f, true);
    OK(fine > 0.0f && fine < coarse);
    OK(std::abs((fader_position_from_db(fine) - 0.78f) * 10 -
                (fader_position_from_db(coarse) - 0.78f)) < 1e-4f);
    return 0;
}

// --- time formats and the ruler ---------------------------------------------

int test_time() {
    SessionSettings s;
    OK(format_timecode(0, s) == "01:00:00:00");
    OK(format_timecode(61 * kRate + kRate / 2, s) == "01:01:01:12");
    OK(format_timecode(-static_cast<Frames>(kRate), s) == "00:59:59:00");
    OK(format_bars_beats(0, s) == "1|1|000");
    OK(format_bars_beats(kBeat, s) == "1|2|000");
    OK(format_bars_beats(kBar, s) == "2|1|000");
    OK(format_bars_beats(kBeat / 2, s) == "1|1|480");
    OK(format_min_sec(0, kRate) == "0:00.000");
    OK(format_min_sec(83 * kRate + kRate / 2, kRate) == "1:23.500");
    OK(format_min_sec(3723LL * kRate, kRate) == "1:02:03.000");
    SessionSettings df = s;
    df.timecode.fps = Fps::F29_97df;
    df.timecode.origin = 0;
    // One minute of 29.97 is 1798.2 frames; drop-frame labels skip ;00 and
    // ;01 at the minute, so frame 1800 reads 00:01:00;02.
    OK(format_timecode(static_cast<Frames>(1800.0 * 1001 / 30000 * kRate) + 1, df) == "00:01:00;02");
    OK(snap_to_grid(kBeat + 100, GridValue::Beat, s) == kBeat);
    OK(snap_to_grid(kBeat - 100, GridValue::Bar, s) == 0);
    OK(snap_to_grid(-5000, GridValue::Beat, s) == 0);

    std::vector<RulerTick> ticks;
    for (TimeFormat f : {TimeFormat::Timecode, TimeFormat::BarsBeats, TimeFormat::MinSec}) {
        for (double fpp : {50.0, 1200.0, 48000.0}) {
            ruler_ticks(f, 0, fpp, 1000, s, ticks);
            double last = -1e9;
            int majors = 0;
            for (const auto& t : ticks) {
                if (!t.major) continue;
                OK(t.x - last >= 89.0);
                OK(!t.label.empty());
                last = t.x;
                ++majors;
            }
            OK(majors >= 2);
        }
    }
    ruler_ticks(TimeFormat::Timecode, 0, 1200, 1000, s, ticks);
    OK(ticks.front().major && ticks.front().label == "01:00:00:00");
    ruler_ticks(TimeFormat::BarsBeats, 0, 1200, 1000, s, ticks);
    OK(ticks.front().label == "1");

    // A very long session zoomed out: 5,000 and 1,000,000 bars across 1000
    // px. Bar labels thin by powers of two and reach the right edge; they
    // used to stop where the first 4096 bar lines ended.
    for (double bars : {5'000.0, 1'000'000.0}) {
        const double fpp = bars * static_cast<double>(kBar) / 1000;
        ruler_ticks(TimeFormat::BarsBeats, 0, fpp, 1000, s, ticks);
        double last = -1e9;
        int majors = 0;
        int64_t stride = 0, previous = 0;
        for (const auto& t : ticks) {
            if (!t.major) continue;
            OK(t.x - last >= 89.0);
            const int64_t bar = std::stoll(t.label);
            // One level for the whole ruler: every label the same number
            // of bars from the one before, a power of two.
            if (majors >= 1) {
                const int64_t d = bar - previous;
                OK(d > 0 && (d & (d - 1)) == 0);
                OK(stride == 0 || d == stride);
                stride = d;
            }
            previous = bar;
            last = t.x;
            ++majors;
        }
        OK(majors >= 5);
        OK(last >= 1000 - 2 * 90.0);
    }
    return 0;
}

// --- the nudge on a tempo map ---------------------------------------------------
//
// 120 4/4 to bar 3, then 60 3/4 (a 48000-frame beat, a 144000-frame bar) for
// two bars, then 120 4/4 again from 480000. Pro Tools' nudge in Bars|Beats
// moves by the musical value where the region is, not the first tempo's.
int test_nudge_tempo_map() {
    constexpr Frames kSlowBeat = 48000, kSlowBar = 3 * kSlowBeat;
    constexpr Frames kBack = 2 * kBar + 2 * kSlowBar;   // 480000
    Snapshot snap = two_tracks();
    TempoEntry slow;
    slow.at = 2 * kBar;
    slow.bpm = 60.0f;
    slow.meter = {{3, 4}};
    TempoEntry back;
    back.at = kBack;
    snap.settings.tempo = {TempoEntry{}, slow, back};
    const SessionSettings& s = snap.settings;

    OK(nudge_target(kBar, 1, GridValue::Beat, s) == kBar + kBeat);
    OK(nudge_target(4 * kBar, 1, GridValue::Beat, s) == 4 * kBar + kSlowBeat);
    // A 3/4 bar from its second beat ends on the 4/4 bar's second beat:
    // three beats, two of them slow.
    OK(nudge_target(4 * kBar, 1, GridValue::Bar, s) == kBack + kBeat);
    OK(nudge_target(2 * kBar, 1, GridValue::Bar, s) == 2 * kBar + kSlowBar);
    // Going back, the step is the one that ends where the region is: the
    // bar before the return to 4/4 is a 3/4 bar.
    OK(nudge_target(kBack, -1, GridValue::Bar, s) == kBack - kSlowBar);
    OK(nudge_target(2 * kBar, -1, GridValue::Bar, s) == kBar);
    // A step across a change is measured in beats, not frames: two 4/4
    // beats after the return and two 3/4 beats before it.
    OK(nudge_target(kBack + 2 * kBeat, -1, GridValue::Bar, s) == kBack - 2 * kSlowBeat);
    OK(nudge_target(kBeat, -1, GridValue::Bar, s) == 0);   // never below 0
    OK(nudge_target(0, -1, GridValue::Beat, s) == 0);

    // One step across several changes, every one of them inside the step:
    // 60 bpm from bar 2, 240 a 60 bpm beat later, 120 a 240 bpm beat after
    // that. A bar from bar 2 is a 60 beat, a 240 beat and two 120 beats.
    {
        SessionSettings many = s;
        TempoEntry t60, t240, t120;
        t60.at = kBar;
        t60.bpm = 60.0f;
        t240.at = kBar + kSlowBeat;
        t240.bpm = 240.0f;
        t120.at = kBar + kSlowBeat + kBeat / 2;
        many.tempo = {TempoEntry{}, t60, t240, t120};
        const Frames bar = kSlowBeat + kBeat / 2 + 2 * kBeat;
        OK(nudge_target(kBar, 1, GridValue::Bar, many) == kBar + bar);
        OK(nudge_target(kBar + bar, -1, GridValue::Bar, many) == kBar);
        // From half a 60 bpm beat: half of it, then half a 240 bpm beat.
        OK(nudge_target(kBar + kSlowBeat / 2, 1, GridValue::Beat, many) ==
           kBar + kSlowBeat + kBeat / 4);
        // From before the first change, across all three: one 120 beat
        // to bar 2, then the 60 beat, the 240 beat and one 120 beat.
        OK(nudge_target(3 * kBeat, 1, GridValue::Bar, many) == kBar + kSlowBeat + kBeat / 2 + kBeat);
        OK(nudge_target(kBar + kSlowBeat + kBeat / 2 + kBeat, -1, GridValue::Bar, many) == 3 * kBeat);
    }

    ModelDocument doc(snap);
    EditController c(doc);
    std::string why;
    c.set_grid(GridValue::Beat);
    OK(c.select_region(11));   // B at 4 bars, inside the 60 bpm stretch
    OK(c.nudge(1, why));
    OK(region(doc.snapshot(), 11)->position == 4 * kBar + kSlowBeat);
    OK(c.selection().start == 4 * kBar + kSlowBeat);
    c.set_grid(GridValue::Bar);
    OK(c.nudge(-1, why));
    OK(region(doc.snapshot(), 11)->position == 4 * kBar + kSlowBeat - kSlowBar);
    OK(c.select_region(10));   // A at 1 bar
    OK(c.nudge(-1, why));
    OK(region(doc.snapshot(), 10)->position == 0);
    OK(!c.nudge(-1, why) && !why.empty());
    return 0;
}

// --- what is drawn where --------------------------------------------------------

// --- fades -------------------------------------------------------------------
//
// Fade to Start / Fade to End / the no-dialog crossfade, the Smart Tool's
// fade corners, Clear on a fade, and the curve the timeline draws held to
// the gain the engine plays.

int test_fades() {
    // The curve: the engine's sonora::fade_in_gain() itself, reached through
    // model::FadeShape. Each shape must land on the engine's curve of the
    // same name (the law's values are pinned in engine/core's
    // test_playback_mix). Linear and S-curve halves sum to one, equal power
    // in power, as session.cpp says a crossfade's two sides must.
    OK(fade_gain(FadeShape::Linear, 0.1f) ==
       sonora::fade_in_gain(sonora::FadeCurve::Linear, 0.1f));
    OK(fade_gain(FadeShape::EqualPower, 0.1f) ==
       sonora::fade_in_gain(sonora::FadeCurve::EqualPower, 0.1f));
    OK(fade_gain(FadeShape::SCurve, 0.1f) ==
       sonora::fade_in_gain(sonora::FadeCurve::SCurve, 0.1f));
    for (FadeShape shape : {FadeShape::Linear, FadeShape::EqualPower, FadeShape::SCurve}) {
        OK(fade_gain(shape, -0.5f) == 0.0f && fade_gain(shape, 0.0f) == 0.0f);
        OK(fade_gain(shape, 1.0f) == 1.0f && fade_gain(shape, 2.0f) == 1.0f);
        for (float x : {0.1f, 0.25f, 0.5f, 0.8f}) {
            const float a = fade_gain(shape, x), b = fade_gain(shape, 1.0f - x);
            OK(shape == FadeShape::EqualPower ? std::abs(a * a + b * b - 1.0f) < 1e-5f
                                              : std::abs(a + b - 1.0f) < 1e-5f);
        }
    }
    OK(fade_gain(FadeShape::Linear, 0.25f) == 0.25f);
    OK(std::abs(fade_gain(FadeShape::EqualPower, 0.5f) - 0.70710678f) < 1e-6f);
    OK(std::abs(fade_gain(FadeShape::SCurve, 0.25f) - 0.14644661f) < 1e-6f);
    OK(std::string(fade_shape_name(FadeShape::SCurve)) == "S-curve");
    OK(next_fade_shape(FadeShape::SCurve) == FadeShape::Linear);

    ModelDocument doc(two_tracks());
    EditController c(doc);
    c.set_viewport(1000, 200);
    const double th = c.view().track_height;
    const Id pl = playlist_of(doc.snapshot(), 0);
    std::string why;
    const auto x = [&](Frames f) { return c.view().x_of(f); };
    const auto range = [&](Frames a, Frames b, int track) {
        const Tool was = c.tool();
        c.set_tool(Tool::Select);
        std::string e;
        const double y = track * th + th / 2;
        const bool ok = c.press(x(a), y, e) && c.release(x(b), y, e);
        c.set_tool(was);
        return ok && c.selection().start == std::min(a, b) && c.selection().end == std::max(a, b);
    };
    const Frames half = kBar / 2;

    // --- Fade to Start / Fade to End, at the insertion point in A [1, 3) bars ---
    c.select_track(0);
    c.set_insertion(kBar);
    OK(!c.fade_to_start(why) && why.find("not inside a region") != std::string::npos);
    c.set_insertion(kBar + half);
    const uint64_t rev = doc.snapshot().revision;
    OK(c.fade_to_start(why));
    OK(region(doc.snapshot(), 10)->fade_in.length == half);
    OK(region(doc.snapshot(), 10)->fade_in.shape == FadeShape::Linear);
    OK(doc.snapshot().revision == rev + 1 && doc.can_undo());  // one operation, one undo step
    c.set_insertion(3 * kBar - half);
    c.set_fade_shape(FadeShape::SCurve);
    OK(c.fade_to_end(why));
    OK(region(doc.snapshot(), 10)->fade_out.length == half);
    OK(region(doc.snapshot(), 10)->fade_out.shape == FadeShape::SCurve);  // the last shape chosen
    // Lengthening keeps the fade's own shape.
    c.set_insertion(2 * kBar);
    OK(c.fade_to_start(why));
    OK(region(doc.snapshot(), 10)->fade_in.length == kBar &&
       region(doc.snapshot(), 10)->fade_in.shape == FadeShape::Linear);
    // Through the fade-out: refused, not shortened.
    c.set_insertion(3 * kBar - kBeat);
    OK(!c.fade_to_start(why) && why.find("run into") != std::string::npos);
    OK(region(doc.snapshot(), 10)->fade_in.length == kBar);
    OK(c.undo(why) && c.undo(why) && c.undo(why));
    OK(region(doc.snapshot(), 10)->fade_in.length == 0 && region(doc.snapshot(), 10)->fade_out.length == 0);
    c.set_fade_shape(FadeShape::Linear);

    // --- the selection: over A's start, over its end; refusals by name ---
    OK(range(half, kBar + half, 0));
    OK(c.fade_selection(why));
    OK(region(doc.snapshot(), 10)->fade_in.length == half);
    // The selection is now the fade, so Clear takes it away again.
    OK(c.selection().start == kBar && c.selection().end == kBar + half);
    OK(c.clear(why));
    OK(region(doc.snapshot(), 10)->fade_in.length == 0);
    OK(region(doc.snapshot(), 10) != nullptr);  // the fade went, not the region

    OK(range(3 * kBar - half, 3 * kBar + half, 0));
    OK(c.fade_selection(why) && region(doc.snapshot(), 10)->fade_out.length == half);
    OK(c.selection().start == 3 * kBar - half && c.selection().end == 3 * kBar);

    OK(range(half, 3 * kBar + half, 0));
    OK(!c.fade_selection(why) && why.find("whole of \"A\"") != std::string::npos);
    OK(range(kBar + half, 2 * kBar, 0));
    OK(!c.fade_selection(why) && why.find("start or end") != std::string::npos);
    OK(range(2 * kBar, 4 * kBar + half, 0));
    OK(!c.fade_selection(why) && why.find("covers 2 regions") != std::string::npos);
    OK(range(6 * kBar, 7 * kBar, 0));
    OK(!c.fade_selection(why) && why.find("no region") != std::string::npos);
    c.set_insertion(kBar);
    OK(!c.fade_selection(why) && why.find("select a time range") != std::string::npos);
    const uint64_t refused_rev = doc.snapshot().revision;

    // Fades move nothing, so Shuffle allows them where it refuses a move.
    c.set_mode(EditMode::Shuffle);
    OK(range(half, kBar + half, 0) && c.fade_selection(why));
    OK(doc.snapshot().revision == refused_rev + 1);
    c.set_mode(EditMode::Slip);

    // --- crossfades: A separated at 2 bars gives a cut with handles both sides ---
    const Id a2 = doc.reserve_id();
    OK(doc.submit(SplitRegion{pl, 10, 2 * kBar, a2}, why));
    OK(range(2 * kBar - kBeat, 2 * kBar + kBeat, 0));
    OK(c.fade_selection(why));
    {
        const Playlist& p = *active_playlist(doc.snapshot().edit->tracks[0].get());
        OK(p.crossfades.size() == 1);
        OK(p.crossfades[0].left == 10 && p.crossfades[0].right == a2);
        OK(p.crossfades[0].length == 2 * kBeat && p.crossfades[0].align == Align::Centre);
        OK(p.crossfades[0].shape == FadeShape::EqualPower);
        OK(valid(doc.snapshot()));
        // The crossfade replaces those ends' own fades, as the engine plays it.
        const RegionFades left = region_fades(p, *region(doc.snapshot(), 10));
        OK(left.out == 0 && left.out_crossfade == &p.crossfades[0] && left.in == half);
    }
    // A lopsided range is centred on the cut, and the selection says so.
    OK(range(2 * kBar - kBeat, 2 * kBar + 3 * kBeat, 0));
    OK(c.fade_selection(why));
    OK(c.selection().start == 2 * kBar - 2 * kBeat && c.selection().end == 2 * kBar + 2 * kBeat);
    // A crossfaded end is not faded on its own: by key and by the Smart Tool.
    c.set_insertion(2 * kBar - kBeat);
    OK(!c.fade_to_end(why) && why.find("crossfaded with") != std::string::npos);
    OK(c.hit(x(2 * kBar) - 3, th * 0.1).zone == HitZone::FadeOut);
    OK(!c.press(x(2 * kBar) - 3, th * 0.1, why) && why.find("crossfaded") != std::string::npos);
    // Clear on exactly the crossfade removes it.
    OK(range(2 * kBar - 2 * kBeat, 2 * kBar + 2 * kBeat, 0));
    OK(c.clear(why));
    OK(active_playlist(doc.snapshot().edit->tracks[0].get())->crossfades.empty());
    // Pre and post: the range stops at the cut.
    OK(range(2 * kBar - kBeat, 2 * kBar, 0) && c.fade_selection(why));
    {
        const Crossfade& xf = active_playlist(doc.snapshot().edit->tracks[0].get())->crossfades[0];
        OK(xf.align == Align::Pre && xf.length == kBeat);
    }
    OK(range(2 * kBar, 2 * kBar + kBeat, 0) && c.fade_selection(why));
    {
        const Crossfade& xf = active_playlist(doc.snapshot().edit->tracks[0].get())->crossfades[0];
        OK(xf.align == Align::Post && xf.length == kBeat);
    }
    // No handle: refused with the model's reason, not shortened. Track 2:
    // C ends where its source does, D starts where its source does.
    const Id pl2 = playlist_of(doc.snapshot(), 1);
    Region cr;
    cr.id = doc.reserve_id();
    cr.name = "C";
    cr.position = 0;
    cr.length = kBar;
    cr.source_offset = 19 * kBar;
    cr.channels = {{100, 0}};
    Region dr = cr;
    dr.id = doc.reserve_id();
    dr.name = "D";
    dr.position = kBar;
    dr.source_offset = 0;
    OK(doc.submit(PlaceRegion{pl2, cr, kNoId}, why) && doc.submit(PlaceRegion{pl2, dr, kNoId}, why));
    OK(range(kBar - kBeat, kBar + kBeat, 1));
    OK(!c.fade_selection(why) && why.find("no handle") != std::string::npos);
    OK(active_playlist(doc.snapshot().edit->tracks[1].get())->crossfades.empty());
    // Short handles (ADR-0021): shortened to the audio there is, the
    // placement kept, and said. E ends where its source does and F has a
    // beat of handle before its start: the centred crossfade moves before
    // the cut, where the audio is.
    const auto place = [&](const char* name, Frames at, Frames offset) {
        Region e = cr;
        e.id = doc.reserve_id();
        e.name = name;
        e.position = at;
        e.source_offset = offset;
        return doc.submit(PlaceRegion{pl2, e, kNoId}, why);
    };
    OK(place("E", 3 * kBar, 19 * kBar) && place("F", 4 * kBar, kBeat));
    OK(range(4 * kBar - kBeat, 4 * kBar + kBeat, 1) && c.fade_selection(why));
    {
        const Playlist& p2 = *active_playlist(doc.snapshot().edit->tracks[1].get());
        OK(p2.crossfades.size() == 1);
        OK(p2.crossfades[0].align == Align::Pre && p2.crossfades[0].length == kBeat);
        OK(valid(doc.snapshot()));
    }
    OK(why.find("shortened to " + std::to_string(kBeat) + " frames, before the cut") !=
       std::string::npos);
    OK(c.selection().start == 4 * kBar - kBeat && c.selection().end == 4 * kBar);
    // A post crossfade there has nothing to draw on: refused, and pointed at
    // the side that has audio.
    OK(range(4 * kBar, 4 * kBar + kBeat, 1) && !c.fade_selection(why));
    OK(why.find("\"E\" ends where its audio does") != std::string::npos &&
       why.find("select up to the cut") != std::string::npos);
    // G has 100 frames past its end, H 50 before its start: centred, the
    // longest that fits, the odd frame after the cut.
    OK(place("G", 6 * kBar, 19 * kBar - 100) && place("H", 7 * kBar, 50));
    OK(range(7 * kBar - kBeat, 7 * kBar + kBeat, 1) && c.fade_selection(why));
    {
        const Playlist& p2 = *active_playlist(doc.snapshot().edit->tracks[1].get());
        OK(p2.crossfades.size() == 2);
        OK(p2.crossfades[1].align == Align::Centre && p2.crossfades[1].length == 101);
    }
    OK(why.find("shortened to 101 frames: \"H\" has 50 frames of handle") != std::string::npos);
    OK(c.selection().start == 7 * kBar - 50 && c.selection().end == 7 * kBar + 51);
    // I ends where its source does and J has two beats before its start:
    // the centred two-beat crossfade moves before the cut at its full
    // length, which is a move, not a shortening (ADR-0021 §4).
    OK(place("I", 9 * kBar, 19 * kBar) && place("J", 10 * kBar, 2 * kBeat));
    OK(range(10 * kBar - kBeat, 10 * kBar + kBeat, 1) && c.fade_selection(why));
    {
        const Playlist& p2 = *active_playlist(doc.snapshot().edit->tracks[1].get());
        OK(p2.crossfades.size() == 3);
        OK(p2.crossfades[2].align == Align::Pre && p2.crossfades[2].length == 2 * kBeat);
    }
    OK(why.find("Crossfade moved before the cut: \"J\" has " + std::to_string(2 * kBeat) +
                " frames of handle") != std::string::npos);
    OK(why.find("shortened") == std::string::npos);
    OK(c.selection().start == 10 * kBar - 2 * kBeat && c.selection().end == 10 * kBar);
    // Room enough: nothing to say.
    OK(range(2 * kBar - kBeat, 2 * kBar + kBeat, 0) && c.fade_selection(why) && why.empty());
    OK(c.undo(why));
    // Two cuts in one range: batch fades are refused by name.
    OK(doc.submit(SplitRegion{pl, a2, 2 * kBar + half, doc.reserve_id()}, why));
    OK(range(2 * kBar - kBeat, 2 * kBar + half + kBeat, 0));
    const uint64_t two_cuts = doc.snapshot().revision;
    OK(!c.fade_selection(why) && why.find("crosses 2 cuts") != std::string::npos);
    OK(doc.snapshot().revision == two_cuts);

    // --- the Smart Tool's fade corners: the top quarter at each end ---
    ModelDocument doc2(two_tracks());
    EditController s(doc2);
    s.set_viewport(1000, 200);
    const double ax0 = s.view().x_of(kBar), ax1 = s.view().x_of(3 * kBar);
    OK(s.hit(ax0 + 3, th * 0.1).zone == HitZone::FadeIn);
    OK(s.hit(ax1 - 3, th * 0.1).zone == HitZone::FadeOut);
    OK(s.hit(ax0 + 3, th * 0.75).zone == HitZone::TrimStart);  // below the corner: still the trim
    OK(s.hit(ax0 + 40, th * 0.1).zone == HitZone::Select);
    s.set_mode(EditMode::Shuffle);  // a fade drag in Shuffle: allowed
    OK(s.press(ax0 + 3, th * 0.1, why));
    s.move(s.view().x_of(kBar + half), th * 0.1);
    OK(s.drag().kind == Drag::Kind::FadeIn && s.drag().fade == half);
    OK(doc2.snapshot().revision == 0);  // a drag in progress is not an operation
    // The preview draws the fade in the hand.
    {
        FlatPeaks peaks;
        Scene scene;
        build_timeline_scene(doc2.snapshot(), s.view(), s.selection(), s.drag(), false,
                             GridValue::Beat, peaks, scene);
        const double band = theme::kRegionNameBand;
        const double wy = 2 + band + 1, wh = th - 4 - band - 2;
        const float col = static_cast<float>(ax0 + 10.5);  // a quarter through: gain 0.26
        const SceneRect* r = rect_at(scene.rects, col, static_cast<float>(wy + wh * 0.1));
        OK(r && r->argb == theme::kFadeShade);
    }
    OK(s.release(s.view().x_of(kBar + half), th * 0.1, why));
    OK(region(doc2.snapshot(), 10)->fade_in.length == half && doc2.snapshot().revision == 1);
    // Dragged past the other end: stops at the fade-out, never through it.
    OK(s.press(ax1 - 3, th * 0.1, why));
    s.move(ax1 - 30, th * 0.1);
    OK(s.release(ax1 - 30, th * 0.1, why));
    const Frames out_len = region(doc2.snapshot(), 10)->fade_out.length;
    OK(out_len > 0);
    OK(s.press(ax0 + 3, th * 0.1, why));
    s.move(ax1 + 50, th * 0.1);
    OK(s.drag().fade == 2 * kBar - out_len);
    OK(s.release(ax1 + 50, th * 0.1, why));
    OK(region(doc2.snapshot(), 10)->fade_in.length == 2 * kBar - out_len && valid(doc2.snapshot()));
    // Dragged back past the start: the fade is removed.
    OK(s.press(ax0 + 3, th * 0.1, why));
    s.move(ax0 - 50, th * 0.1);
    OK(s.release(ax0 - 50, th * 0.1, why));
    OK(region(doc2.snapshot(), 10)->fade_in.length == 0);

    // --- the drawing: the curve, the shade above it, the crossfade box ---
    {
        FlatPeaks peaks;
        Scene scene;
        ModelDocument d3(two_tracks());
        OK(d3.submit(SetFade{pl, 10, FadeEnd::In, {kBar, FadeShape::Linear}}, why));
        const Id b2 = d3.reserve_id();
        OK(d3.submit(SplitRegion{pl, 11, 4 * kBar + half, b2}, why));
        Crossfade xf;
        xf.left = 11;
        xf.right = b2;
        xf.length = kBeat;
        OK(d3.submit(SetCrossfade{pl, xf}, why));
        // B's own fade-out, under the crossfade at its end: not heard, not drawn.
        OK(d3.submit(SetFade{pl, 11, FadeEnd::Out, {half - kBeat, FadeShape::Linear}}, why));
        EditController e(d3);
        e.set_viewport(1000, 200);
        build_timeline_scene(d3.snapshot(), e.view(), e.selection(), e.drag(), false,
                             GridValue::Beat, peaks, scene);
        const double band = theme::kRegionNameBand;
        const double wy = 2 + band + 1, wh = th - 4 - band - 2;
        const uint32_t palette = theme::region_colour(0);
        // A quarter through A's linear fade-in (80 px long): shade above the
        // curve, the waveform's colour well below it, the line on it.
        const double col = e.view().x_of(kBar) + 20;
        const double g = (20.5) / 80.0;
        const double yc = wy + (1.0 - g) * wh;
        const SceneRect* r = rect_at(scene.rects, static_cast<float>(col + 0.5), static_cast<float>(wy + 2));
        OK(r && r->argb == theme::kFadeShade);
        r = rect_at(scene.rects, static_cast<float>(col + 0.5), static_cast<float>(yc));
        OK(r && r->argb == theme::kFadeLine);
        r = rect_at(scene.rects, static_cast<float>(col + 0.5), static_cast<float>(wy + wh - 1));
        OK(r && r->argb == theme::region_body(palette));
        // Past the fade: no shade.
        r = rect_at(scene.rects, static_cast<float>(e.view().x_of(2 * kBar) + 10.5), static_cast<float>(wy + 2));
        OK(r && r->argb != theme::kFadeShade && r->argb != theme::kFadeLine);
        // The crossfade: a box over the cut, above the name band.
        r = rect_at(scene.rects, static_cast<float>(e.view().x_of(4 * kBar + half)) + 0.5f, 8.0f);
        OK(r && (r->argb == theme::kCrossfadeFill || r->argb == theme::kCrossfadeEdge));
        r = rect_at(scene.rects, static_cast<float>(e.view().x_of(4 * kBar + half) - 3.5), 8.0f);
        OK(r && r->argb == theme::kCrossfadeFill);
        // B's own fade-out, left of the crossfade: replaced, so not drawn.
        r = rect_at(scene.rects, static_cast<float>(e.view().x_of(4 * kBar + half - kBeat) - 4.5),
                    static_cast<float>(wy + 2));
        OK(r && r->argb != theme::kFadeShade);
    }
    return 0;
}

// A crossfade replaces the fade at the end it joins, but the model keeps that
// fade and still counts it (fade_in + fade_out <= length). And a crossfade
// must not overlap the region's own fade at its other end, which the engine
// would multiply. The model refuses that too (PR #69); it is checked here
// first, where the operator acts, so a refusal names something they can see.
int test_fades_against_crossfades() {
    const Frames half = kBar / 2;
    std::string why;

    // --- the hidden fade: A [1, 3) bars with a 1-bar fade-out, then a 1-beat
    // Pre crossfade into B, moved to butt at 3 bars ---
    {
        ModelDocument doc(two_tracks());
        const Id pl = playlist_of(doc.snapshot(), 0);
        OK(doc.submit(MoveRegion{pl, 11, 3 * kBar, kNoId}, why));
        OK(doc.submit(SetFade{pl, 10, FadeEnd::Out, {kBar, FadeShape::Linear}}, why));
        Crossfade xf;
        xf.left = 10;
        xf.right = 11;
        xf.length = kBeat;
        xf.align = Align::Pre;
        OK(doc.submit(SetCrossfade{pl, xf}, why));
        OK(region_fades(*active_playlist(doc.snapshot().edit->tracks[0].get()),
                        *region(doc.snapshot(), 10)).out == 0);  // not drawn, not heard
        EditController c(doc);
        c.set_viewport(1000, 200);
        const double th = c.view().track_height;
        // By key: refused by the clamp, naming the fade under the crossfade,
        // not by the model naming fade_out.length.
        const uint64_t rev = doc.snapshot().revision;
        c.select_track(0);
        c.set_insertion(kBar + kBar + half);
        OK(!c.fade_to_start(why));
        OK(why.find("under its crossfade") != std::string::npos &&
           why.find("fade_out.length") == std::string::npos);
        OK(doc.snapshot().revision == rev);
        // By the Smart Tool: the drag stops where the model's count stops it.
        const double ax0 = c.view().x_of(kBar);
        OK(c.press(ax0 + 3, th * 0.1, why));
        c.move(c.view().x_of(kBar + kBar + half), th * 0.1);
        OK(c.drag().fade == kBar);
        OK(c.release(c.view().x_of(kBar + kBar + half), th * 0.1, why));
        OK(region(doc.snapshot(), 10)->fade_in.length == kBar && valid(doc.snapshot()));
    }

    // --- a crossfade made here drops the fades at its cut, as Pro Tools
    // does: A's 1-bar fade-out and B's 1-beat fade-in go with the Pre
    // crossfade, in one compound and one undo step that brings them back ---
    {
        ModelDocument doc(two_tracks());
        const Id pl = playlist_of(doc.snapshot(), 0);
        OK(doc.submit(MoveRegion{pl, 11, 3 * kBar, kNoId}, why));
        OK(doc.submit(SetFade{pl, 10, FadeEnd::Out, {kBar, FadeShape::Linear}}, why));
        OK(doc.submit(SetFade{pl, 11, FadeEnd::In, {kBeat, FadeShape::Linear}}, why));
        EditController c(doc);
        c.set_viewport(1000, 200);
        const double th = c.view().track_height;
        c.set_tool(Tool::Select);
        OK(c.press(c.view().x_of(3 * kBar - kBeat), th / 2, why) &&
           c.release(c.view().x_of(3 * kBar), th / 2, why));
        const uint64_t rev = doc.snapshot().revision;
        const std::size_t depth = doc.history().undo_depth();
        OK(c.fade_selection(why));
        OK(doc.snapshot().revision == rev + 1 && doc.history().undo_depth() == depth + 1);
        OK(doc.undo_label() == "Crossfade");
        OK(active_playlist(doc.snapshot().edit->tracks[0].get())->crossfades.size() == 1);
        OK(region(doc.snapshot(), 10)->fade_out.length == 0);
        OK(region(doc.snapshot(), 11)->fade_in.length == 0);
        OK(valid(doc.snapshot()));
        // Nothing waits under it: A's fade-in may now take the whole region
        // up to the crossfade, which the hidden 1-bar fade-out refused.
        c.set_insertion(3 * kBar - kBeat);
        OK(c.fade_to_start(why));
        OK(region(doc.snapshot(), 10)->fade_in.length == 2 * kBar - kBeat);
        OK(c.undo(why));
        // Clear brings back nothing.
        OK(c.press(c.view().x_of(3 * kBar - kBeat), th / 2, why) &&
           c.release(c.view().x_of(3 * kBar), th / 2, why));
        OK(c.clear(why));
        OK(active_playlist(doc.snapshot().edit->tracks[0].get())->crossfades.empty());
        OK(region(doc.snapshot(), 10)->fade_out.length == 0);
        OK(region(doc.snapshot(), 11)->fade_in.length == 0);
        // Undo of the clear, then of the crossfade: both fades come back.
        OK(c.undo(why) && c.undo(why));
        OK(active_playlist(doc.snapshot().edit->tracks[0].get())->crossfades.empty());
        OK(region(doc.snapshot(), 10)->fade_out.length == kBar);
        OK(region(doc.snapshot(), 11)->fade_in.length == kBeat);
        // With no fade at the cut it is still the one SetCrossfade.
        OK(doc.submit(SetFade{pl, 10, FadeEnd::Out, Fade{}}, why));
        OK(doc.submit(SetFade{pl, 11, FadeEnd::In, Fade{}}, why));
        OK(c.press(c.view().x_of(3 * kBar - kBeat), th / 2, why) &&
           c.release(c.view().x_of(3 * kBar), th / 2, why));
        OK(c.fade_selection(why) && doc.undo_label() == "SetCrossfade");
    }

    // --- the overlap: a crossfade into the region's own fade at the other end ---
    {
        ModelDocument doc(two_tracks());
        const Id pl = playlist_of(doc.snapshot(), 0);
        OK(doc.submit(MoveRegion{pl, 11, 3 * kBar, kNoId}, why));
        OK(doc.submit(SetFade{pl, 10, FadeEnd::In, {kBar + 3 * kBeat, FadeShape::Linear}}, why));
        EditController c(doc);
        c.set_viewport(1000, 200);
        const double th = c.view().track_height;
        const auto range = [&](Frames a, Frames b) {
            c.set_tool(Tool::Select);
            std::string e;
            const double x0 = c.view().x_of(a), x1 = c.view().x_of(b);
            const bool ok = c.press(x0, th / 2, e) && c.release(x1, th / 2, e);
            c.set_tool(Tool::Smart);
            return ok;
        };
        const uint64_t rev = doc.snapshot().revision;
        // Centred, 1 bar: half a bar into A, which has 1.75 bars of fade-in.
        OK(range(3 * kBar - half, 3 * kBar + half));
        OK(!c.fade_selection(why));
        OK(why.find("\"A\"'s fade-in") != std::string::npos);
        OK(doc.snapshot().revision == rev);
        OK(active_playlist(doc.snapshot().edit->tracks[0].get())->crossfades.empty());
        // A Pre crossfade of one beat just meets it: allowed.
        OK(range(3 * kBar - kBeat, 3 * kBar) && c.fade_selection(why));
        OK(active_playlist(doc.snapshot().edit->tracks[0].get())->crossfades.size() == 1);
        OK(valid(doc.snapshot()));
        // The mirror: B [3, 4) with a 0.75-bar fade-out, a half-bar Post crossfade.
        OK(c.undo(why));
        OK(doc.submit(SetFade{pl, 11, FadeEnd::Out, {3 * kBeat, FadeShape::Linear}}, why));
        const uint64_t rev2 = doc.snapshot().revision;
        OK(range(3 * kBar, 3 * kBar + half));
        OK(!c.fade_selection(why));
        OK(why.find("\"B\"'s fade-out") != std::string::npos);
        OK(doc.snapshot().revision == rev2);
    }

    // --- the hidden fade at the other end (PR #58 review, note 1) ---
    // P [0, 1) butts A [1, 3) butts B [3, 4). A keeps a 1.75-bar fade-in
    // under a 1-beat Pre crossfade with P. Regression: a centred 1-bar
    // crossfade at A's end counted only the crossfade heard at A's start, so
    // it was accepted; clearing P|A then brought the fade-in back over it,
    // 0.25 bar of two fades the engine multiplies.
    {
        Snapshot s = two_tracks();
        {
            auto edit = std::make_shared<EditState>(*s.edit);
            auto t = std::make_shared<TrackEdit>(*edit->tracks[0]);
            auto p = std::make_shared<Playlist>(*t->playlists[0]);
            Region pr = p->regions[0];
            pr.id = 12;
            pr.name = "P";
            pr.position = 0;
            pr.length = kBar;
            pr.source_offset = 0;
            p->regions.insert(p->regions.begin(), pr);
            t->playlists[0] = p;
            edit->tracks[0] = t;
            s.edit = edit;
        }
        ModelDocument doc(std::move(s));
        OK(valid(doc.snapshot()));
        const Id pl = playlist_of(doc.snapshot(), 0);
        OK(doc.submit(MoveRegion{pl, 11, 3 * kBar, kNoId}, why));
        OK(doc.submit(SetFade{pl, 10, FadeEnd::In, {kBar + 3 * kBeat, FadeShape::Linear}}, why));
        Crossfade start;
        start.left = 12;
        start.right = 10;
        start.length = kBeat;
        start.align = Align::Pre;
        OK(doc.submit(SetCrossfade{pl, start}, why));
        EditController c(doc);
        c.set_viewport(1000, 200);
        const double th = c.view().track_height;
        const auto range = [&](Frames a, Frames b) {
            c.set_tool(Tool::Select);
            std::string e;
            const double x0 = c.view().x_of(a), x1 = c.view().x_of(b);
            const bool ok = c.press(x0, th / 2, e) && c.release(x1, th / 2, e);
            c.set_tool(Tool::Smart);
            return ok;
        };
        const auto crossfades = [&]() {
            return active_playlist(doc.snapshot().edit->tracks[0].get())->crossfades.size();
        };
        const uint64_t rev = doc.snapshot().revision;
        OK(range(3 * kBar - half, 3 * kBar + half));
        OK(!c.fade_selection(why));
        OK(why.find("\"A\"'s fade-in") != std::string::npos &&
           why.find("under its crossfade with \"P\"") != std::string::npos);
        OK(doc.snapshot().revision == rev && crossfades() == 1);
        // A Pre crossfade of a quarter bar fits beside the hidden fade-in.
        OK(range(3 * kBar - kBeat, 3 * kBar) && c.fade_selection(why));
        OK(crossfades() == 2);
        OK(c.undo(why));

        // A document that already has the overlap (made before this check,
        // or by another interface): clearing P|A brings A's fade-in back
        // shortened to meet the crossfade at A's end, as the model does on
        // an edge edit (PR #69). Refusing would block the operator over a
        // fade they cannot see or edit while A's end is crossfaded.
        Crossfade end;
        end.left = 10;
        end.right = 11;
        end.length = kBar;
        end.align = Align::Centre;
        OK(doc.submit(SetCrossfade{pl, end}, why));
        OK(range(kBar - kBeat, kBar));
        OK(c.clear(why));
        OK(crossfades() == 1 && region(doc.snapshot(), 10)->fade_in.length == 2 * kBar - half);
        OK(valid(doc.snapshot()));
        // Undo brings back the crossfade and the full hidden fade-in.
        OK(c.undo(why));
        OK(crossfades() == 2 && region(doc.snapshot(), 10)->fade_in.length == kBar + 3 * kBeat);
        // The other way round: clearing A|B brings back nothing (A has no
        // stored fade-out), so it is cleared; then P|A clears too.
        OK(range(3 * kBar - half, 3 * kBar + half));
        OK(c.clear(why));
        OK(crossfades() == 1);
        OK(range(kBar - kBeat, kBar));
        OK(c.clear(why));
        OK(crossfades() == 0 && region(doc.snapshot(), 10)->fade_in.length == kBar + 3 * kBeat);
        OK(valid(doc.snapshot()));
    }
    return 0;
}

int test_scene() {
    ModelDocument doc(two_tracks());
    EditController c(doc);
    c.set_viewport(1000, 200);
    FlatPeaks peaks;
    Scene scene;
    build_timeline_scene(doc.snapshot(), c.view(), c.selection(), c.drag(), false,
                         GridValue::Beat, peaks, scene);
    const double th = c.view().track_height;
    const uint32_t palette = theme::region_colour(0);
    const float ax = static_cast<float>(c.view().x_of(kBar) + 30);
    // Region A: name band at the top, body below it, waveform through the
    // middle (FlatPeaks: half height).
    const SceneRect* r = rect_at(scene.rects, ax, 4);
    OK(r && r->argb == theme::region_name_band(palette));
    r = rect_at(scene.rects, ax, static_cast<float>(th - 4));
    OK(r && r->argb == theme::region_body(palette));
    const double band = theme::kRegionNameBand;
    const float mid = static_cast<float>(2 + band + 1 + (th - 4 - band - 2) / 2);
    r = rect_at(scene.rects, ax, mid);
    OK(r && r->argb == theme::region_wave(palette));
    // Between regions: the lane. Track 2: the other lane colour.
    r = rect_at(scene.rects, static_cast<float>(c.view().x_of(3 * kBar + kBeat)), 30);
    OK(r && (r->argb == theme::kLaneA || r->argb == theme::kGridBeat));
    r = rect_at(scene.rects, 5, static_cast<float>(th + 30));
    OK(r && r->argb == theme::kLaneB);
    // The region's name is a label in its band.
    bool named = false;
    for (const auto& l : scene.labels) named = named || l.text == "A";
    OK(named);

    // Selected: the outline is the accent.
    std::string why;
    OK(c.press(ax, static_cast<float>(th * 0.75), why) && c.release(ax, static_cast<float>(th * 0.75), why));
    build_timeline_scene(doc.snapshot(), c.view(), c.selection(), c.drag(), false,
                         GridValue::Beat, peaks, scene);
    r = rect_at(scene.rects, static_cast<float>(c.view().x_of(kBar)) + 0.5f, 30);
    OK(r && r->argb == theme::kAccent);

    // The overlay: the insertion line while stopped, the playhead (red when
    // recording) while rolling.
    std::vector<SceneRect> overlay;
    build_timeline_overlay(c.view(), c.selection(), TransportOverlay{}, overlay);
    OK(overlay.size() == 1 && overlay[0].argb == theme::kAccent);
    build_timeline_overlay(c.view(), c.selection(), TransportOverlay{kRate, true, true}, overlay);
    OK(overlay.size() == 1 && overlay[0].argb == theme::kRecord);
    OK(std::abs(overlay[0].x - 39.5f) < 1.0f);

    // Grid mode draws the grid value's lines.
    build_timeline_scene(doc.snapshot(), c.view(), c.selection(), c.drag(), true,
                         GridValue::Beat, peaks, scene);
    r = rect_at(scene.rects, static_cast<float>(std::floor(c.view().x_of(kBeat))), th * 1.5f);
    OK(r && r->argb == theme::kGridBeat);
    r = rect_at(scene.rects, static_cast<float>(std::floor(c.view().x_of(4 * kBar + 0))) , th * 1.5f);
    OK(r && r->argb == theme::kGridBar);

    // The ruler: background, and ticks under its labels.
    build_ruler_scene(TimeFormat::BarsBeats, doc.snapshot().settings, c.view(), 20, scene);
    OK(rect_at(scene.rects, 500, 2)->argb == theme::kPanel);
    OK(!scene.labels.empty() && scene.labels[0].text == "1");
    return 0;
}

// --- keys, through the profiles, into the Edit and Mix windows -------------

int route(const char* profile, const std::vector<std::pair<std::string, std::string>>& keys) {
    KeyMap map;
    std::string err;
    CHECK(KeyMap::load_file(std::string(SONORA_KEYMAP_DIR) + "/" + profile, map, err));
    ModelDocument doc(two_tracks());
    FlatPeaks peaks;
    SessionModel session(doc, peaks, false);
    std::vector<std::string> sent;
    KeyRouter router(map, api::testing::FakeEngine::engine_commands(), [&sent](const std::string& c, const CommandContext& ctx) {
        sent.push_back(c + "@" + std::to_string(ctx.target));
        return static_cast<uint64_t>(sent.size());
    });
    session.installCommands(router.local());
    router.setSelection([&session]() { return session.selectedTrackForCommands(); });
    router.prepare(qt_device_chords(true));
    session.setViewport(1000, 200);

    const auto press = [&](const std::string& chord) {
        KeyChord k;
        std::string e;
        if (!KeyMap::parse_chord(chord, k, e)) return RouteResult{};
        return router.route(k);
    };
    for (const auto& [chord, expect] : keys) {
        const RouteResult r = press(chord);
        if (r.command != expect || (r.kind != RouteResult::Kind::Local &&
                                    r.kind != RouteResult::Kind::Sent)) {
            std::fprintf(stderr, "FAIL %s: %s gave %s (kind %d)\n", profile, chord.c_str(),
                         r.command.c_str(), static_cast<int>(r.kind));
            return 1;
        }
        ++checks;
    }

    // Effects, not just names: the window toggles, the mode changes, the
    // selection is the target of a target-less track key.
    CHECK(session.window() == "edit");
    return 0;
}

int test_routing() {
    if (route("protools.json", {{"ctrl+equal", "window.toggle_mix_edit"},
                                {"f4", "edit.mode.grid"},
                                {"f2", "edit.mode.slip"},
                                {"f8", "tool.grab"},
                                {"ctrl+7", "tool.smart"},
                                {"ctrl+bracketright", "zoom.in"},
                                {"ctrl+bracketleft", "zoom.out"},
                                {"alt+a", "zoom.fit_session"},
                                {"ctrl+e", "edit.separate_at_selection"},
                                {"b", "edit.separate_at_selection"},
                                {"a", "edit.trim_start_to_insertion"},
                                {"s", "edit.trim_end_to_insertion"},
                                {"ctrl+b", "edit.clear"},
                                {"d", "edit.fade_to_start"},
                                {"f", "edit.fade_selection"},
                                {"g", "edit.fade_to_end"},
                                {"ctrl+z", "edit.undo"},
                                {"ctrl+shift+z", "edit.redo"},
                                {"numpad_plus", "nudge.forward"},
                                {"shift+s", "track.toggle_solo"},
                                {"shift+m", "track.toggle_mute"},
                                {"semicolon", "track.select_next"},
                                {"ctrl+equal", "window.toggle_mix_edit"}})) {
        return 1;
    }
    if (route("logic.json", {{"x", "window.toggle_mix_edit"},
                             {"cmd+t", "edit.separate_at_selection"},
                             {"delete", "edit.clear"},
                             {"ctrl+alt+x", "edit.fade_selection"},
                             {"cmd+z", "edit.undo"},
                             {"cmd+shift+z", "edit.redo"},
                             {"cmd+right", "zoom.in"},
                             {"cmd+bracketleft", "edit.trim_start_to_insertion"},
                             {"alt+right", "nudge.forward"},
                             {"down", "track.select_next"},
                             {"s", "track.toggle_solo"},
                             {"x", "window.toggle_mix_edit"}})) {
        return 1;
    }

    // What the keys do, end to end, with Pro Tools keys.
    KeyMap map;
    std::string err;
    CHECK(KeyMap::load_file(std::string(SONORA_KEYMAP_DIR) + "/protools.json", map, err));
    ModelDocument doc(two_tracks());
    FlatPeaks peaks;
    SessionModel session(doc, peaks, false);
    std::vector<std::string> sent;
    KeyRouter router(map, api::testing::FakeEngine::engine_commands(), [&sent](const std::string& c, const CommandContext& ctx) {
        sent.push_back(c + "@" + std::to_string(ctx.target));
        return static_cast<uint64_t>(sent.size());
    });
    session.installCommands(router.local());
    router.setSelection([&session]() { return session.selectedTrackForCommands(); });
    router.prepare(qt_device_chords(true));
    session.setViewport(1000, 200);
    const auto key = [&](const char* chord) {
        KeyChord k;
        std::string e;
        KeyMap::parse_chord(chord, k, e);
        return router.route(k);
    };

    OK(session.window() == "edit");
    key("ctrl+equal");
    OK(session.window() == "mix");
    key("ctrl+equal");
    OK(session.window() == "edit");
    key("f4");
    OK(session.editMode() == "Grid");
    key("f2");
    OK(session.editMode() == "Slip");
    const double fpp = session.controller().view().fpp;
    key("ctrl+bracketright");
    OK(session.controller().view().fpp == fpp / 2);

    // Insertion inside A, then B separates it; Ctrl+Z puts it back.
    session.controller().set_insertion(2 * kBar);
    const RouteResult sep = key("b");
    OK(sep.result == DispatchResult::Ok);
    OK(doc.snapshot().edit->tracks[0]->playlists[0]->regions.size() == 3);
    key("ctrl+z");
    OK(doc.snapshot().edit->tracks[0]->playlists[0]->regions.size() == 2);
    key("ctrl+shift+z");
    OK(doc.snapshot().edit->tracks[0]->playlists[0]->regions.size() == 3);

    // Shift+S solos the selected track (track 1 by default), Shift+M mutes.
    key("shift+s");
    OK(session.solos().at(0) && !session.solos().at(1));
    key("semicolon");  // select the next track down
    key("shift+m");
    OK(session.mutes().at(1));
    // Shift+I puts it in Input mode (Pro Tools' TrackInput), in the
    // interface as Shift+M is: the mode is the document's.
    key("shift+i");
    OK(session.inputMonitors() == QList<bool>({false, true}));
    // The target-less arm key now arms the selection, via the engine.
    key("shift+r");
    OK(!sent.empty() && sent.back() == "track.toggle_arm@1");

    // A refusal is said, with the reason.
    const RouteResult r = key("ctrl+b");  // nothing selected to clear
    OK(r.result == DispatchResult::Failed);
    OK(r.message.find("nothing is selected") != std::string::npos);
    return 0;
}

// --- File > Save and Open, through SessionModel and common/model's codec ------

bool contains(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }

QByteArray file_bytes(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// ADR-0018 §4: a first Save goes into the folder the engine says it records
// the session into, named after that folder, so the session file is beside
// its takes. A folder that already holds another session's file is not
// shared: the save goes to a new folder under the sessions folder, as
// before the engine said where.
int test_first_save_beside_its_takes() {
    QTemporaryDir dir;
    CHECK(dir.isValid());
    FlatPeaks peaks;
    std::string why;
    const QString engine_folder = dir.filePath(QStringLiteral("Untitled"));
    CHECK(QDir(dir.path()).mkpath(QStringLiteral("Untitled/Audio Files")));

    ModelDocument doc(make_demo_session(4, kRate));
    doc.set_saver(engine_saver());
    SessionModel session(doc, peaks, true);
    session.setSessionsDir(dir.path());
    session.setEngineFolder(engine_folder);
    OK(session.saveSession(why));
    const QString file = engine_folder + QStringLiteral("/Untitled.sonora");
    OK(session.sessionFile() == file);
    OK(QFileInfo(file).isFile());
    OK(doc.snapshot().settings.name == "Untitled");
    OK(!QFileInfo(dir.filePath(QStringLiteral("Untitled 2"))).exists());

    // A new session in the same folder: Untitled.sonora is there already.
    ModelDocument next(make_demo_session(4, kRate));
    next.set_saver(engine_saver());
    SessionModel second(next, peaks, true);
    second.setSessionsDir(dir.path());
    second.setEngineFolder(engine_folder);
    OK(second.saveSession(why));
    OK(second.sessionFile() == dir.filePath(QStringLiteral("Demo session/Demo session.sonora")));
    OK(file_bytes(file) == file_bytes(session.sessionFile()));
    return 0;
}

// PR #142 review N2, ADR-0018 §3: a first Save while the engine records into
// an untitled folder asks for <sessions root>/<name>/<name>.sonora, which the
// engine renames that folder to, so the session keeps the name the operator
// gave it; it used to be saved as "Untitled", after the folder. A session
// with no name of its own is saved in its folder, under the folder's name.
int test_first_save_keeps_the_sessions_name() {
    QTemporaryDir dir;
    CHECK(dir.isValid());
    FlatPeaks peaks;
    std::string why;
    const QString untitled = dir.filePath(QStringLiteral("Untitled 3"));
    CHECK(QDir(dir.path()).mkpath(QStringLiteral("Untitled 3/Audio Files")));
    CHECK(QDir(dir.path()).mkpath(QStringLiteral("Bass")));

    Snapshot named = make_demo_session(4, kRate);
    named.settings.name = "Bass";
    ModelDocument doc(named);
    doc.set_saver(engine_saver());
    SessionModel session(doc, peaks, true);
    session.setSessionsDir(dir.filePath(QStringLiteral("elsewhere")));
    session.setEngineFolder(untitled, true);
    OK(session.saveSession(why));
    // "Bass" is taken, so the first free name, as for any first Save; and
    // under the folder the engine's folder is in, not the interface's own.
    OK(session.sessionFile() == dir.filePath(QStringLiteral("Bass 2/Bass 2.sonora")));
    OK(doc.snapshot().settings.name == "Bass 2");

    Snapshot plain = make_demo_session(4, kRate);
    plain.settings.name = "Untitled";
    ModelDocument next(plain);
    next.set_saver(engine_saver());
    SessionModel second(next, peaks, true);
    second.setSessionsDir(dir.path());
    second.setEngineFolder(untitled, true);
    OK(second.saveSession(why));
    OK(second.sessionFile() == untitled + QStringLiteral("/Untitled 3.sonora"));
    OK(next.snapshot().settings.name == "Untitled 3");
    return 0;
}

int test_files() {
    QTemporaryDir dir;
    CHECK(dir.isValid());
    FlatPeaks peaks;
    std::string why;

    ModelDocument doc(make_demo_session(4, kRate));
    doc.set_saver(engine_saver());
    SessionModel session(doc, peaks, true);
    session.setSessionsDir(dir.path());
    QStringList notices;
    QObject::connect(&session, &SessionModel::notice, [&notices](const QString& t) { notices << t; });
    OK(session.sessionName() == QStringLiteral("Demo session · synthetic audio"));
    OK(!session.dirty());

    // Ctrl+S is app.save, which the window runs: it names the file and
    // serialises the session, and the engine (its saver, standing in here)
    // writes it.
    KeyMap map;
    std::string err;
    CHECK(KeyMap::load_file(std::string(SONORA_KEYMAP_DIR) + "/protools.json", map, err));
    KeyRouter router(map, api::testing::FakeEngine::engine_commands(),
                     [](const std::string&, const CommandContext&) { return uint64_t{1}; });
    session.installCommands(router.local());
    router.prepare(qt_device_chords(true));
    KeyChord ctrl_s;
    CHECK(KeyMap::parse_chord("ctrl+s", ctrl_s, err));
    const RouteResult saved = router.route(ctrl_s);
    OK(saved.kind == RouteResult::Kind::Local && saved.result == DispatchResult::Ok);
    const QString file = dir.filePath(QStringLiteral("Demo session/Demo session.sonora"));
    OK(QFileInfo(file).isFile());
    OK(session.sessionFile() == file);
    OK(!session.dirty());
    OK(notices.size() == 1 && notices.back().startsWith(QStringLiteral("Saved ")));

    // Nothing changed: nothing written, and the generation stays.
    const QByteArray first = file_bytes(file);
    OK(session.saveSession(why));
    OK(notices.back().startsWith(QStringLiteral("Nothing to save")));
    OK(file_bytes(file) == first);

    // An edit and a fader move, then save: both are in the file.
    session.selectTrack(0);
    session.controller().set_insertion(2 * kBar);
    OK(session.controller().separate(why));
    OK(session.dirty());
    OK(session.sessionName().contains(QStringLiteral("edited")));
    OK(session.controller().set_gain(1, -12.5f, why));
    OK(session.saveSession(why));
    OK(!session.dirty() && file_bytes(file) != first);
    {
        std::string bytes;
        Loaded loaded;
        OK(read_session_file(file, bytes, loaded, why));
        OK(loaded.save_generation == 2);
        OK(loaded.written_by == "sonora-ui");
    }

    // Open it in another window's document: the same session, no undo
    // into the one that was there before, the selection back at track 1.
    ModelDocument other(make_empty_session(2, kRate));
    other.set_saver(engine_saver());
    SessionModel opener(other, peaks, true);
    opener.setSessionsDir(dir.path());
    const QVariantList listed = opener.sessionList();
    OK(listed.size() == 1);
    OK(listed[0].toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Demo session"));
    std::string edit_why;
    OK(opener.controller().set_gain(0, -3.0f, edit_why));  // the empty session's own edit
    OK(opener.dirty());
    OK(!opener.openSessionFile(file, false, why) && contains(why, "unsaved"));
    OK(other.snapshot().edit->tracks.size() == 2);  // refused: untouched
    OK(opener.openSessionFile(file, true, why));
    OK(same_document(other.snapshot(), doc.snapshot()));
    OK(!other.can_undo() && !other.can_redo());
    OK(!opener.dirty());
    OK(opener.sessionFile() == file);
    OK(opener.controller().selection().track == 0);
    OK(opener.sessionList()[0].toMap().value(QStringLiteral("current")).toBool());
    OK(other.snapshot().mix.find(other.snapshot().edit->tracks[1]->id)->gain_db == -12.5f);
    // Editing the opened session allocates ids past every one in the file.
    opener.controller().set_insertion(kBar / 2);
    OK(opener.controller().separate(why));
    OK(valid(other.snapshot()));
    OK(opener.controller().undo(why));

    // A file that does not parse is refused with where, and changes nothing.
    QDir(dir.path()).mkpath(QStringLiteral("Broken"));
    {
        QFile broken(dir.filePath(QStringLiteral("Broken/Broken.sonora")));
        CHECK(broken.open(QIODevice::WriteOnly));
        broken.write("{ \"format\": ");
    }
    const uint64_t before = other.revision();
    OK(!opener.openSessionFile(dir.filePath(QStringLiteral("Broken/Broken.sonora")), true, why));
    OK(contains(why, "Broken.sonora") && contains(why, "line"));
    OK(other.revision() == before);
    OK(opener.sessionList().size() == 2);

    // A session at another rate cannot play here, and says so.
    ModelDocument slow(make_empty_session(1, 44100));
    slow.set_saver(engine_saver());
    SessionModel slow_session(slow, peaks, true);
    slow_session.setSessionsDir(dir.path());
    OK(slow.snapshot().settings.session_id.empty());
    OK(slow_session.saveSession(why));  // no name: "Untitled"
    OK(slow.snapshot().settings.name == "Untitled");
    // A session with no id, as one saved before format 7 opens, is given
    // one by its first save, and the next save keeps it.
    const std::string slow_id = slow.snapshot().settings.session_id;
    OK(slow_id.size() == 36 && valid_session_id(slow_id));
    OK(slow_session.saveSession(why));
    OK(slow.snapshot().settings.session_id == slow_id);
    OK(slow_session.sessionName() == QStringLiteral("Untitled"));  // no regions: nothing synthetic
    const QString untitled = dir.filePath(QStringLiteral("Untitled/Untitled.sonora"));
    OK(!opener.openSessionFile(untitled, true, why) && contains(why, "44100"));
    ModelDocument slow2(make_empty_session(1, 44100));
    slow2.set_saver(engine_saver());
    SessionModel slow_session2(slow2, peaks, true);
    slow_session2.setSessionsDir(dir.path());
    OK(slow_session2.saveSession(why));
    OK(slow2.snapshot().settings.name == "Untitled 2");  // never over another session

    // A file an older build saved with a fade running into the crossfade at
    // its region's other end (PR #69): it opens, the fade shortened, and the
    // notice says so. Made here as that build's trim left it: B's fade-out
    // met a centred 1-bar crossfade with A, then grew past it.
    {
        ModelDocument old(two_tracks());
        const Id pl = playlist_of(old.snapshot(), 0);
        OK(old.submit(MoveRegion{pl, 11, 3 * kBar, kNoId}, why));
        OK(old.submit(SetFade{pl, 11, FadeEnd::Out, {kBar / 2, FadeShape::Linear}}, why));
        Crossfade xf;
        xf.left = 10;
        xf.right = 11;
        xf.length = kBar;
        xf.align = Align::Centre;
        OK(old.submit(SetCrossfade{pl, xf}, why));
        std::string bytes, err;
        OK(serialize(old.snapshot(), WriteInfo{"sonora-ui", 1}, bytes, err));
        QByteArray text = QByteArray::fromStdString(bytes);
        OK(text.count("\"length\": 48000") == 1);
        text.replace("\"length\": 48000", "\"length\": 72000");
        QDir(dir.path()).mkpath(QStringLiteral("Old"));
        QFile f(dir.filePath(QStringLiteral("Old/Old.sonora")));
        CHECK(f.open(QIODevice::WriteOnly));
        f.write(text);
        f.close();
        QStringList said;
        QObject::connect(&opener, &SessionModel::notice, [&said](const QString& t) { said << t; });
        OK(opener.openSessionFile(f.fileName(), true, why));
        OK(region(other.snapshot(), 11)->fade_out.length == kBar / 2);
        OK(valid(other.snapshot()));
        OK(!said.isEmpty() && said.back().startsWith(QStringLiteral("Opened ")) &&
           said.back().contains(QStringLiteral("1 value repaired")));
        // The repair is a notice of its own, by key path with the old and
        // the new, held rather than said once on the status line.
        int held = 0;
        QObject::connect(&opener, &SessionModel::repairsChanged, [&held]() { ++held; });
        OK(opener.repairs().size() == 1);
        const QVariantMap r = opener.repairs().at(0).toMap();
        OK(r.value(QStringLiteral("path")).toString() ==
           QStringLiteral("tracks[0].playlists[0].regions[1].fade_out.length"));
        OK(r.value(QStringLiteral("was")).toString() == QStringLiteral("72000 frames"));
        OK(r.value(QStringLiteral("now")).toString() == QStringLiteral("48000 frames"));
        OK(r.value(QStringLiteral("why")).toString().contains(QStringLiteral("crossfade")));
        // Modified: what is shown is not the file on disk, so another Open
        // asks first and Save writes it.
        OK(opener.dirty());
        OK(!opener.openSessionFile(file, false, why) && contains(why, "unsaved"));
        OK(opener.saveSession(why));
        OK(!opener.dirty());
        Loaded saved;
        OK(read_session_file(f.fileName(), bytes, saved, why) && saved.repairs.empty());
        OK(saved.file_version == kFormatVersion);
        OK(!saved.snapshot.settings.session_id.empty() &&
           saved.snapshot.settings.session_id == other.snapshot().settings.session_id);
        // Saving does not dismiss it; the operator does.
        OK(opener.repairs().size() == 1 && held == 0);
        opener.dismissRepair(5);
        OK(opener.repairs().size() == 1 && held == 0);
        opener.dismissRepair(0);
        OK(opener.repairs().isEmpty() && held == 1);
        // A session opened over a repaired one takes its notices away.
        OK(opener.openSessionFile(f.fileName(), true, why));
        OK(opener.repairs().isEmpty() && !opener.dirty());
    }

    // Not while a take is open.
    TransportSnapshot rec;
    rec.state = TransportMode::Recording;
    opener.onTransport(rec, true);
    OK(!opener.openSessionFile(file, true, why) && contains(why, "take is open"));

    // A name that cannot be a folder is refused, not rewritten.
    SessionSettings bad = slow.snapshot().settings;
    bad.name = "Verse/Chorus";
    ModelDocument named(make_empty_session(1, kRate));
    bad.sample_rate = kRate;
    bad.timecode = named.snapshot().settings.timecode;
    OK(named.submit(SetSettings{bad}, why));
    SessionModel named_session(named, peaks, true);
    named_session.setSessionsDir(dir.path());
    OK(!named_session.saveSession(why) && contains(why, "cannot save"));
    return 0;
}

// A session named `name`, at the test rate.
Snapshot named_session(const char* name) {
    Snapshot s = make_empty_session(1, kRate);
    s.settings.name = name;
    return s;
}

// What the engine says when the disk is full: the save did not happen.
bool full_disk(const std::string&, const std::string&, const Snapshot&, std::string& why,
               std::string&) {
    why = "writing the session file failed: no space left on the device";
    return false;
}

int test_save_failures() {
    QTemporaryDir dir;
    CHECK(dir.isValid());
    FlatPeaks peaks;
    std::string why;
    const QDir d(dir.path());

    // A first save that fails leaves no folder and does not rename the
    // session, so the retry is "Foo", not "Foo 2" (nor "Foo 2 2").
    ModelDocument doc(named_session("Foo"));
    doc.set_saver(full_disk);
    SessionModel session(doc, peaks, true);
    session.setSessionsDir(dir.path());
    QStringList notices;
    QObject::connect(&session, &SessionModel::notice, [&notices](const QString& t) { notices << t; });
    for (int attempt = 0; attempt < 2; ++attempt) {
        OK(!session.saveSession(why) && contains(why, "no space left"));
        OK(doc.snapshot().settings.name == "Foo");
        OK(!d.exists(QStringLiteral("Foo")));
        OK(session.sessionFile().isEmpty());
        OK(!session.saveWaiting());
    }
    doc.set_saver(engine_saver());
    OK(session.saveSession(why));
    OK(doc.snapshot().settings.name == "Foo");
    OK(session.sessionFile() == session_file_path(dir.path(), QStringLiteral("Foo")));

    // Taken by another session: a failed save still renames nothing.
    ModelDocument doc2(named_session("Foo"));
    doc2.set_saver(full_disk);
    SessionModel session2(doc2, peaks, true);
    session2.setSessionsDir(dir.path());
    OK(!session2.saveSession(why));
    OK(doc2.snapshot().settings.name == "Foo");
    OK(!d.exists(QStringLiteral("Foo 2")));

    // A save whose rename landed but whose folder sync did not is a save,
    // said with its caveat; the next save goes to the same file.
    const ModelDocument::Saver real = engine_saver();
    doc2.set_saver([real](const std::string& file, const std::string& bytes, const Snapshot& s,
                          std::string& w, std::string& c) {
        const bool ok = real(file, bytes, s, w, c);
        c = "the folder could not be synced";
        return ok;
    });
    OK(session2.saveSession(why));
    OK(doc2.snapshot().settings.name == "Foo 2");
    OK(session2.sessionFile() == session_file_path(dir.path(), QStringLiteral("Foo 2")));
    OK(!session2.dirty());
    session2.selectTrack(0);
    OK(session2.controller().set_gain(0, -1.0f, why));
    OK(session2.saveSession(why));
    OK(session2.sessionFile() == session_file_path(dir.path(), QStringLiteral("Foo 2")));
    OK(!d.exists(QStringLiteral("Foo 3")));

    // No engine to save to: refused, by name.
    ModelDocument alone(named_session("Alone"));
    SessionModel alone_session(alone, peaks, true);
    alone_session.setSessionsDir(dir.path());
    OK(!alone_session.saveSession(why) && contains(why, "no engine"));
    OK(!d.exists(QStringLiteral("Alone")));

    // The name limit is bytes of the UTF-8 file name, not characters: 120
    // CJK characters are 360 bytes, past what a file name can hold.
    const QString cjk120(120, QChar(0x97F3));  // 音
    const std::string long_name = session_name_problem(cjk120);
    OK(contains(long_name, "bytes"));
    OK(session_name_problem(QString(80, QChar(0x97F3))).empty());
    OK(!session_name_problem(QString(81, QChar(0x97F3))).empty());
    OK(session_name_problem(QString(241, QLatin1Char('a'))).empty());
    OK(!session_name_problem(QString(242, QLatin1Char('a'))).empty());
    ModelDocument wide(named_session(""));
    {
        SessionSettings s = wide.snapshot().settings;
        s.name = cjk120.toStdString();
        OK(wide.submit(SetSettings{s}, why));
    }
    SessionModel wide_session(wide, peaks, true);
    wide_session.setSessionsDir(dir.path());
    OK(!wide_session.saveSession(why) && contains(why, "bytes"));
    // The longest name the rule allows saves, temporary file and all.
    for (const QString& longest : {QString(80, QChar(0x97F3)), QString(241, QLatin1Char('a'))}) {
        ModelDocument ok_doc(named_session(""));
        ok_doc.set_saver(engine_saver());
        SessionSettings s = ok_doc.snapshot().settings;
        s.name = longest.toStdString();
        OK(ok_doc.submit(SetSettings{s}, why));
        SessionModel ok_session(ok_doc, peaks, true);
        ok_session.setSessionsDir(dir.path());
        OK(ok_session.saveSession(why));
        OK(QFileInfo(session_file_path(dir.path(), longest)).isFile());
    }
    // A name the rule allows that "<name> 2" would push past it is refused,
    // not written under a name the folder cannot hold.
    {
        ModelDocument dup(named_session(""));
        dup.set_saver(engine_saver());
        SessionSettings s = dup.snapshot().settings;
        s.name = std::string(241, 'a');
        OK(dup.submit(SetSettings{s}, why));
        SessionModel dup_session(dup, peaks, true);
        dup_session.setSessionsDir(dir.path());
        OK(!dup_session.saveSession(why) && contains(why, "bytes"));
        OK(dup.snapshot().settings.name == std::string(241, 'a'));
    }
    return 0;
}

}  // namespace

// ADR-0015 §3, §6 and O2, O4: File > Save writes each MIDI sequence to
// "MIDI Sequences/<sha>.msq" before the session file, Open reads them back,
// a file no kept session file names is deleted, nothing is while a kept
// file cannot be read, and a missing one opens silent and is named again.
int test_midi_sequence_files() {
    QTemporaryDir dir;
    CHECK(dir.isValid());
    FlatPeaks peaks;
    std::string why;

    Snapshot base = make_empty_session(1, kRate);
    base.settings.name = "Bass";
    sonora::model::History h(base);
    sonora::model::AddTrack t;
    t.track = h.current().next_id;
    t.playlist = t.track + 1;
    t.name = "Synth";
    t.playlist_name = "Synth.01";
    t.kind = TrackKind::Midi;
    t.channels = 0;
    CHECK(h.apply(t, why));
    MidiLists first;
    for (int i = 0; i < 20; ++i) first.events.push_back(MidiEvent{i * 1000, 0xB0, 1, 2});
    MidiRegion r;
    r.id = h.current().next_id;
    r.name = "Synth_01";
    r.length = 4 * kTicksPerQuarter;
    r.data = std::make_shared<const MidiSequence>(first);
    CHECK(h.apply(PlaceMidiRegion{t.playlist, r, 0, {}}, why));

    ModelDocument doc(h.current());
    doc.set_saver(engine_saver());
    SessionModel session(doc, peaks, true);
    session.setSessionsDir(dir.path());
    OK(session.saveSession(why));
    const QString file = session.sessionFile();
    const QDir seqs(QDir(QFileInfo(file).absolutePath()).filePath(QStringLiteral("MIDI Sequences")));
    const SequenceFileRef a = sequence_ref(MidiSequence(first));
    const QString a_file = seqs.filePath(QString::fromStdString(a.sha256) + QStringLiteral(".msq"));
    OK(file_bytes(a_file).toStdString() == encode_lists(first));
    OK(seqs.entryList(QDir::Files).size() == 1);   // no ".tmp" left behind
    OK(contains(file_bytes(file).toStdString(), a.sha256.c_str()));

    // An edit makes another sequence; once saved, nothing names the first.
    EditMidi e;
    e.playlist = t.playlist;
    e.region = r.id;
    e.add_events = {MidiEvent{30000, 0xB0, 1, 3}};
    OK(doc.submit(e, why));
    OK(session.saveSession(why));
    const auto& now = *doc.snapshot().edit->tracks[1]->midi_playlists[0]->regions[0].data;
    const SequenceFileRef b = sequence_ref(now);
    const QString b_file = seqs.filePath(QString::fromStdString(b.sha256) + QStringLiteral(".msq"));
    // The first file, now a rotated backup (session-format.md, "Saving" step
    // 5), still names the first sequence, so cleanup keeps it.
    OK(QFileInfo::exists(b_file) && QFileInfo::exists(a_file));

    // Opened elsewhere: the same notes, from the file.
    ModelDocument other(make_empty_session(1, kRate));
    other.set_saver(engine_saver());
    SessionModel opener(other, peaks, true);
    opener.setSessionsDir(dir.path());
    OK(opener.openSessionFile(file, true, why));
    OK(same_document(other.snapshot(), doc.snapshot()));
    OK(opener.repairs().isEmpty());

    // A kept file that cannot be read stops every deletion, and says so.
    const QString backups = QDir(QFileInfo(file).absolutePath()).filePath(QStringLiteral("Session File Backups"));
    CHECK(QDir().mkpath(backups));
    {
        QFile junk(QDir(backups).filePath(QStringLiteral("Bass.v9.sonora")));
        CHECK(junk.open(QIODevice::WriteOnly) && junk.write("{not json") > 0);
    }
    QStringList notices;
    QObject::connect(&opener, &SessionModel::notice, [&notices](const QString& n) { notices << n; });
    OK(opener.openSessionFile(file, true, why));
    e.add_events = {MidiEvent{31000, 0xB0, 1, 4}};
    OK(other.submit(e, why));
    OK(opener.saveSession(why));
    OK(QFileInfo::exists(b_file));
    OK(!notices.isEmpty() && notices.back().contains(QStringLiteral("cannot be read")));
    QFile::remove(QDir(backups).filePath(QStringLiteral("Bass.v9.sonora")));

    // An interrupted write is gone once the engine next writes the folder;
    // a missing file opens silent, is listed, and the next save names it
    // again.
    const auto& latest = *other.snapshot().edit->tracks[1]->midi_playlists[0]->regions[0].data;
    const QString c_file =
        seqs.filePath(QString::fromStdString(sequence_ref(latest).sha256) + QStringLiteral(".msq"));
    OK(QFileInfo::exists(c_file));
    {
        QFile tmp(c_file + QStringLiteral(".tmp"));
        CHECK(tmp.open(QIODevice::WriteOnly) && tmp.write("torn") > 0);
    }
    CHECK(QFile::remove(c_file));
    ModelDocument third(make_empty_session(1, kRate));
    third.set_saver(engine_saver());
    SessionModel reopener(third, peaks, true);
    reopener.setSessionsDir(dir.path());
    OK(reopener.openSessionFile(file, true, why));
    OK(reopener.repairs().size() == 1);
    const QVariantMap missing = reopener.repairs()[0].toMap();
    OK(missing.value(QStringLiteral("now")).toString().contains(QStringLiteral("MIDI data missing")));
    OK(missing.value(QStringLiteral("why")).toString().contains(QStringLiteral("not there")));
    const MidiRegion& silent = third.snapshot().edit->tracks[1]->midi_playlists[0]->regions[0];
    OK(silent.data->missing && silent.data->events().empty());
    const QByteArray held = file_bytes(file);
    OK(reopener.saveSession(why));
    OK(file_bytes(file) == held);
    reopener.selectTrack(0);
    OK(reopener.controller().set_gain(0, -2.0f, why));
    OK(reopener.saveSession(why));
    OK(file_bytes(file) != held);
    OK(contains(file_bytes(file).toStdString(), sequence_ref(latest).sha256.c_str()));
    OK(!QFileInfo::exists(c_file + QStringLiteral(".tmp")));
    OK(!QFileInfo::exists(c_file));
    return 0;
}

// A session of one audio track and one MIDI track whose region holds
// `events` controller events, its region id in `region`.
Snapshot midi_session(int events, Id& playlist, Id& region) {
    Snapshot base = make_empty_session(1, kRate);
    base.settings.name = "Keys";
    sonora::model::History h(base);
    sonora::model::AddTrack t;
    t.track = h.current().next_id;
    t.playlist = t.track + 1;
    t.name = "Synth";
    t.playlist_name = "Synth.01";
    t.kind = TrackKind::Midi;
    t.channels = 0;
    std::string why;
    h.apply(t, why);
    MidiLists seq;
    for (int i = 0; i < events; ++i) seq.events.push_back(MidiEvent{i * 1000, 0xB0, 1, 2});
    MidiRegion r;
    r.id = h.current().next_id;
    r.name = "Synth_01";
    r.length = 4 * kTicksPerQuarter;
    r.data = std::make_shared<const MidiSequence>(seq);
    h.apply(PlaceMidiRegion{t.playlist, r, 0, {}}, why);
    playlist = t.playlist;
    region = r.id;
    return h.current();
}

QString msq(const QString& session_file, const MidiSequence& s) {
    return QDir(QFileInfo(session_file).absolutePath())
        .filePath(QStringLiteral("MIDI Sequences/") + QString::fromStdString(sequence_ref(s).sha256) +
                  QStringLiteral(".msq"));
}

const MidiSequence& first_sequence(const TimelineDocument& d) {
    return *d.snapshot().edit->tracks[1]->midi_playlists[0]->regions[0].data;
}

// #109 review, suspected 5: an autosave or a backup names the session
// folder's sequence files, and Open reads them from there, not from
// "Session File Backups/MIDI Sequences/". Cleanup itself, after a durable
// save only and with each kept file parsed once, is the engine's saver's
// and tested with it (common/session_files, engine/control).
int test_midi_sequence_cleanup_follow_ups() {
    QTemporaryDir dir;
    CHECK(dir.isValid());
    FlatPeaks peaks;
    std::string why;
    Id playlist = 0, region = 0;
    ModelDocument doc(midi_session(20, playlist, region));
    doc.set_saver(engine_saver());
    SessionModel session(doc, peaks, true);
    session.setSessionsDir(dir.path());
    OK(session.saveSession(why));
    const QString file = session.sessionFile();
    OK(QFileInfo::exists(msq(file, first_sequence(doc))));

    const QString backups =
        QDir(QFileInfo(file).absolutePath()).filePath(QStringLiteral("Session File Backups"));
    CHECK(QDir().mkpath(backups));
    std::string bytes;
    {
        sonora::model::WriteInfo info{"test", 9};
        std::string error;
        CHECK(serialize(doc.snapshot(), info, bytes, error));
    }
    const QString autosave = QDir(backups).filePath(QStringLiteral("Keys.autosave.sonora"));
    {
        QFile out(autosave);
        CHECK(out.open(QIODevice::WriteOnly) &&
              out.write(bytes.data(), static_cast<qint64>(bytes.size())) ==
                  static_cast<qint64>(bytes.size()));
    }
    OK(session_folder_of(autosave) == QFileInfo(file).absolutePath());
    Loaded loaded;
    std::string read;
    OK(read_session_file(autosave, read, loaded, why));
    OK(loaded.missing_sequences.empty() && loaded.verified_sequences.size() == 1);
    OK(!QFileInfo::exists(QDir(backups).filePath(QStringLiteral("MIDI Sequences"))));

    // A save the engine refuses (here: the folder would not sync is a
    // caution, a refusal is a failure) leaves the window naming what it did.
    doc.set_saver([](const std::string&, const std::string&, const Snapshot&, std::string& w,
                     std::string&) {
        w = "the session names MIDI data the engine does not hold";
        return false;
    });
    EditMidi e;
    e.playlist = playlist;
    e.region = region;
    e.add_events = {MidiEvent{100000, 0xB0, 1, 3}};
    OK(doc.submit(e, why));
    OK(!session.saveSession(why) && contains(why, "does not hold"));
    OK(session.dirty() && session.sessionFile() == file);
    return 0;
}

// Pro Tools' TrackInput (track.toggle_input_monitor): the track's monitor
// mode in the document, Input or Auto, kept with the rest of its mix and
// never an undo step. A MIDI track has no audio input to monitor.
int test_input_monitor() {
    FlatPeaks peaks;
    std::string why;
    Id playlist = 0, region = 0;
    ModelDocument doc(midi_session(1, playlist, region));
    SessionModel session(doc, peaks, false);
    EditController& c = session.controller();
    const Id audio = doc.snapshot().edit->tracks[0]->id;
    const auto mode = [&]() { return doc.snapshot().mix.find(audio)->monitor; };
    const bool could_undo = doc.can_undo();
    OK(session.inputMonitors() == QList<bool>({false, false}));
    OK(c.toggle_input_monitor(0, why) && mode() == MonitorMode::Input);
    OK(session.inputMonitors() == QList<bool>({true, false}));
    OK(c.toggle_input_monitor(0, why) && mode() == MonitorMode::Auto);
    OK(session.inputMonitors() == QList<bool>({false, false}));
    // Off (a file may say so) is not Input, so the button goes to Input.
    TrackMix off = *doc.snapshot().mix.find(audio);
    off.monitor = MonitorMode::Off;
    OK(doc.submit(SetMix{off}, why));
    OK(session.inputMonitors() == QList<bool>({false, false}));
    OK(c.toggle_input_monitor(0, why) && mode() == MonitorMode::Input);
    OK(doc.can_undo() == could_undo);
    const Id midi = doc.snapshot().edit->tracks[1]->id;
    OK(!c.toggle_input_monitor(1, why) && contains(why, "Synth is a MIDI track"));
    OK(doc.snapshot().mix.find(midi)->monitor == MonitorMode::Auto);
    OK(!c.toggle_input_monitor(-1, why) && why == "no track is selected");
    return 0;
}

// daw.model.session_mgmt in the window: Save As goes on in a new folder
// under the new name once the engine says it moved there, the old file as
// it was; a name already taken is refused; Save Copy In writes a copy and
// stays; an engine that wrote the copy and stayed is a held notice and the
// window keeps its file; a newer autosave is offered at open, held.
int test_save_as_and_copy() {
    QTemporaryDir dir;
    CHECK(dir.isValid());
    FlatPeaks peaks;
    std::string why;
    ModelDocument doc(make_empty_session(1, kRate));
    doc.set_saver(engine_saver());
    SessionModel session(doc, peaks, true);
    session.setSessionsDir(dir.path());
    QStringList held, opened;
    QObject::connect(&session, &SessionModel::heldNotice, [&](const QString& t) { held << t; });
    QObject::connect(&session, &SessionModel::openNotice, [&](const QString& t) { opened << t; });
    OK(session.saveSession(why));
    const QString first = session.sessionFile();
    session.setEngineFolder(QFileInfo(first).absolutePath());
    QFile before_file(first);
    CHECK(before_file.open(QIODevice::ReadOnly));
    const QByteArray before = before_file.readAll();
    before_file.close();

    // The engine's part: the same common/session_files Save As, and the
    // SessionFolder event, before the reply, when it moved.
    auto folder = std::make_shared<sonora::session_files::SessionFolder>();
    bool engine_moves = true;
    doc.set_saver_as([&, folder](const std::string& file, uint32_t flags, const std::string& bytes,
                                 const Snapshot& document, std::string& w, std::string& caution) {
        const std::string source = session.engineFolder().toStdString();
        if (!folder->save_as(file, bytes, document, source, (flags & sonora::api::kSaveAsNoAudio) == 0, w,
                             caution)) {
            return false;
        }
        if ((flags & sonora::api::kSaveAsCopy) == 0 && engine_moves) {
            session.setEngineFolder(QFileInfo(QString::fromStdString(file)).absolutePath());
        } else if ((flags & sonora::api::kSaveAsCopy) == 0) {
            caution = "the copy is written, but the session stays where it was";
        }
        return true;
    });

    OK(session.saveSessionAs(QStringLiteral("Mix 2"), false, why));
    const QString moved = QDir(dir.path()).filePath(QStringLiteral("Mix 2/Mix 2.sonora"));
    OK(session.sessionFile() == moved && QFileInfo::exists(moved));
    OK(session.sessionName() == QStringLiteral("Mix 2"));
    OK(!session.dirty());
    QFile after_file(first);
    CHECK(after_file.open(QIODevice::ReadOnly));
    OK(after_file.readAll() == before);

    OK(!session.saveSessionAs(QStringLiteral("Mix 2"), false, why));
    OK(why.find("already") != std::string::npos);
    OK(!session.saveSessionAs(QStringLiteral("a/b"), false, why));

    session.setCopyWithAudio(false);
    OK(session.saveSessionAs(QStringLiteral("Copy"), true, why));
    OK(session.sessionFile() == moved);
    OK(QFileInfo::exists(QDir(dir.path()).filePath(QStringLiteral("Copy/Copy.sonora"))));

    engine_moves = false;
    OK(session.saveSessionAs(QStringLiteral("Stayed"), false, why));
    OK(session.sessionFile() == moved);
    OK(!held.isEmpty() && held.back().contains(QStringLiteral("copy only")));

    // An autosave newer than the file is offered at open, held.
    {
        Snapshot s = doc.snapshot();
        std::string bytes, error;
        CHECK(serialize(s, sonora::model::WriteInfo{"sonora-engine", 99}, bytes, error));
        const QString autosave = autosave_file_of(moved);
        CHECK(QDir().mkpath(QFileInfo(autosave).absolutePath()));
        QFile out(autosave);
        CHECK(out.open(QIODevice::WriteOnly));
        out.write(bytes.data(), static_cast<qint64>(bytes.size()));
    }
    OK(session.openSessionFile(moved, true, why));
    OK(!opened.isEmpty() && opened.back().contains(QStringLiteral("autosave")));
    return 0;
}

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    if (test_model() || test_history() || test_controller() || test_fades() || test_fades_against_crossfades() ||
        test_taper() ||
        test_time() || test_nudge_tempo_map() ||
        test_scene() || test_routing() || test_files() || test_first_save_beside_its_takes() ||
        test_first_save_keeps_the_sessions_name() ||
        test_save_failures() ||
        test_midi_sequence_files() ||
        test_midi_sequence_cleanup_follow_ups() || test_input_monitor() || test_save_as_and_copy()) {
        return 1;
    }
    std::printf("ui edit: %d checks passed\n", checks);
    return 0;
}
