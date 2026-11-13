// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Live Loops grid (ADR-0164): cells by track and scene, scene launch,
// the quantise selector. Commands go through launch::Launcher and the grid
// shown is the one the host hands over (the UI's mirror, never the audio
// thread's own state). A command the launcher refuses is a held notice,
// never the 2.5 s status line, and a cell is drawn in the state the grid
// reports, so a refused launch never shows as queued.
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <functional>

#include "sonora/launch/launch_grid.h"
#include "sonora/launch/launcher.h"

namespace sonora::ui {

class LiveLoopsModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList scenes READ scenes NOTIFY changed)
    Q_PROPERTY(int trackCount READ trackCount NOTIFY changed)
    Q_PROPERTY(QStringList quantizeNames READ quantizeNames CONSTANT)
    Q_PROPERTY(int quantize READ quantize WRITE setQuantize NOTIFY changed)
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged)

public:
    using GridSource = std::function<const launch::LaunchGrid&()>;

    LiveLoopsModel(launch::Launcher& launcher, GridSource grid, QObject* parent = nullptr);

    QVariantList scenes() const;
    int trackCount() const;
    QStringList quantizeNames() const;
    int quantize() const { return quantize_; }
    QString notice() const { return notice_; }

    void setQuantize(int index);

    Q_INVOKABLE void triggerCell(int track, int scene);
    Q_INVOKABLE void triggerScene(int scene);
    Q_INVOKABLE void stopTrack(int track);
    Q_INVOKABLE void stopAll();
    Q_INVOKABLE void dismissNotice();
    // The grid moved under us (the launcher advanced): draw it again.
    Q_INVOKABLE void refresh() { emit changed(); }

    static QString stateName(launch::CellState s);

signals:
    void changed();
    void noticeChanged();

private:
    void result(bool accepted, const QString& refusal);

    launch::Launcher& launcher_;
    GridSource grid_;
    int quantize_ = 2;  // Bar, the launcher's default
    QString notice_;
};

}  // namespace sonora::ui
