// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Edit and Mix windows' logic, without a window: the stand-in model's
// operations and undo, mouse gestures and keys turned into operations, the
// zoom and scroll mapping, the fader taper, the time formats and the ruler,
// and which rectangle the timeline draws where. Pure C++ apart from the
// SessionModel QObject in the routing test; nothing is rendered.
#include <QGuiApplication>
#include <cmath>
#include <cstdio>
#include <string>

#include "edit_controller.h"
#include "fader_taper.h"
#include "key_router.h"
#include "peak_source.h"
#include "qt_keys.h"
#include "session_model.h"
#include "synthetic_engine.h"
#include "theme.h"
#include "time_format.h"
#include "timeline_document.h"
#include "timeline_scene.h"

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
// 20-bar source; track 2 is empty.
Snapshot two_tracks() {
    Snapshot s = make_empty_session(2, kRate);
    auto pool = std::make_shared<SourcePool>();
    pool->sources.push_back({100, "Take 1", 1, 20 * kBar});
    s.sources = pool;
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

const Region* region(const Snapshot& s, Id id) { return s.find_region(id); }

// --- the model ---------------------------------------------------------------

int test_model() {
    {
        const Snapshot demo = make_demo_session(18, kRate);
        OK(check_invariants(demo).empty());
        OK(demo.edit->tracks.size() == 18);
        OK(demo.settings.timecode_origin == 3600LL * kRate);  // 01:00:00:00
    }

    LocalDocument doc(two_tracks());
    const Snapshot s0 = doc.snapshot();
    OK(check_invariants(s0).empty());
    const Id pl = playlist_of(s0, 0);
    std::string why;

    // Move: the region moves, and the untouched track is the same object.
    OK(doc.submit(MoveRegion{pl, 10, 8 * kBar, kNoId}, why));
    OK(region(doc.snapshot(), 10)->position == 8 * kBar);
    OK(doc.snapshot().edit->tracks[1] == s0.edit->tracks[1]);
    OK(doc.snapshot().revision == s0.revision + 1);
    OK(check_invariants(doc.snapshot()).empty());

    // Undo is a pointer: the EditState from before, not a reconstruction.
    OK(doc.undo(why));
    OK(doc.snapshot().edit == s0.edit);
    OK(doc.redo(why));
    OK(region(doc.snapshot(), 10)->position == 8 * kBar);
    OK(doc.undo(why));

    // Moving B (1 bar) into the middle of A ([1, 3) bars) splits A; the
    // right half takes the id the operation carries, and without one the
    // move is refused rather than an id invented.
    Snapshot out;
    OK(!apply(doc.snapshot(), MoveRegion{pl, 11, kBar + kBar / 2, kNoId}, out, why));
    OK(!why.empty());
    OK(!doc.submit(MoveRegion{pl, 11, kBar + kBar / 2, 10}, why));  // id in use
    OK(doc.submit(MoveRegion{pl, 11, kBar + kBar / 2, 60}, why));
    OK(region(doc.snapshot(), 10)->end() == kBar + kBar / 2);
    OK(region(doc.snapshot(), 60)->position == 2 * kBar + kBar / 2);
    OK(region(doc.snapshot(), 60)->source_offset == 2 * kBar + kBar / 2);
    OK(region(doc.snapshot(), 60)->end() == 3 * kBar);
    OK(check_invariants(doc.snapshot()).empty());
    OK(doc.undo(why));
    // Over A's tail only, A is trimmed and nothing is split.
    OK(doc.submit(MoveRegion{pl, 11, 2 * kBar + kBar / 2, kNoId}, why));
    OK(region(doc.snapshot(), 10)->end() == 2 * kBar + kBar / 2);
    OK(check_invariants(doc.snapshot()).empty());
    OK(doc.undo(why));
    OK(doc.snapshot().edit == s0.edit);

    // Trim: the offset follows the start; the source bounds hold.
    OK(doc.submit(TrimRegion{pl, 10, Edge::Start, kBar + kBeat}, why));
    OK(region(doc.snapshot(), 10)->source_offset == kBar + kBeat);
    OK(region(doc.snapshot(), 10)->end() == 3 * kBar);
    OK(!doc.submit(TrimRegion{pl, 10, Edge::Start, -1}, why));
    OK(!doc.submit(TrimRegion{pl, 10, Edge::End, 5 * kBar}, why));  // over B
    OK(!doc.submit(TrimRegion{pl, 11, Edge::End, 30 * kBar}, why));  // past the source
    OK(doc.undo(why));

    // Split: the left keeps the id, the right gets the new one.
    OK(!doc.submit(SplitRegion{pl, 10, 2 * kBar, 11}, why));
    OK(!doc.submit(SplitRegion{pl, 10, 5 * kBar, 50}, why));  // outside
    OK(doc.submit(SplitRegion{pl, 10, 2 * kBar, 50}, why));
    OK(region(doc.snapshot(), 10)->end() == 2 * kBar);
    OK(region(doc.snapshot(), 50)->position == 2 * kBar);
    OK(region(doc.snapshot(), 50)->source_offset == 2 * kBar);
    OK(check_invariants(doc.snapshot()).empty());

    // Remove, then undo twice is back to the start.
    OK(doc.submit(RemoveRegion{pl, 50}, why));
    OK(region(doc.snapshot(), 50) == nullptr);
    OK(doc.undo(why) && doc.undo(why));
    OK(doc.snapshot().edit == s0.edit);
    OK(!doc.undo(why));

    // A new edit after an undo drops the redo branch.
    OK(doc.submit(RemoveRegion{pl, 11}, why));
    OK(doc.undo(why));
    OK(doc.can_redo());
    OK(doc.submit(RemoveRegion{pl, 10}, why));
    OK(!doc.can_redo());
    OK(doc.undo(why));

    // Mix is not an edit: undo does not move a fader.
    const Id t1 = s0.edit->tracks[0]->id;
    OK(doc.submit(SetMix{t1, MixField::GainDb, -6.0f}, why));
    OK(doc.submit(SetMix{t1, MixField::GainDb, 40.0f}, why));
    OK(doc.snapshot().mix.find(t1)->gain_db == kMaxGainDb);  // clamped to +12
    OK(!doc.can_undo());
    OK(!doc.submit(SetMix{999, MixField::Mute, 1}, why));
    OK(!doc.submit(SetMix{t1, MixField::Pan, NAN}, why));
    return 0;
}

// --- gestures and commands -----------------------------------------------------

int test_controller() {
    LocalDocument doc(two_tracks());
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
    // ...and dragging it past B stops at B, as the resolver clamps.
    const double a_end2 = c.view().x_of(3 * kBar - kRate) - 2;
    OK(c.press(a_end2, lower, why));
    c.move(a_end2 + 400, lower);
    OK(c.release(a_end2 + 400, lower, why));
    OK(region(doc.snapshot(), 10)->end() == 4 * kBar);
    OK(c.undo(why) && c.undo(why));

    // --- upper half: the insertion point, then separate there ---
    OK(c.press(a_mid, upper, why) && c.release(a_mid, upper, why));
    OK(c.selection().region == kNoId);
    OK(c.selection().insertion == c.view().frame_at(a_mid));
    OK(c.separate(why));
    OK(doc.snapshot().edit->tracks[0]->active()->regions.size() == 3);
    OK(c.undo(why));
    OK(doc.snapshot().edit->tracks[0]->active()->regions.size() == 2);

    // A range separates at both ends.
    OK(c.press(c.view().x_of(kBar + kBeat), upper, why));
    c.move(c.view().x_of(2 * kBar), upper);
    OK(c.release(c.view().x_of(2 * kBar), upper, why));
    OK(c.selection().has_range());
    OK(c.separate(why));
    OK(doc.snapshot().edit->tracks[0]->active()->regions.size() == 4);
    OK(c.undo(why) && c.undo(why));

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
    df.fps = "29.97df";
    df.timecode_origin = 0;
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
    return 0;
}

// --- what is drawn where --------------------------------------------------------

int test_scene() {
    LocalDocument doc(two_tracks());
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
    LocalDocument doc(two_tracks());
    FlatPeaks peaks;
    SessionModel session(doc, peaks, false);
    std::vector<std::string> sent;
    KeyRouter router(map, SyntheticEngine::engine_commands(), [&sent](const std::string& c, const CommandContext& ctx) {
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
    LocalDocument doc(two_tracks());
    FlatPeaks peaks;
    SessionModel session(doc, peaks, false);
    std::vector<std::string> sent;
    KeyRouter router(map, SyntheticEngine::engine_commands(), [&sent](const std::string& c, const CommandContext& ctx) {
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
    OK(doc.snapshot().edit->tracks[0]->active()->regions.size() == 3);
    key("ctrl+z");
    OK(doc.snapshot().edit->tracks[0]->active()->regions.size() == 2);
    key("ctrl+shift+z");
    OK(doc.snapshot().edit->tracks[0]->active()->regions.size() == 3);

    // Shift+S solos the selected track (track 1 by default), Shift+M mutes.
    key("shift+s");
    OK(session.solos().at(0) && !session.solos().at(1));
    key("semicolon");  // select the next track down
    key("shift+m");
    OK(session.mutes().at(1));
    // The target-less arm key now arms the selection, via the engine.
    key("shift+r");
    OK(!sent.empty() && sent.back() == "track.toggle_arm@1");

    // A refusal is said, with the reason.
    const RouteResult r = key("ctrl+b");  // nothing selected to clear
    OK(r.result == DispatchResult::Failed);
    OK(r.message.find("nothing is selected") != std::string::npos);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    if (test_model() || test_controller() || test_taper() || test_time() || test_scene() ||
        test_routing()) {
        return 1;
    }
    std::printf("ui edit: %d checks passed\n", checks);
    return 0;
}
