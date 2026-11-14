// SPDX-License-Identifier: GPL-3.0-or-later
//
// Knob.qml from the keyboard: each press is one begin/moved/end, the step
// follows the modifier, Home resets, the ends clamp without opening an undo
// step, and the knob is a named Slider to assistive technology.
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickView>
#include <QTest>
#include <cmath>
#include <cstdio>

#include "register_types.h"

using namespace sonora::ui;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

class Log : public QObject {
    Q_OBJECT
public:
    int begins = 0, ends = 0, resets = 0;
    double last = -1;
    int moves = 0;
    QObject* knob = nullptr;
public slots:
    void onBegin() { ++begins; }
    void onEnd() { ++ends; }
    void onReset() { ++resets; }
    // The model accepts what it is told, as the panel does.
    void onMoved(qreal p) {
        last = p;
        ++moves;
        knob->setProperty("position", p);
    }
};

int knob_keys() {
    QQuickView view;
    view.setInitialProperties({{QStringLiteral("position"), 0.5},
                               {QStringLiteral("accessibleName"), QStringLiteral("Ratio")}});
    view.setSource(QUrl(QStringLiteral("qrc:/sonora/qml/Knob.qml")));
    for (const auto& e : view.errors()) std::fprintf(stderr, "%s\n", qPrintable(e.toString()));
    CHECK(view.status() == QQuickView::Ready);
    view.show();
    CHECK(QTest::qWaitForWindowExposed(&view));
    QQuickItem* knob = view.rootObject();
    CHECK(knob != nullptr && knob->activeFocusOnTab());

    Log log;
    log.knob = knob;
    QObject::connect(knob, SIGNAL(begin()), &log, SLOT(onBegin()));
    QObject::connect(knob, SIGNAL(end()), &log, SLOT(onEnd()));
    QObject::connect(knob, SIGNAL(reset()), &log, SLOT(onReset()));
    QObject::connect(knob, SIGNAL(moved(qreal)), &log, SLOT(onMoved(qreal)));

    knob->forceActiveFocus();
    CHECK(knob->hasActiveFocus());
    const auto near = [](double a, double b) { return std::fabs(a - b) < 1e-9; };

    QTest::keyClick(&view, Qt::Key_Up);
    CHECK(log.begins == 1 && log.moves == 1 && log.ends == 1 && near(log.last, 0.51));
    QTest::keyClick(&view, Qt::Key_Left);
    CHECK(log.begins == 2 && near(log.last, 0.50));
    QTest::keyClick(&view, Qt::Key_Right, Qt::ShiftModifier);
    CHECK(near(log.last, 0.60));
    QTest::keyClick(&view, Qt::Key_Down, Qt::ControlModifier);
    CHECK(near(log.last, 0.599));
    QTest::keyClick(&view, Qt::Key_PageDown);
    CHECK(near(log.last, 0.499));
    QTest::keyClick(&view, Qt::Key_PageUp);
    CHECK(near(log.last, 0.599));
    CHECK(log.begins == 6 && log.ends == 6 && log.moves == 6);

    QTest::keyClick(&view, Qt::Key_Home);
    CHECK(log.resets == 1 && log.begins == 6);

    knob->setProperty("position", 0.995);
    QTest::keyClick(&view, Qt::Key_PageUp);
    CHECK(near(log.last, 1.0));
    const int before = log.begins;
    QTest::keyClick(&view, Qt::Key_Up);
    CHECK(log.begins == before && near(knob->property("position").toDouble(), 1.0));

    CHECK(knob->property("accessibleName").toString() == QStringLiteral("Ratio"));
    CHECK(knob->property("accessibleValue").toString() == QStringLiteral("100 %"));
    QQuickItem* ring = knob->findChild<QQuickItem*>(QStringLiteral("knobFocusRing"));
    CHECK(ring != nullptr && ring->isVisible());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    register_types();
    if (knob_keys()) return 1;
    std::printf("ui knob keys: ok\n");
    return 0;
}

#include "test_knob_keys.moc"
