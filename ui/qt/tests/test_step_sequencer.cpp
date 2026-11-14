// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Step Sequencer window's model and QML (logic.step_sequencer):
//   - a click toggles a step, selects it and publishes the pattern;
//   - the lane edits the selected step only, and clamps like the pattern;
//   - length and swing clamp, and a shorter pattern drops a selection past
//     its end rather than editing a step that is no longer there;
//   - row mute and solo are the pattern's, and a row with no source says so;
//   - the QML grid shows one cell per lane and step and a click reaches the
//     model.
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickView>
#include <cstdio>

#include "register_types.h"
#include "step_sequencer_model.h"

using namespace sonora;
using namespace sonora::ui;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                            \
        }                                                                        \
    } while (0)

namespace {

// Repeater delegates are visual children only, which findChild cannot see.
QQuickItem* find(QQuickItem* from, const QString& name) {
    if (from->objectName() == name) return from;
    for (QQuickItem* c : from->childItems())
        if (QQuickItem* f = find(c, name)) return f;
    return nullptr;
}

int toggling() {
    int published = 0;
    StepSequencerModel m{[&published](const stepseq::Pattern&) { ++published; }};
    CHECK(!m.hasSelection());
    m.toggleStep(2, 5);
    CHECK(m.stepOn(2, 5));
    CHECK(m.selectedRow() == 2 && m.selectedStep() == 5);
    CHECK(published == 1);
    m.toggleStep(2, 5);
    CHECK(!m.stepOn(2, 5));
    m.toggleStep(99, 0);  // outside the grid: refused, not wrapped
    m.toggleStep(0, 99);
    CHECK(published == 2);
    return 0;
}

int lane() {
    StepSequencerModel m;
    CHECK(m.velocity() == 0);  // nothing selected: nothing to edit
    m.setVelocity(50);
    CHECK(m.velocity() == 0);
    m.toggleStep(1, 3);
    m.setVelocity(300);
    CHECK(m.velocity() == 127);
    m.setProbability(-5);
    CHECK(m.probability() == 0);
    m.setPitch(40);
    CHECK(m.pitch() == 24);
    m.setPitch(-3);
    CHECK(m.pitch() == -3);
    m.select(1, 4);  // a neighbour is untouched
    CHECK(m.velocity() == 100 && m.pitch() == 0);
    return 0;
}

int sizes() {
    StepSequencerModel m;
    m.setLength(1000);
    CHECK(m.length() == stepseq::kMaxSteps);
    m.setLength(0);
    CHECK(m.length() == 1);
    m.setSwing(10);
    CHECK(m.swing() == stepseq::kMinSwingPercent);
    m.setSwing(90);
    CHECK(m.swing() == stepseq::kMaxSwingPercent);
    m.setLength(16);
    m.toggleStep(0, 12);
    m.setLength(8);
    CHECK(!m.hasSelection());
    return 0;
}

int rows() {
    StepSequencerModel m;
    CHECK(!m.rowHasSource(0));
    m.setRowSource(0, 7);
    CHECK(m.rowHasSource(0) && !m.rowHasSource(1));
    m.toggleMute(0);
    m.toggleSolo(1);
    CHECK(m.rowMuted(0) && m.rowSoloed(1));
    CHECK(m.pattern().any_solo());
    m.toggleSolo(1);
    CHECK(!m.pattern().any_solo());
    return 0;
}

int qml() {
    StepSequencerModel m;
    QQuickView view;
    view.setInitialProperties({{QStringLiteral("seq"), QVariant::fromValue(&m)}});
    view.setSource(QUrl(QStringLiteral("qrc:/sonora/qml/StepSequencer.qml")));
    for (const auto& e : view.errors()) std::fprintf(stderr, "%s\n", qPrintable(e.toString()));
    CHECK(view.status() == QQuickView::Ready);
    view.show();
    QQuickItem* root = view.rootObject();
    QQuickItem* c = find(root, "cell3_7");
    CHECK(c != nullptr);
    CHECK(find(root, "cell7_15") != nullptr);
    CHECK(find(root, "cell8_0") == nullptr);  // 8 lanes
    m.toggleStep(3, 7);
    CHECK(c->property("on").toBool());
    m.toggleMute(2);
    CHECK(find(root, "mute2")->property("active").toBool());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    register_types();
    if (toggling() != 0) return 1;
    if (lane() != 0) return 1;
    if (sizes() != 0) return 1;
    if (rows() != 0) return 1;
    if (qml() != 0) return 1;
    std::printf("ui step sequencer: all checks passed\n");
    return 0;
}
