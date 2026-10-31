// SPDX-License-Identifier: GPL-3.0-or-later
#include "scene_item.h"

#include <QFont>
#include <QFontMetricsF>
#include <QQuickWindow>
#include <QSGGeometryNode>
#include <QSGRendererInterface>
#include <QSGTextNode>
#include <QSGVertexColorMaterial>
#include <QTextLayout>
#include <map>

#include "session_model.h"
#include "theme.h"

namespace sonora::ui {

namespace {

constexpr int kVerticesPerRect = 6;

// QSGVertexColorMaterial blends premultiplied colours.
void set_rect(QSGGeometry::ColoredPoint2D* v, const SceneRect& r) {
    const uint32_t a = r.argb >> 24;
    const auto pm = [a](uint32_t c) { return static_cast<uchar>((c & 0xffu) * a / 255u); };
    const uchar red = pm(r.argb >> 16), g = pm(r.argb >> 8), b = pm(r.argb);
    const auto al = static_cast<uchar>(a);
    const float x0 = r.x, y0 = r.y, x1 = r.x + r.w, y1 = r.y + r.h;
    v[0].set(x0, y0, red, g, b, al);
    v[1].set(x1, y0, red, g, b, al);
    v[2].set(x0, y1, red, g, b, al);
    v[3].set(x1, y0, red, g, b, al);
    v[4].set(x1, y1, red, g, b, al);
    v[5].set(x0, y1, red, g, b, al);
}

// Fills `node` with `rects`. The vertex buffer grows when it must and is
// reused otherwise; a rebuild is a copy, not an allocation.
QSGGeometryNode* rect_node(QSGGeometryNode* node, const SceneRect* rects, std::size_t count) {
    const int need = static_cast<int>(count) * kVerticesPerRect;
    if (node == nullptr) {
        node = new QSGGeometryNode;
        auto* g = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), need);
        g->setDrawingMode(QSGGeometry::DrawTriangles);
        node->setGeometry(g);
        node->setFlag(QSGNode::OwnsGeometry);
        node->setMaterial(new QSGVertexColorMaterial);
        node->setFlag(QSGNode::OwnsMaterial);
    } else if (node->geometry()->vertexCount() != need) {
        node->geometry()->allocate(need);
    }
    auto* v = node->geometry()->vertexDataAsColoredPoint2D();
    for (std::size_t i = 0; i < count; ++i) set_rect(v + i * kVerticesPerRect, rects[i]);
    node->markDirty(QSGNode::DirtyGeometry);
    return node;
}

// A geometry node with more than 65535 vertices is drawn only as far as that:
// the scene graph indexes a batch with 16 bits. A session of a dozen tracks
// passes it, and the tracks and lanes drawn last would be blank, so the
// rectangles are split over nodes of at most 10000 (60000 vertices).
constexpr std::size_t kRectsPerNode = 10000;

void rect_group(QSGNode* group, const std::vector<SceneRect>& rects) {
    const std::size_t nodes =
        std::max<std::size_t>(1, (rects.size() + kRectsPerNode - 1) / kRectsPerNode);
    while (static_cast<std::size_t>(group->childCount()) < nodes) {
        group->appendChildNode(rect_node(nullptr, nullptr, 0));
    }
    while (static_cast<std::size_t>(group->childCount()) > nodes) {
        QSGNode* extra = group->lastChild();
        group->removeChildNode(extra);
        delete extra;
    }
    for (std::size_t i = 0; i < nodes; ++i) {
        const std::size_t first = i * kRectsPerNode;
        rect_node(static_cast<QSGGeometryNode*>(group->childAtIndex(static_cast<int>(i))),
                  rects.data() + first, std::min(kRectsPerNode, rects.size() - first));
    }
}

QFont label_font(int px) {
    QFont f;
    f.setFamilies({QString::fromLatin1(theme::kFontFamily), QString::fromLatin1(theme::kFontFallback)});
    f.setPixelSize(px);
    // Tabular figures, so a ruler's labels and a counter do not shimmer as
    // their digits change.
    f.setFeature(QFont::Tag("tnum"), 1);
    return f;
}

QColor colour(uint32_t argb) { return QColor::fromRgba(argb); }

