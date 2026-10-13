// SPDX-License-Identifier: GPL-3.0-or-later
//
// One channel's meter, drawn in the Qt Quick scene graph (ADR-0006: a custom
// QQuickItem, not a QWidget, not QPainter, not a stack of QML Rectangles).
//
// The item owns no timer and emits no signal when its level changes. The
// shell's single 60 Hz tick calls feed() and the item schedules a redraw;
// updatePaintNode() rewrites one fixed-size vertex buffer. Every meter uses
// the same vertex-colour material, so the batching renderer can draw all of
// them in one call.
#pragma once

#include <QPointer>
#include <QQuickItem>

#include "meter_model.h"
#include "shell_model.h"

class QSGNode;

namespace sonora::ui {

class MeterItem : public QQuickItem {
    Q_OBJECT
    // Set once by QML; neither is expected to change afterwards.
    Q_PROPERTY(int channel READ channel WRITE setChannel NOTIFY channelChanged)
    Q_PROPERTY(sonora::ui::ShellModel* source READ source WRITE setSource NOTIFY sourceChanged)

public:
    explicit MeterItem(QQuickItem* parent = nullptr);
    ~MeterItem() override;

    int channel() const { return channel_; }
    void setChannel(int c);
    ShellModel* source() const { return source_; }
    void setSource(ShellModel* s);

    // From the shell's tick, on the GUI thread.
    void feed(float peak, float dt_s);
    void setNoData();
    const MeterBallistics& ballistics() const { return ballistics_; }

    Q_INVOKABLE void clearOver();

signals:
    void channelChanged();
    void sourceChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override;
    void componentComplete() override;
    void mousePressEvent(QMouseEvent* e) override;

private:
    void attach();

    int channel_ = -1;
    QPointer<ShellModel> source_;
    bool attached_ = false;
    MeterBallistics ballistics_;
};

}  // namespace sonora::ui
