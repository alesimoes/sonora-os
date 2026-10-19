// SPDX-License-Identifier: GPL-3.0-or-later
//
// The page sonora-ui shows instead of its windows when the engine that
// answers speaks another control protocol or state layout version
// (SocketEngine::refused_version). Exiting would leave the appliance on a
// black screen that systemd restarts into the same refusal for ever, so the
// interface stays up on this page, says why, and does not retry: the
// engine at the endpoint is still that build.
#pragma once

#include <QString>

class QQuickView;

namespace sonora::ui {

// Loads RefusalPage.qml into `view` with `reason`, its first letter
// capitalised. False, with the QML errors in `error`, if it did not load.
bool load_refusal_page(QQuickView& view, const QString& reason, QString& error);

}  // namespace sonora::ui
