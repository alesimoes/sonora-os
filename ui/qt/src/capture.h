// SPDX-License-Identifier: GPL-3.0-or-later
//
// Screenshot mode (--capture-size): the window is given the reviewer's
// reference size, and the banner about the rig's missing GPU is not drawn.
// Parsing and sizing live here, apart from main(), so a test can hold them.
#pragma once

#include <QSize>
#include <QString>
#include <QStringList>

namespace sonora::ui {

// "1920x1080"; false for anything else, and for a side under 640 x 360.
inline bool parse_capture_size(const QString& text, QSize& out) {
    const QStringList parts = text.split(QLatin1Char('x'));
    if (parts.size() != 2) return false;
    bool w_ok = false, h_ok = false;
    const int w = parts[0].toInt(&w_ok), h = parts[1].toInt(&h_ok);
    if (!w_ok || !h_ok || w < 640 || h < 360 || w > 7680 || h > 4320) return false;
    out = QSize(w, h);
    return true;
}

// The window's size in device-independent pixels for a picture of
// `pixels`, under QT_SCALE_FACTOR `scale` (1.0 when unset).
inline QSize capture_window_size(const QSize& pixels, double scale) {
    if (scale < 1.0) scale = 1.0;
    return QSize(static_cast<int>(pixels.width() / scale + 0.5),
                 static_cast<int>(pixels.height() / scale + 0.5));
}

}  // namespace sonora::ui
