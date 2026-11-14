// SPDX-License-Identifier: GPL-3.0-or-later
//
// The picture: the viewport the notes fit in, blob geometry before and after
// an edit, hit_test, the piano ruler and the note names.
#include "sonora/ui_pitch/pitch_view.h"

#include "pitch_test_support.h"

using namespace sonora;
using namespace sonora::ui_pitch;
using namespace pitchtest;

namespace {

bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }

PitchEditorModel opened() {
    PitchEditorModel m;
    std::string why;
    m.open(region(), three_notes(), why);
    return m;
}

int test_viewport() {
    const PitchEditorModel m = opened();
    const PitchViewport v = fit_viewport(m, 780.0, 400.0);
    // A3..E4 with three semitones either side, on row boundaries.
    CHECK(near(v.cents_low, -1550.0) && near(v.cents_high, -150.0));
    CHECK(v.frame_from == 0 && v.frame_to == 144000);
    CHECK(near(x_of_frame(v, 72000.0), 390.0));
    CHECK(near(y_of_cents(v, -150.0), 0.0) && near(y_of_cents(v, -1550.0), 400.0));
    CHECK(near(cents_of_dy(v, -40.0), 140.0));
    // One note still gets an octave.
    PitchEditorModel one;
    std::string why;
    CHECK(one.open(region(), {flat_note(0, 4800, 0.0)}, why));
    const PitchViewport w = fit_viewport(one, 780.0, 400.0);
    CHECK(w.cents_high - w.cents_low >= 1200.0);
    return 0;
}

int test_blobs() {
    PitchEditorModel m = opened();
    PitchViewport v = fit_viewport(m, 780.0, 400.0);
    std::vector<NoteBlob> blobs = note_blobs(m, v);
    CHECK(blobs.size() == 3);
    const double row = 400.0 * 100.0 / 1400.0;
    CHECK(near(blobs[0].x, 130.0) && near(blobs[0].w, 130.0));
    CHECK(near(blobs[0].h, 0.8 * row));
    CHECK(near(blobs[0].y + blobs[0].h / 2.0, 300.0));
    CHECK(!blobs[0].moved && !blobs[0].selected);
    CHECK(blobs[0].label == "A3 0");
    // 100 contour frames drawn as 50 points, flat on the note.
    CHECK(blobs[0].contour.size() == 100);
    CHECK(near(blobs[0].contour[1], 300.0));

    std::string why;
    CHECK(m.begin_drag(0, why) && m.drag_to(100.0, why));
    m.end_drag();
    blobs = note_blobs(m, v);
    CHECK(blobs[0].moved && blobs[0].selected);
    CHECK(near(blobs[0].origin_y + blobs[0].h / 2.0, 300.0));
    CHECK(near(blobs[0].y + blobs[0].h / 2.0, 300.0 - row));
    CHECK(near(blobs[0].contour[1], 300.0 - row));
    CHECK(blobs[0].label == "A#3 0");
    // Vibrato removed: a contour swinging +-20 cents is drawn flat.
    pitch::Note wobbly = flat_note(0, 24000, 0.0);
    for (std::size_t k = 0; k < wobbly.contour_cents.size(); ++k) {
        wobbly.contour_cents[k] = float(20.0 * std::sin(double(k)));
    }
    PitchEditorModel w;
    CHECK(w.open(region(), {wobbly}, why) && w.select(0) && w.set_vibrato(0.0, why));
    v = fit_viewport(w, 780.0, 400.0);
    blobs = note_blobs(w, v);
    double lo = 1e9, hi = -1e9;
    for (std::size_t k = 1; k < blobs[0].contour.size(); k += 2) {
        lo = std::min(lo, blobs[0].contour[k]);
        hi = std::max(hi, blobs[0].contour[k]);
    }
    CHECK(hi - lo < 1.0);
    return 0;
}

int test_hit_test() {
    const PitchEditorModel m = opened();
    const PitchViewport v = fit_viewport(m, 780.0, 400.0);
    const std::vector<NoteBlob> blobs = note_blobs(m, v);
    CHECK(hit_test(blobs, 195.0, 300.0) == 0);
    CHECK(hit_test(blobs, 195.0, 310.0) == 0);
    CHECK(hit_test(blobs, 195.0, 330.0) == -1);  // the row below
    CHECK(hit_test(blobs, 100.0, 300.0) == -1);  // before the note
    CHECK(hit_test(blobs, 600.0, 10.0) == -1);
    const NoteBlob& c4 = blobs[1];
    CHECK(hit_test(blobs, c4.x + 1.0, c4.y + c4.h / 2.0) == 1);
    // A blob narrower than a pointer is still hit within kMinHitPixels.
    NoteBlob tiny;
    tiny.index = 4;
    tiny.x = 50.0;
    tiny.w = 2.0;
    tiny.y = 50.0;
    tiny.h = 2.0;
    CHECK(hit_test({tiny}, 55.0, 51.0) == 4);
    CHECK(hit_test({tiny}, 57.0, 51.0) == -1);
    // Two in reach: the nearer centre wins.
    NoteBlob next = tiny;
    next.index = 5;
    next.x = 56.0;
    CHECK(hit_test({tiny, next}, 56.0, 51.0) == 5);
    CHECK(hit_test({tiny, next}, 52.0, 51.0) == 4);
    return 0;
}

int test_ruler_and_names() {
    const PitchEditorModel m = opened();
    const PitchViewport v = fit_viewport(m, 780.0, 400.0);
    const std::vector<RulerRow> rows = ruler_rows(v);
    CHECK(rows.size() == 14);
    CHECK(near(rows.front().y, 0.0) && near(rows.back().y + rows.back().h, 400.0));
    int named = 0;
    for (const RulerRow& r : rows) {
        if (!r.name.empty()) {
            ++named;
            CHECK(r.name == "C4" && !r.black);
        }
    }
    CHECK(named == 1);
    CHECK(!rows[0].black);   // G4, the top row
    CHECK(rows[1].black);    // F#4
    CHECK(note_name(0.0) == "A4");
    CHECK(note_name(-900.0) == "C4");
    CHECK(note_name(-2000.0) == "C#3");
    CHECK(note_name(300.0) == "C5");
    CHECK(cents_off_label(-888.0) == "+12");
    CHECK(cents_off_label(-905.0) == "-5");
    CHECK(cents_off_label(-900.0) == "0");
    CHECK(key_name(0) == "C" && key_name(11) == "B" && key_name(12) == "?");
    CHECK(scale_name(pitch::Scale::HarmonicMinor) == "Harmonic minor");
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += test_viewport();
    failed += test_blobs();
    failed += test_hit_test();
    failed += test_ruler_and_names();
    if (failed == 0) std::printf("ui_pitch_view: ok\n");
    return failed == 0 ? 0 : 1;
}
