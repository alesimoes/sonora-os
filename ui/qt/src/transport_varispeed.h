// SPDX-License-Identifier: GPL-3.0-or-later
//
// The varispeed field on the transport bar (ADR-0190): a playback ratio
// typed in percent, 50 to 200, and the pitch shift it brings, in semitones,
// beside it (varispeed moves pitch with speed, as tape did). The ratio is
// sent as transport.varispeed through the shell, so a key and the field
// reach the engine by one road.
//
// Text outside the range, or not a number, is refused here, before anything
// is sent, and the reason is held in `refusal` until the next entry: the
// field must not show 49 % while the engine plays at 100.
#pragma once

#include <QObject>
#include <QString>
#include <functional>

namespace sonora::ui {

class ShellModel;

class TransportVarispeed : public QObject {
    Q_OBJECT
    Q_PROPERTY(ShellModel* shell READ shell WRITE setShell NOTIFY shellChanged)
    Q_PROPERTY(double percent READ percent NOTIFY changed)
    Q_PROPERTY(QString percentText READ percentText NOTIFY changed)
    Q_PROPERTY(QString semitonesText READ semitonesText NOTIFY changed)
    Q_PROPERTY(bool atUnity READ atUnity NOTIFY changed)
    Q_PROPERTY(QString refusal READ refusal NOTIFY refusalChanged)

public:
    // Sends transport.varispeed with the ratio text; false when it could not
    // be sent at all.
    using Sender = std::function<bool(const QString& ratio)>;

    explicit TransportVarispeed(QObject* parent = nullptr) : QObject(parent) {}
    explicit TransportVarispeed(Sender send, QObject* parent = nullptr)
        : QObject(parent), send_(std::move(send)) {}

    ShellModel* shell() const { return shell_; }
    void setShell(ShellModel* shell);

    double percent() const { return percent_; }
    QString percentText() const;
    QString semitonesText() const;
    bool atUnity() const { return percent_ == 100.0; }
    QString refusal() const { return refusal_; }

    // "150", "150%", "150 %", "87.5": true when sent. Otherwise `refusal`
    // says why and the shown value is unchanged.
    Q_INVOKABLE bool submit(const QString& text);
    // Double-click: back to 100 %.
    Q_INVOKABLE bool reset() { return submit(QStringLiteral("100")); }

    static double semitones(double percent);

signals:
    void changed();
    void refusalChanged();
    void shellChanged();

private:
    void refuse(const QString& why);

    ShellModel* shell_ = nullptr;
    Sender send_;
    double percent_ = 100.0;
    QString refusal_;
};

}  // namespace sonora::ui
