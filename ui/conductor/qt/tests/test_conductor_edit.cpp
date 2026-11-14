// SPDX-License-Identifier: GPL-3.0-or-later
//
// Editing on the Tempo and Meter rulers (time.tempo_map, ADR-0140): what the
// ruler's gestures do through EditController. A drag is one Undo Tempo step,
// a double-click on empty ruler adds a change at the insertion point, the
// first tempo cannot be removed and says why, a meter lands on the bar line,
// and nothing moves while a take is open.
#include <cstdio>
#include <string>
#include <vector>

#include "conductor_ruler_edit.h"
#include "edit_controller.h"
#include "timeline_document.h"

using namespace sonora;
using namespace sonora::ui;
using namespace sonora::ui::model;
namespace ce = sonora::ui::conductor_edit;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            return 1;                                                                    \
        }                                                                                \
    } while (0)

namespace {

constexpr uint32_t kRate = 48000;
// One bar of 4/4 at 120 bpm, the demo session's tempo.
constexpr Frames kBar = 96000;

const std::vector<TempoEntry>& tempo(const ModelDocument& d) { return d.snapshot().settings.tempo; }

bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// Ruler pixels of a frame at the controller's zoom, scrolled to the start.
double px(const EditController& c, Frames f) { return c.view().x_of(f); }

int test_add_and_hit() {
    ModelDocument doc(make_demo_session(2, kRate));
    EditController c(doc);
    std::string why;
    CHECK(tempo(doc).size() == 1);
    c.set_insertion(4 * kBar);
    CHECK(ce::tempo_text(c, -1) == "120.00");
    CHECK(ce::type_tempo(c, false, -1, "96", why));
    CHECK(tempo(doc).size() == 2 && tempo(doc)[1].at == 4 * kBar && tempo(doc)[1].bpm == 96.0f);

    const double x = px(c, 4 * kBar);
    CHECK(ce::mark_at(c, x) == 1);
    CHECK(ce::mark_at(c, x + 5) == 1);
    CHECK(ce::mark_at(c, x + 9) == -1);
    CHECK(ce::mark_at(c, 0) == 0);
    CHECK(ce::tempo_text(c, 1) == "96.00");
    // Typed at a mark: that mark changes, nothing is added.
    CHECK(ce::type_tempo(c, false, 1, "100 bpm", why));
    CHECK(tempo(doc).size() == 2 && tempo(doc)[1].bpm == 100.0f);
    return 0;
}

int test_drag_one_undo_step() {
    ModelDocument doc(make_demo_session(2, kRate));
    EditController c(doc);
    std::string why;
    CHECK(c.set_tempo(4 * kBar, 96.0f, why));
    const std::vector<TempoEntry> before = tempo(doc);

    // 100 px up is 10 bpm; the label is what the tooltip shows mid-drag,
    // and nothing is sent until the button comes up.
    CHECK(ce::drag_label(c, 1, 100.0, false) == "106.00 bpm");
    CHECK(ce::drag_label(c, 1, 37.0, false) == "99.70 bpm");
    CHECK(ce::drag_label(c, 1, 37.0, true) == "100.00 bpm");
    CHECK(ce::drag_label(c, 7, 10.0, false).empty());
    CHECK(tempo(doc) == before);

    CHECK(ce::drag_end(c, false, 1, 100.0, false, why));
    CHECK(tempo(doc).size() == 2 && tempo(doc)[1].bpm == 106.0f && tempo(doc)[1].at == 4 * kBar);
    // A drag past the format's bound stops at it.
    CHECK(ce::drag_label(c, 0, -1e6, false) == "1.00 bpm");
    CHECK(ce::drag_label(c, 0, 1e6, false) == "1000.00 bpm");
    CHECK(ce::drag_end(c, false, 0, -200.0, false, why));
    CHECK(tempo(doc)[0].bpm == 100.0f);

    // Undo Tempo steps back one drag at a time.
    CHECK(c.tempo_undo(why));
    CHECK(tempo(doc)[0].bpm == 120.0f && tempo(doc)[1].bpm == 106.0f);
    CHECK(c.tempo_undo(why));
    CHECK(tempo(doc) == before);
    return 0;
}

int test_remove() {
    ModelDocument doc(make_demo_session(2, kRate));
    EditController c(doc);
    std::string why;
    CHECK(c.set_tempo(4 * kBar, 96.0f, why));
    CHECK(c.set_tempo(8 * kBar, 140.0f, why));

    CHECK(!ce::remove(c, false, 0, why));
    CHECK(has(why, "first tempo cannot be removed"));
    CHECK(tempo(doc).size() == 3);
    CHECK(!ce::remove(c, false, 9, why));
    CHECK(has(why, "no tempo mark"));

    // The mark asked for goes, not the one nearest the insertion point.
    c.set_insertion(4 * kBar);
    CHECK(ce::remove(c, false, 2, why));
    CHECK(tempo(doc).size() == 2 && tempo(doc)[1].at == 4 * kBar);
    CHECK(c.tempo_undo(why));
    CHECK(tempo(doc).size() == 3);
    return 0;
}

int test_meter() {
    ModelDocument doc(make_demo_session(2, kRate));
    EditController c(doc);
    std::string why;
    // Double-clicked a little after bar 9's line: the meter starts on it.
    const double x = px(c, 8 * kBar + 3000);
    CHECK(ce::meter_text(c, x) == "4/4");
    CHECK(ce::set_meter(c, false, x, 7, 8, why));
    bool found = false;
    for (const TempoEntry& e : tempo(doc)) {
        if (e.at == 8 * kBar) found = e.meter[0] == 7 && e.meter[1] == 8;
    }
    CHECK(found);
    CHECK(c.selection().insertion == 8 * kBar);
    CHECK(ce::meter_text(c, px(c, 8 * kBar) + 4) == "7/8");
    CHECK(c.tempo_undo(why));
    CHECK(ce::meter_text(c, px(c, 8 * kBar) + 4) == "4/4");
    return 0;
}

int test_refusals() {
    ModelDocument doc(make_demo_session(2, kRate));
    EditController c(doc);
    std::string why;
    CHECK(c.set_tempo(4 * kBar, 96.0f, why));
    const std::vector<TempoEntry> before = tempo(doc);

    // A take is open: every edit is refused, and says so.
    CHECK(!ce::drag_end(c, true, 1, 50.0, false, why));
    CHECK(has(why, "take is open"));
    why.clear();
    CHECK(!ce::type_tempo(c, true, -1, "90", why));
    CHECK(has(why, "take is open"));
    why.clear();
    CHECK(!ce::remove(c, true, 1, why));
    CHECK(has(why, "take is open"));
    why.clear();
    CHECK(!ce::set_meter(c, true, 10.0, 3, 4, why));
    CHECK(has(why, "take is open"));
    CHECK(tempo(doc) == before);

    // What is typed must be a tempo the format allows.
    CHECK(!ce::type_tempo(c, false, 1, "fast", why));
    CHECK(has(why, "is not a tempo"));
    CHECK(!ce::type_tempo(c, false, 1, "", why));
    CHECK(!ce::type_tempo(c, false, 1, "5000", why));
    CHECK(has(why, "1 to 1000"));
    CHECK(!ce::drag_end(c, false, 5, 10.0, false, why));
    CHECK(has(why, "no tempo mark"));
    CHECK(tempo(doc) == before);
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += test_add_and_hit();
    failed += test_drag_one_undo_step();
    failed += test_remove();
    failed += test_meter();
    failed += test_refusals();
    if (failed == 0) std::printf("ui_conductor_edit: ok\n");
    return failed == 0 ? 0 : 1;
}