// Replaces `parent`'s children with text nodes for `labels`, one per colour.
void label_nodes(QSGNode* parent, QQuickWindow* window, const std::vector<SceneLabel>& labels) {
    while (QSGNode* c = parent->firstChild()) {
        parent->removeChildNode(c);
        delete c;
    }
    if (window == nullptr || labels.empty()) return;
    std::map<std::pair<uint32_t, int>, QSGTextNode*> nodes;
    for (const SceneLabel& l : labels) {
        if (l.max_w < 8) continue;
        auto& node = nodes[{l.argb, l.px}];
        if (node == nullptr) {
            node = window->createTextNode();
            node->setColor(colour(l.argb));
            node->setRenderType(QSGTextNode::QtRendering);
            parent->appendChildNode(node);
        }
        const QFont font = label_font(l.px);
        const QString text = QFontMetricsF(font).elidedText(QString::fromStdString(l.text),
                                                            Qt::ElideRight, l.max_w);
        if (text.isEmpty()) continue;
        QTextLayout layout(text, font);
        layout.beginLayout();
        layout.createLine();
        layout.endLayout();
        node->addTextLayout(QPointF(l.x, l.y), &layout);
    }
}

}  // namespace

SceneItem::SceneItem(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    setClip(true);
}

SceneItem::~SceneItem() {
    if (session_) session_->detachItem(this);
}

void SceneItem::setSession(SessionModel* s) {
    if (s == session_) return;
    if (session_) session_->detachItem(this);
    session_ = s;
    if (session_) session_->attachItem(this);
    emit sessionChanged();
    if (isComponentComplete()) sizeChanged();
    markContentDirty();
}

void SceneItem::componentComplete() {
    QQuickItem::componentComplete();
    sizeChanged();
    markContentDirty();
}

void SceneItem::markContentDirty() {
    content_dirty_ = true;
    overlay_dirty_ = true;
    polish();
    update();
}

void SceneItem::markOverlayDirty() {
    overlay_dirty_ = true;
    polish();
    update();
}

void SceneItem::geometryChange(const QRectF& now, const QRectF& before) {
    QQuickItem::geometryChange(now, before);
    if (now.size() != before.size()) {
        sizeChanged();
        markContentDirty();
    }
}

void SceneItem::updatePolish() {
    if (session_ == nullptr) return;
    if (content_dirty_) {
        buildContent(content_);
        content_dirty_ = false;
        content_uploaded_ = false;
    }
    if (overlay_dirty_) {
        buildOverlay(overlay_);
        overlay_dirty_ = false;
        overlay_uploaded_ = false;
    }
}

QSGNode* SceneItem::updatePaintNode(QSGNode* old, UpdatePaintNodeData*) {
    // As MeterItem: the software adaptation cannot draw vertex-coloured
    // geometry, and the shell's banner says so in red.
    if (window() && window()->rendererInterface()->graphicsApi() == QSGRendererInterface::Software) {
        delete old;
        return nullptr;
    }
    QSGNode* root = old;
    if (root == nullptr) {
        root = new QSGNode;
        root->appendChildNode(new QSGNode);  // content
        root->appendChildNode(new QSGNode);  // labels
        root->appendChildNode(new QSGNode);  // overlay
        content_uploaded_ = overlay_uploaded_ = false;
    }
    QSGNode* content = root->childAtIndex(0);
    QSGNode* labels = root->childAtIndex(1);
    QSGNode* overlay = root->childAtIndex(2);
    if (!content_uploaded_) {
        rect_group(content, content_.rects);
        label_nodes(labels, window(), content_.labels);
        content_uploaded_ = true;
    }
    if (!overlay_uploaded_) {
        rect_group(overlay, overlay_);
        overlay_uploaded_ = true;
    }
    return root;
}

// --- timeline ---

void TimelineItem::buildContent(Scene& out) { session_->buildTimeline(out); }

void TimelineItem::buildOverlay(std::vector<SceneRect>& out) { session_->buildOverlay(out); }

void TimelineItem::sizeChanged() {
    if (session_ && isComponentComplete()) session_->setViewport(width(), height());
}

// --- ruler ---

void RulerItem::setFormat(int f) {
    if (f == format_) return;
    format_ = f;
    emit formatChanged();
    markContentDirty();
}

void RulerItem::buildContent(Scene& out) {
    session_->buildRuler(static_cast<TimeFormat>(format_), height(), out);
}

void MarkerRulerItem::buildContent(Scene& out) { session_->buildMarkerRuler(height(), out); }

void ConductorRulerItem::setKind(int k) {
    if (k == kind_) return;
    kind_ = k;
    emit kindChanged();
    markContentDirty();
}

void ConductorRulerItem::buildContent(Scene& out) {
    session_->buildConductorRuler(kind_ == 1 ? ConductorRuler::Meter : ConductorRuler::Tempo,
                                  height(), out);
}

}  // namespace sonora::ui
