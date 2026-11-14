// SPDX-License-Identifier: GPL-3.0-or-later
#include "transport_varispeed.h"

#include <QLocale>
#include <cmath>

#include "shell_model.h"
#include "sonora/api/varispeed.h"

namespace sonora::ui {

namespace {

constexpr double kMinPercent = api::kVarispeedMin * 100.0;
constexpr double kMaxPercent = api::kVarispeedMax * 100.0;

// "1.5", never "1.500000": the text the engine's parser reads back exactly.
QString ratio_text(double percent) {
    const long long micro = std::llround(percent * 10000.0);  // percent/100 * 1e6
    QString t = QString::number(static_cast<double>(micro) / 1e6, 'f', 6);
    while (t.endsWith(QLatin1Char('0'))) t.chop(1);
    if (t.endsWith(QLatin1Char('.'))) t.chop(1);
    return t;
}

}  // namespace

void TransportVarispeed::setShell(ShellModel* shell) {
    if (shell_ == shell) return;
    shell_ = shell;
    if (shell_) {
        send_ = [shell](const QString& ratio) {
            shell->invokeWith(QString::fromLatin1(api::kVarispeedCommand), -1, ratio);
            return true;
        };
    } else {
        send_ = nullptr;
    }
    emit shellChanged();
}

double TransportVarispeed::semitones(double percent) {
    return 12.0 * std::log2(percent / 100.0);
}

QString TransportVarispeed::percentText() const {
    const int digits = std::floor(percent_) == percent_ ? 0 : 2;
    return QLocale::c().toString(percent_, 'f', digits) + QStringLiteral(" %");
}

QString TransportVarispeed::semitonesText() const {
    const double st = std::round(semitones(percent_) * 100.0) / 100.0;
    if (st == 0.0) return QStringLiteral("0.00 st");
    return QStringLiteral("%1%2 st")
        .arg(st > 0 ? QStringLiteral("+") : QString())
        .arg(QLocale::c().toString(st, 'f', 2));
}

void TransportVarispeed::refuse(const QString& why) {
    if (refusal_ == why) return;
    refusal_ = why;
    emit refusalChanged();
}

bool TransportVarispeed::submit(const QString& text) {
    QString t = text.trimmed();
    if (t.endsWith(QLatin1Char('%'))) t = t.left(t.size() - 1).trimmed();
    bool ok = false;
    const double value = QLocale::c().toDouble(t, &ok);
    if (t.isEmpty() || !ok || !std::isfinite(value)) {
        refuse(QStringLiteral("\"%1\" is not a speed: type a percent from 50 to 200").arg(text));
        return false;
    }
    if (value < kMinPercent || value > kMaxPercent) {
        refuse(QStringLiteral("%1 % is outside 50 to 200 %: varispeed not changed").arg(t));
        return false;
    }
    if (!send_) {
        refuse(QStringLiteral("varispeed cannot be sent: no engine link"));
        return false;
    }
    if (!send_(ratio_text(value))) {
        refuse(QStringLiteral("varispeed was refused: not changed"));
        return false;
    }
    refuse(QString());
    percent_ = value;
    emit changed();
    return true;
}

}  // namespace sonora::ui
