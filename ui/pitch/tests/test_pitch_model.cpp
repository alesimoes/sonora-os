// SPDX-License-Identifier: GPL-3.0-or-later
//
// PitchEditorModel's state: opening and analysing a region, selection, the
// drag and its three snaps, the inspector's ranges, the key snap, Reset, and
// that a new session leaves nothing of the last one.
#include "pitch_test_support.h"

using namespace sonora;
using namespace sonora::ui_pitch;
using namespace pitchtest;

namespace {

PitchEditorModel opened() {
    PitchEditorModel m;
    std::string why;
    m.open(region(), three_notes(), why);
    return m;
}

int test_open_refusals_are_held() {
    PitchEditorModel m;
    std::string why;
    CHECK(!m.open(region(), {}, why));
    CHECK(why.find("no notes") != std::string::npos);
    CHECK(m.notice() == why);
    CHECK(!m.is_open());
    // A note past the region's end.
    CHECK(!m.open(region(), {flat_note(140000, 150000, 0.0)}, why));
    CHECK(why.find("outside Lead 1") != std::string::npos);
    CHECK(!m.notice().empty());
    m.dismiss_notice();
    CHECK(m.notice().empty());
    CHECK(m.open(region(), three_notes(), why));
    CHECK(m.is_open() && m.size() == 3);
    CHECK(m.status() == "3 notes in Lead 1");
    CHECK(!m.edited());
    return 0;
}

int test_analyse_finds_notes_and_refuses_silence() {
    PitchEditorModel m;
    std::string why;
    pitch::YinTracker yin;
    PitchRegion r = region();
    r.length = 48000;
    const auto step = [](double t) { return t < 0.5 ? -1200.0 : -900.0; };
    CHECK(m.analyse(r, tone(1.0, kRate, step), yin, why));
    CHECK(m.size() == 2);
    CHECK(std::fabs(m.note(0).median_cents + 1200.0) < 15.0);
    CHECK(std::fabs(m.note(1).median_cents + 900.0) < 15.0);
    // Silence is not pitched material: refused, held, nothing open.
    CHECK(!m.analyse(r, std::vector<float>(48000, 0.0f), yin, why));
    CHECK(!why.empty());
    CHECK(m.notice() == why);
    CHECK(!m.is_open() && m.size() == 0);
    return 0;
}

int test_select() {
    PitchEditorModel m = opened();
    CHECK(m.selected() == -1);
    CHECK(m.select(1) && m.selected() == 1);
    CHECK(!m.select(3) && m.selected() == 1);
    CHECK(m.select(-1) && m.selected() == -1);
    return 0;
}

int test_drag_snaps() {
    PitchEditorModel m = opened();
    std::string why;
    // Semitone, the default: 130 cents up lands one semitone up.
    CHECK(m.snap_mode() == SnapMode::Semitone);
    CHECK(m.begin_drag(1, why));
    CHECK(m.selected() == 1 && m.dragging());
    CHECK(m.drag_to(130.0, why));
    CHECK(m.edit(1).shift_cents == 100.0);
    CHECK(m.pitch_cents(1) == -800.0);
    // Measured from where the drag began, not from the last step.
    CHECK(m.drag_to(-40.0, why));
    CHECK(m.edit(1).shift_cents == 0.0);
    m.end_drag();
    CHECK(!m.dragging());
    CHECK(!m.drag_to(100.0, why));

    // Off: whole cents.
    m.set_snap_mode(SnapMode::Off);
    CHECK(m.begin_drag(0, why) && m.drag_to(12.4, why));
    CHECK(m.edit(0).shift_cents == 12.0);
    m.end_drag();

    // Scale: C major. C4 (-900) dragged 120 up is C#4, not in the scale; D4 is.
    m.set_snap_mode(SnapMode::Scale);
    CHECK(m.set_key(0, why));
    m.set_scale(pitch::Scale::Major);
    CHECK(m.begin_drag(1, why) && m.drag_to(120.0, why));
    CHECK(std::fabs(m.pitch_cents(1) + 700.0) < 1e-9);
    m.end_drag();
    CHECK(m.edited());
    // A drag past the contract's range stops at its edge.
    m.set_snap_mode(SnapMode::Off);
    CHECK(m.begin_drag(2, why) && m.drag_to(9000.0, why));
    CHECK(m.edit(2).shift_cents == double(api::kPitchCentsMax));
    CHECK(!m.drag_to(std::nan(""), why));
    return 0;
}

int test_drag_refusals() {
    PitchEditorModel closed;
    std::string why;
    CHECK(!closed.begin_drag(0, why));
    CHECK(why.find("open") != std::string::npos);
    PitchEditorModel m = opened();
    CHECK(!m.begin_drag(5, why));
    CHECK(m.notice() == why);
    return 0;
}

int test_inspector() {
    PitchEditorModel m = opened();
    std::string why;
    CHECK(!m.set_straighten(50.0, why));
    CHECK(why == "pitch: select a note first");
    CHECK(m.select(0));
    CHECK(m.set_straighten(75.0, why) && m.edit(0).drift_scale == 0.25);
    CHECK(m.set_vibrato(150.0, why) && m.edit(0).vibrato_scale == 1.5);
    CHECK(m.set_formant(-300, why) && m.edit(0).formant_cents == -300);
    CHECK(m.set_gain(-3.5, why) && m.edit(0).gain_db == -3.5);
    CHECK(!m.set_straighten(101.0, why));
    CHECK(!m.set_vibrato(-1.0, why));
    CHECK(!m.set_formant(2401, why));
    CHECK(!m.set_gain(12.5, why));
    CHECK(!m.set_gain(std::nan(""), why));
    // Refusals left the last accepted values.
    CHECK(m.edit(0).drift_scale == 0.25 && m.edit(0).gain_db == -3.5);
    CHECK(m.edit(1).formant_cents == 0);
    return 0;
}

int test_snap_all_and_reset() {
    PitchEditorModel m;
    std::string why;
    // 30 cents sharp of A3, C4 and E4.
    std::vector<pitch::Note> notes = three_notes();
    for (pitch::Note& n : notes) n.median_cents += 30.0;
    CHECK(m.open(region(), notes, why));
    CHECK(!m.set_key(12, why));
    CHECK(!m.set_amount(120.0, why));
    CHECK(m.set_key(9, why));  // A minor's notes are C major's
    m.set_scale(pitch::Scale::NaturalMinor);
    CHECK(m.set_amount(50.0, why));
    CHECK(m.snap_all(why));
    for (std::size_t i = 0; i < m.size(); ++i) CHECK(std::fabs(m.edit(i).shift_cents + 15.0) < 1e-9);
    CHECK(m.reset_edits(why));
    CHECK(!m.edited());
    return 0;
}

int test_new_session_resets() {
    PitchEditorModel m = opened();
    std::string why;
    m.set_capability({true, ""});
    CHECK(m.select(2) && m.set_gain(-6.0, why));
    CHECK(!m.set_gain(40.0, why));
    CHECK(!m.notice().empty());
    m.reset_session();
    CHECK(!m.is_open() && m.size() == 0);
    CHECK(m.selected() == -1 && !m.dragging() && !m.pending() && !m.stale());
    CHECK(m.notice().empty() && m.status().empty() && m.last_argument().empty());
    // The engine's capability is the connection's, not the session's.
    CHECK(m.capability().available);
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += test_open_refusals_are_held();
    failed += test_analyse_finds_notes_and_refuses_silence();
    failed += test_select();
    failed += test_drag_snaps();
    failed += test_drag_refusals();
    failed += test_inspector();
    failed += test_snap_all_and_reset();
    failed += test_new_session_resets();
    if (failed == 0) std::printf("ui_pitch_model: ok\n");
    return failed == 0 ? 0 : 1;
}
