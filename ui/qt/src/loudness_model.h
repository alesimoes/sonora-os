// SPDX-License-Identifier: GPL-3.0-or-later
//
// The master's loudness (ui.mixer.loudness): momentary, short-term and
// integrated LUFS, loudness range and true peak as the engine's meter
// publishes them (ITU-R BS.1770, EBU R128; state block layout 8), against a
// target the operator picks. Every number is the engine's; this model only
// formats them and judges them against the target.
//
// A reading that is not there is said so: before the first block the meter
// has measured nothing (-200), and with the engine gone nothing is current.
// Neither is drawn as a quiet mix, which is what a number at the bottom of
// the scale would look like.
#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <functional>

#include "engine_adapter.h"

namespace sonora::ui {

class LoudnessModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool alive READ alive NOTIFY changed)
    Q_PROPERTY(bool panelOpen READ panelOpen WRITE setPanelOpen NOTIFY panelChanged)
    Q_PROPERTY(QString momentaryText READ momentaryText NOTIFY changed)
    Q_PROPERTY(QString shortTermText READ shortTermText NOTIFY changed)
    Q_PROPERTY(QString integratedText READ integratedText NOTIFY changed)
    Q_PROPERTY(QString rangeText READ rangeText NOTIFY changed)
    Q_PROPERTY(QString truePeakText READ truePeakText NOTIFY changed)
    Q_PROPERTY(QString maxMomentaryText READ maxMomentaryText NOTIFY changed)
    Q_PROPERTY(QString maxShortTermText READ maxShortTermText NOTIFY changed)
    Q_PROPERTY(double momentary READ momentary NOTIFY changed)
    Q_PROPERTY(double integrated READ integrated NOTIFY changed)
    Q_PROPERTY(bool truePeakOver READ truePeakOver NOTIFY changed)
    Q_PROPERTY(QString warning READ warning NOTIFY changed)
    Q_PROPERTY(QVariantList presets READ presets CONSTANT)
    Q_PROPERTY(int preset READ preset WRITE setPreset NOTIFY targetChanged)
    Q_PROPERTY(QString targetText READ targetText NOTIFY targetChanged)
    Q_PROPERTY(QString ceilingText READ ceilingText NOTIFY targetChanged)
    // 0 no target or no reading yet, 1 within the tolerance, 2 outside it.
    Q_PROPERTY(int verdict READ verdict NOTIFY changed)
    Q_PROPERTY(QString deltaText READ deltaText NOTIFY changed)
    Q_PROPERTY(bool canReset READ canReset NOTIFY changed)
    Q_PROPERTY(QString refusal READ refusal NOTIFY refusalChanged)

public:
    // Sends meters.reset with the argument; true when it was sent.
    using Resetter = std::function<bool(const QString& argument)>;
    // Whether this build can run the command (ShellModel::implemented).
    using Implemented = std::function<bool()>;

    LoudnessModel(Resetter reset, Implemented implemented, QObject* parent = nullptr);

    // The tick's feed (ShellModel::setLoudnessSink): the frame and whether
    // the engine is alive. Emits only when something changed.
    void onFrame(const EngineFrame& f, bool alive);

    bool alive() const { return alive_; }
    bool panelOpen() const { return panel_open_; }
    void setPanelOpen(bool on);
    QString momentaryText() const { return level(r_.momentary); }
    QString shortTermText() const { return level(r_.short_term); }
    QString integratedText() const { return level(r_.integrated); }
    QString maxMomentaryText() const { return level(r_.max_momentary); }
    QString maxShortTermText() const { return level(r_.max_short_term); }
    QString rangeText() const;
    QString truePeakText() const { return level(r_.true_peak_max_db); }
    double momentary() const { return r_.momentary; }
    double integrated() const { return r_.integrated; }
    bool truePeakOver() const;
    QString warning() const;
    QVariantList presets() const;
    int preset() const { return preset_; }
    // -1 clears the target. An index past the list is refused by ignoring it.
    void setPreset(int index);
    QString targetText() const;
    QString ceilingText() const;
    int verdict() const;
    QString deltaText() const;
    bool canReset() const;
    QString refusal() const { return refusal_; }

    // Restarts the integrated loudness, the range and the maxima: the
    // engine's meters.reset master. Refused loudly, in `refusal`, when the
    // engine is gone or this build lacks the command.
    Q_INVOKABLE bool reset();
    Q_INVOKABLE void togglePanel() { setPanelOpen(!panel_open_); }
    // A session opened: the target and any held refusal are the last
    // session's, not this one's.
    void sessionOpened();
    // The screenshot scenes' reading, so a picture shows numbers the demo
    // engine does not measure.
    Q_INVOKABLE void showDemo();

    // Pure, for the tests: "-23.4", or the dash when `v` is "none yet".
    static QString level(float v);

signals:
    void changed();
    void panelChanged();
    void targetChanged();
    void refusalChanged();

private:
    void hold(const QString& why);

    Resetter reset_;
    Implemented implemented_;
    LoudnessReading r_;
    bool alive_ = false;
    bool panel_open_ = false;
    bool demo_ = false;
    int preset_ = -1;
    QString refusal_;
};

}  // namespace sonora::ui
