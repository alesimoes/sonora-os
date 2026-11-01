// SPDX-License-Identifier: GPL-3.0-or-later
#include "meter_item.h"

#include <QMouseEvent>
#include <QQuickWindow>
#include <QSGGeometryNode>
#include <QSGRendererInterface>
#include <QSGVertexColorMaterial>

#include "shell_model.h"

namespace sonora::ui {

namespace {

// Six vertices per rectangle, as two triangles. Plain triangles rather than
// an indexed strip keep every meter's geometry the same shape, which is one
// of the conditions for the renderer to merge them into one draw call.
constexpr int kVerticesPerRect = 6;
constexpr int kVertexCount = kMeterMaxRects * kVerticesPerRect;

void set_rect(QSGGeometry::ColoredPoint2D* v, const MeterRect& r) {
    const uchar a = static_cast<uchar>(r.argb >> 24);
    const uchar red = static_cast<uchar>(r.argb >> 16);
    const uchar g = static_cast<uchar>(r.argb >> 8);
    const uchar b = static_cast<uchar>(r.argb);
    const float x0 = r.x, y0 = r.y, x1 = r.x + r.w, y1 = r.y + r.h;
    v[0].set(x0, y0, red, g, b, a);
    v[1].set(x1, y0, red, g, b, a);
    v[2].set(x0, y1, red, g, b, a);
    v[3].set(x1, y0, red, g, b, a);
    v[4].set(x1, y1, red, g, b, a);
    v[5].set(x0, y1, red, g, b, a);
}

}  // namespace

MeterItem::MeterItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    setAcceptedMouseButtons(Qt::LeftButton);
}

MeterItem::~MeterItem() {
    if (source_ && attached_) source_->detachMeter(this);
}

void MeterItem::setChannel(int c) {
    if (c == channel_) return;
    channel_ = c;
    emit channelChanged();
    attach();
}

void MeterItem::setBusSide(int s) {
    if (s == bus_side_) return;
    bus_side_ = s;
    emit busSideChanged();
    attach();
}

void MeterItem::setSource(ShellModel* s) {
    if (s == source_) return;
    if (source_ && attached_) source_->detachMeter(this);
    attached_ = false;
    source_ = s;
    emit sourceChanged();
    attach();
}

void MeterItem::componentComplete() {
    QQuickItem::componentComplete();
    attach();
}

void MeterItem::attach() {
    if (attached_ || !source_ || (channel_ < 0 && bus_side_ < 0) || !isComponentComplete()) return;
    source_->attachMeter(this);
    attached_ = true;
}

void MeterItem::feed(float peak, float dt_s) {
    ballistics_.feed(peak, dt_s);
    update();
}

void MeterItem::setNoData() {
    if (!ballistics_.has_data) return;
    ballistics_.set_no_data();
    update();
}

void MeterItem::clearOver() {
    ballistics_.clear_over();
    update();
}

void MeterItem::mousePressEvent(QMouseEvent* e) {
    // Clearing the over indicator is the only thing a click on a meter does.
    // It is the interface's own state, not the engine's, so it needs no
    // command; when the engine counts clips (§4.1's clip_count), this clears
    // this reader's view of them and nobody else's.
    clearOver();
    e->accept();
}

QSGNode* MeterItem::updatePaintNode(QSGNode* old, UpdatePaintNodeData*) {
    // The software adaptation cannot draw a custom geometry node. Drawing
    // nothing is honest only because the shell says, in red, that meters are
    // unavailable on this rendering path (ShellModel::metersAvailable).
    if (window() && window()->rendererInterface()->graphicsApi() ==
                        QSGRendererInterface::Software) {
        delete old;
        return nullptr;
    }

    auto* node = static_cast<QSGGeometryNode*>(old);
    if (node == nullptr) {
        node = new QSGGeometryNode;
        // Sized once for the most rectangles a meter ever draws; never
        // reallocated afterwards.
        auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(),
                                         kVertexCount);
        geometry->setDrawingMode(QSGGeometry::DrawTriangles);
        node->setGeometry(geometry);
        node->setFlag(QSGNode::OwnsGeometry);
        node->setMaterial(new QSGVertexColorMaterial);
        node->setFlag(QSGNode::OwnsMaterial);
    }

    MeterRect rects[kMeterMaxRects];
    const int n = meter_rects(ballistics_, static_cast<float>(width()),
                              static_cast<float>(height()), rects);
    auto* v = node->geometry()->vertexDataAsColoredPoint2D();
    for (int i = 0; i < kMeterMaxRects; ++i) {
        set_rect(v + i * kVerticesPerRect, i < n ? rects[i] : MeterRect{});
    }
    node->markDirty(QSGNode::DirtyGeometry);
    return node;
}

}  // namespace sonora::ui
