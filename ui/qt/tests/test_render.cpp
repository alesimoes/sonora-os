// SPDX-License-Identifier: GPL-3.0-or-later
//
// A meter rendered for real: a QQuickWindow on xcb under Xvfb, drawn by the
// scene graph's OpenGL path (llvmpipe here), read back with grabWindow(). The
// other UI tests never render, so without this nothing runs
// MeterItem::updatePaintNode(). The pixels checked are the meter_model
// colours; what a meter shows is test_meter_model's job, and this only proves
// that it reaches the screen.
#include <QGuiApplication>
#include <QImage>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTest>
#include <cstdio>
#include <cstdlib>

#include "meter_item.h"

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

// meter_model.cpp's colours.
constexpr QRgb kNoDataBackground = 0xff2a2d31;
constexpr QRgb kGreen = 0xff3fbf7f;
constexpr QRgb kOverOn = 0xffff2d2d;
constexpr QRgb kWindow = 0xff0000ff;

bool is(const QImage& img, int x, int y, QRgb want) {
    const QRgb got = img.pixel(x, y);
    const bool ok = std::abs(qRed(got) - qRed(want)) <= 2 &&
                    std::abs(qGreen(got) - qGreen(want)) <= 2 &&
                    std::abs(qBlue(got) - qBlue(want)) <= 2;
    if (!ok) std::fprintf(stderr, "  pixel (%d,%d) is %08x, want %08x\n", x, y, got, want);
    return ok;
}

// Counts the scene graph's calls, so a test can tell "redrawn" from "the
// old frame happens to look right".
class CountingMeter : public MeterItem {
public:
    using MeterItem::MeterItem;
    int paints = 0;

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData* d) override {
        ++paints;
        return MeterItem::updatePaintNode(old, d);
    }
};

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);

    QQuickWindow window;
    window.setColor(QColor::fromRgb(kWindow));
    window.resize(100, 300);
    auto* meter = new CountingMeter(window.contentItem());
    meter->setPosition(QPointF(0, 0));
    meter->setSize(QSizeF(40, 100));
    window.show();
    CHECK(QTest::qWaitForWindowExposed(&window));
    // The software adaptation never builds the meter's geometry node, by
    // design; a pass there would prove nothing.
    CHECK(window.rendererInterface()->graphicsApi() != QSGRendererInterface::Software);
    std::printf("ui render: graphics API %d, platform %s\n",
                static_cast<int>(window.rendererInterface()->graphicsApi()),
                qPrintable(QGuiApplication::platformName()));

    // Full scale: the over band lit at the top, green at the bottom of the
    // bar, and nothing drawn outside the item.
    meter->feed(1.0f, 0.0f);
    QImage img = window.grabWindow();
    CHECK(!img.isNull());
    CHECK(img.width() == 100 && img.height() == 300);
    CHECK(is(img, 5, 1, kOverOn));
    CHECK(is(img, 5, 97, kGreen));
    CHECK(is(img, 5, 150, kWindow));
    CHECK(is(img, 70, 50, kWindow));

    // The engine goes away: the no-data face, and the over still latched.
    meter->setNoData();
    img = window.grabWindow();
    CHECK(is(img, 5, 50, kNoDataBackground));
    CHECK(is(img, 5, 1, kOverOn));

    // Resized while the engine is dead, so no feed() asks for a redraw: the
    // meter must still be drawn at its new size.
    const int paints = meter->paints;
    meter->setSize(QSizeF(40, 200));
    img = window.grabWindow();
    std::printf("ui render: updatePaintNode calls %d before the resize, %d after\n", paints,
                meter->paints);
    CHECK(meter->paints > paints);
    CHECK(is(img, 5, 150, kNoDataBackground));
    CHECK(is(img, 5, 250, kWindow));

    std::printf("ui render: all checks passed\n");
    return 0;
}
