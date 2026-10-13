// SPDX-License-Identifier: GPL-3.0-or-later
//
// The scene-graph items of the Edit window: the timeline and its rulers.
// Custom QQuickItems drawing in the Qt Quick scene graph (ADR-0006): one
// vertex-coloured geometry node for all of a layer's rectangles, and text
// nodes for its labels -- not QWidgets, not QPainter, not QML Rectangles.
//
// The work is split by thread the way Qt Quick intends. What to draw is
// computed on the GUI thread in updatePolish(), from timeline_scene.h, and
// only when something changed (an edit, a zoom, a scroll, a resize). The
// render thread's updatePaintNode() copies the result into vertices. The
// overlay (playhead, insertion cursor) is a separate node so the 60 Hz
// playhead moves without rebuilding a single waveform column.
#pragma once

#include <QPointer>
#include <QQuickItem>
#include <vector>

#include "session_model.h"
#include "timeline_scene.h"

class QSGNode;

namespace sonora::ui {


class SceneItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(sonora::ui::SessionModel* session READ session WRITE setSession NOTIFY sessionChanged)

public:
    explicit SceneItem(QQuickItem* parent = nullptr);
    ~SceneItem() override;

    SessionModel* session() const { return session_; }
    void setSession(SessionModel* s);

    // From SessionModel, on the GUI thread. Neither emits a signal.
    void markContentDirty();
    void markOverlayDirty();

    // What the last polish produced. For tests, which check pixels against
    // it, and for nothing else.
    const Scene& content() const { return content_; }
    const std::vector<SceneRect>& overlay() const { return overlay_; }

signals:
    void sessionChanged();

protected:
    virtual void buildContent(Scene& out) = 0;
    virtual void buildOverlay(std::vector<SceneRect>& out) { out.clear(); }
    virtual void sizeChanged() {}

    void updatePolish() override;
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override;
    void geometryChange(const QRectF& now, const QRectF& before) override;
    void componentComplete() override;

    QPointer<SessionModel> session_;

private:
    Scene content_;
    std::vector<SceneRect> overlay_;
    bool content_dirty_ = true;
    bool overlay_dirty_ = true;
    bool content_uploaded_ = false;
    bool overlay_uploaded_ = false;
};

// The track lanes: regions, waveforms, grid, selection, playhead.
class TimelineItem : public SceneItem {
    Q_OBJECT
public:
    using SceneItem::SceneItem;

protected:
    void buildContent(Scene& out) override;
    void buildOverlay(std::vector<SceneRect>& out) override;
    void sizeChanged() override;
};

// One ruler: timecode, bars|beats or min:secs.
class RulerItem : public SceneItem {
    Q_OBJECT
    // 0 timecode, 1 bars|beats, 2 min:secs (TimeFormat's order).
    Q_PROPERTY(int format READ format WRITE setFormat NOTIFY formatChanged)
public:
    using SceneItem::SceneItem;
    int format() const { return format_; }
    void setFormat(int f);

signals:
    void formatChanged();

protected:
    void buildContent(Scene& out) override;

private:
    int format_ = 0;
};

}  // namespace sonora::ui
