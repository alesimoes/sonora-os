// SPDX-License-Identifier: GPL-3.0-or-later
#include "live_loops_model.h"

#include <QVariantMap>

namespace sonora::ui {
namespace {

constexpr launch::Quantize kQuantizes[] = {launch::Quantize::Immediate, launch::Quantize::Beat,
                                           launch::Quantize::Bar, launch::Quantize::Bars2,
                                           launch::Quantize::Bars4};
constexpr int kQuantizeCount = static_cast<int>(sizeof(kQuantizes) / sizeof(kQuantizes[0]));

}  // namespace

LiveLoopsModel::LiveLoopsModel(launch::Launcher& launcher, GridSource grid, QObject* parent)
    : QObject(parent), launcher_(launcher), grid_(std::move(grid)) {}

QString LiveLoopsModel::stateName(launch::CellState s) {
    switch (s) {
        case launch::CellState::Stopped: return QStringLiteral("stopped");
        case launch::CellState::Queued: return QStringLiteral("queued");
        case launch::CellState::Playing: return QStringLiteral("playing");
        case launch::CellState::QueuedStop: return QStringLiteral("queuedStop");
        case launch::CellState::Armed: return QStringLiteral("armed");
        case launch::CellState::Recording: return QStringLiteral("recording");
    }
    return QStringLiteral("stopped");
}

QStringList LiveLoopsModel::quantizeNames() const {
    return {QStringLiteral("Immediate"), QStringLiteral("Beat"), QStringLiteral("Bar"),
            QStringLiteral("2 Bars"), QStringLiteral("4 Bars")};
}

int LiveLoopsModel::trackCount() const { return static_cast<int>(grid_().tracks()); }

QVariantList LiveLoopsModel::scenes() const {
    const launch::LaunchGrid& g = grid_();
    QVariantList out;
    for (std::uint32_t s = 0; s < g.scenes(); ++s) {
        QVariantList cells;
        for (std::uint32_t t = 0; t < g.tracks(); ++t) {
            const launch::Cell& c = g.cell(t, s);
            QVariantMap m;
            m.insert(QStringLiteral("track"), static_cast<int>(t));
            m.insert(QStringLiteral("scene"), static_cast<int>(s));
            m.insert(QStringLiteral("hasClip"), c.clip != launch::kNoClip);
            m.insert(QStringLiteral("state"), stateName(c.state));
            cells.push_back(m);
        }
        QVariantMap row;
        row.insert(QStringLiteral("scene"), static_cast<int>(s));
        row.insert(QStringLiteral("cells"), cells);
        out.push_back(row);
    }
    return out;
}

void LiveLoopsModel::result(bool accepted, const QString& refusal) {
    if (!accepted && notice_ != refusal) {
        notice_ = refusal;
        emit noticeChanged();
    }
    emit changed();
}

void LiveLoopsModel::setQuantize(int index) {
    if (index < 0 || index >= kQuantizeCount) return;
    if (!launcher_.set_quantize(kQuantizes[index])) {
        result(false, QStringLiteral("The launcher refused the quantise change; it stays at %1.")
                          .arg(quantizeNames().at(quantize_)));
        return;
    }
    quantize_ = index;
    result(true, {});
}

void LiveLoopsModel::triggerCell(int track, int scene) {
    const bool ok = track >= 0 && scene >= 0 &&
                    launcher_.trigger_cell(static_cast<std::uint32_t>(track),
                                           static_cast<std::uint32_t>(scene));
    result(ok, QStringLiteral("Cell %1 of scene %2 did not launch: it is empty or the launcher's queue is full.")
                   .arg(track + 1).arg(scene + 1));
}

void LiveLoopsModel::triggerScene(int scene) {
    const bool ok = scene >= 0 && launcher_.trigger_scene(static_cast<std::uint32_t>(scene));
    result(ok, QStringLiteral("Scene %1 did not launch: the launcher's queue is full.").arg(scene + 1));
}

void LiveLoopsModel::stopTrack(int track) {
    const bool ok = track >= 0 && launcher_.stop_track(static_cast<std::uint32_t>(track));
    result(ok, QStringLiteral("Track %1 did not stop: the launcher's queue is full.").arg(track + 1));
}

void LiveLoopsModel::stopAll() {
    result(launcher_.stop_all(), QStringLiteral("Stop all was not accepted: the launcher's queue is full."));
}

void LiveLoopsModel::dismissNotice() {
    if (notice_.isEmpty()) return;
    notice_.clear();
    emit noticeChanged();
}

}  // namespace sonora::ui
