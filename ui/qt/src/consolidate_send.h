// SPDX-License-Identifier: GPL-3.0-or-later
//
// The one place the interface turns a consolidate request into the engine's
// session.consolidate command, shared by Consolidate and Render Clip Gain so
// the two cannot drift apart.
#pragma once

#include <QString>
#include <functional>
#include <string>

#include "edit_controller.h"
#include "sonora/api/consolidate_argument.h"

namespace sonora::ui {

// Sends the request; with no engine it refuses, and says what could not be done.
inline bool send_consolidate(const std::function<void(const QString&, const QString&)>& send,
                             const EditController::ConsolidateRequest& req, const char* what,
                             std::string& why) {
    if (!send) {
        why = std::string("no engine to render ") + what;
        return false;
    }
    api::ConsolidateArgument arg;
    arg.track = req.track;
    arg.playlist = req.playlist;
    arg.start = req.start;
    arg.end = req.end;
    arg.name = req.name;
    send(QStringLiteral("session.consolidate"),
         QString::fromStdString(api::format_consolidate_argument(arg)));
    return true;
}

}  // namespace sonora::ui
