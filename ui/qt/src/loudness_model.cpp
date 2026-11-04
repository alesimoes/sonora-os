// SPDX-License-Identifier: GPL-3.0-or-later
#include "loudness_model.h"

#include <QVariantMap>
#include <cmath>
#include <cstring>

namespace sonora::ui {

namespace {

// The engine's "none yet": anything at or under this is not a measurement.
constexpr float kNone = -199.0f;

struct Preset {
    const char* name;
    double lufs;
    double ceiling_dbtp;   // the true-peak limit that goes with the target
    double tolerance_lu;
};

// The delivery targets engineers are handed: streaming services, podcasts,
// EBU R128 broadcast (Tech 3341, +-0.5 LU, -1 dBTP) and ATSC A/85 (+-2 LU,
// -2 dBTP).
constexpr Preset kPresets[] = {
    {"Streaming", -14.0, -1.0, 1.0},
    {"Podcast", -16.0, -1.0, 1.0},
    {"EBU R128", -23.0, -1.0, 0.5},
    {"ATSC A/85", -24.0, -2.0, 2.0},
};
constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));

QString signed_lu(double v) {
    return QString(v > 0 ? "+" : "") + QString::number(v, 'f', 1) + QStringLiteral(" LU");
}

}  // namespace

LoudnessModel::LoudnessModel(Resetter reset, Implemented implemented, QObject* parent)
    : QObject(parent), reset_(std::move(reset)), implemented_(std::move(implemented)) {}

QString LoudnessModel::level(float v) {
    if (!(v > kNone)) return QStringLiteral("—");
    return QString::number(static_cast<double>(v), 'f', 1);
}

void LoudnessModel::onFrame(const EngineFrame& f, bool alive) {
    if (demo_) return;
    const LoudnessReading& n = alive ? f.master_loudness : LoudnessReading{};
    const bool same = alive == alive_ && std::memcmp(&n, &r_, sizeof(n)) == 0;
    if (same) return;
    alive_ = alive;
    r_ = n;
    emit changed();
}

void LoudnessModel::setPanelOpen(bool on) {
    if (panel_open_ == on) return;
    panel_open_ = on;
    emit panelChanged();
}

QString LoudnessModel::rangeText() const {
    // LRA is 0 until the gate has passed enough blocks; with nothing
    // integrated yet it is not a measurement either.
    if (!(r_.integrated > kNone)) return QStringLiteral("—");
    return QString::number(static_cast<double>(r_.range_lu), 'f', 1);
}

bool LoudnessModel::truePeakOver() const {
    if (preset_ < 0 || !(r_.true_peak_max_db > kNone)) return false;
    return r_.true_peak_max_db > kPresets[preset_].ceiling_dbtp;
}

QString LoudnessModel::warning() const {
    if (r_.nonfinite == 0) return QString();
    return QStringLiteral("The meter replaced %1 invalid samples since its reset: "
                          "the signal is faulty, not silent.")
        .arg(r_.nonfinite);
}

QVariantList LoudnessModel::presets() const {
    QVariantList out;
    for (const Preset& p : kPresets) {
        QVariantMap m;
        m[QStringLiteral("name")] = QString::fromLatin1(p.name);
        m[QStringLiteral("lufs")] = p.lufs;
        m[QStringLiteral("label")] =
            QString::number(p.lufs, 'f', 0) + QStringLiteral(" LUFS");
        out << m;
    }
    return out;
}

void LoudnessModel::setPreset(int index) {
    if (index < -1 || index >= kPresetCount || index == preset_) return;
    preset_ = index;
    emit targetChanged();
    emit changed();
}

QString LoudnessModel::targetText() const {
    if (preset_ < 0) return QStringLiteral("No target");
    return QString::fromLatin1(kPresets[preset_].name) + QStringLiteral(" ") +
           QString::number(kPresets[preset_].lufs, 'f', 0) + QStringLiteral(" LUFS");
}

QString LoudnessModel::ceilingText() const {
    if (preset_ < 0) return QString();
    return QStringLiteral("ceiling ") + QString::number(kPresets[preset_].ceiling_dbtp, 'f', 0) +
           QStringLiteral(" dBTP");
}

int LoudnessModel::verdict() const {
    if (preset_ < 0 || !alive_ || !(r_.integrated > kNone)) return 0;
    const double d = static_cast<double>(r_.integrated) - kPresets[preset_].lufs;
    return std::fabs(d) <= kPresets[preset_].tolerance_lu ? 1 : 2;
}

QString LoudnessModel::deltaText() const {
    if (verdict() == 0) return QString();
    return signed_lu(static_cast<double>(r_.integrated) - kPresets[preset_].lufs);
}

bool LoudnessModel::canReset() const { return alive_ && (!implemented_ || implemented_()); }

void LoudnessModel::hold(const QString& why) {
    if (refusal_ == why) return;
    refusal_ = why;
    emit refusalChanged();
}

bool LoudnessModel::reset() {
    if (demo_) {
        demo_ = false;
        r_ = LoudnessReading{};
        emit changed();
        return true;
    }
    if (!alive_) {
        hold(QStringLiteral("The engine is not responding: the loudness was not reset."));
        return false;
    }
    if (implemented_ && !implemented_()) {
        hold(QStringLiteral("This engine cannot reset its meters (meters.reset): the "
                            "integrated loudness keeps counting."));
        return false;
    }
    if (!reset_ || !reset_(QStringLiteral("master"))) {
        hold(QStringLiteral("The reset was not sent: the integrated loudness keeps counting."));
        return false;
    }
    hold(QString());
    return true;
}

void LoudnessModel::sessionOpened() {
    demo_ = false;
    setPreset(-1);
    hold(QString());
    r_ = LoudnessReading{};
    emit changed();
}

void LoudnessModel::showDemo() {
    demo_ = true;
    alive_ = true;
    r_ = LoudnessReading{};
    r_.momentary = -21.8f;
    r_.short_term = -22.6f;
    r_.max_momentary = -17.9f;
    r_.max_short_term = -20.4f;
    r_.integrated = -23.2f;
    r_.range_lu = 6.4f;
    r_.true_peak_max_db = -2.3f;
    preset_ = 2;
    panel_open_ = true;
    emit targetChanged();
    emit panelChanged();
    emit changed();
}

}  // namespace sonora::ui
