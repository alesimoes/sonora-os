// SPDX-License-Identifier: GPL-3.0-or-later
#include "session_model.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTimer>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

#include "fader_taper.h"
#include "scene_item.h"
#include "edit_clipboard.h"
#include "session_files.h"
#include "sonora/api/consolidate_argument.h"
#include "sonora/api/messages.h"
#include "sonora/api/render.h"
#include "sonora/command.h"
#include "sonora/model/render_in_place.h"
#include "sonora/model/session_file.h"
#include "sonora/model/session_template.h"
#include "transport_model.h"
#include "session_model_support.h"

namespace sonora::ui {

using namespace model;

void SessionModel::installCommands(CommandRegistry& local) {
    using Why = std::string;
    const InstallSimple simple = [this, &local](const char* id, SimpleHandler fn) {
        local.install(id, ExplainingHandler([this, fn](const CommandContext& ctx, Why& why) {
                          const bool ok = fn(ctx, why);
                          sync();
                          return ok;
                      }));
    };
    installEditStateCommands(local, simple);
    installTrackMenuCommands(local, simple);
    installNameCommands(local, simple);
    installClipCommands(local, simple);
    installCompingCommands(local, simple);
    installWindowCommands(local, simple);
    installEditCommands(local, simple);
    installFileCommands(local, simple);
}

// A track command acts on the track given, or the selected one (the
// router fills the target from the selection).
int SessionModel::commandRow(const CommandContext& ctx) const {
    return ctx.has_target() ? ctx.target : ctl_.selection().track;
}

void SessionModel::installEditStateCommands(CommandRegistry& local, const InstallSimple& simple) {
    using Why = std::string;
    const auto mode = [this](EditMode m) {
        return [this, m](const CommandContext&, Why&) {
            ctl_.set_mode(m);
            return true;
        };
    };
    simple("edit.mode.shuffle", mode(EditMode::Shuffle));
    simple("edit.mode.slip", mode(EditMode::Slip));
    simple("edit.mode.spot", mode(EditMode::Spot));
    simple("edit.mode.grid", mode(EditMode::Grid));
    simple("edit.mode.next", [this](const CommandContext&, Why&) {
        ctl_.next_mode();
        return true;
    });

    const auto tool = [this](Tool t) {
        return [this, t](const CommandContext&, Why&) {
            ctl_.set_tool(t);
            return true;
        };
    };
    simple("tool.smart", tool(Tool::Smart));
    simple("tool.trim", tool(Tool::Trim));
    simple("tool.select", tool(Tool::Select));
    simple("tool.grab", tool(Tool::Grab));
    simple("tool.pencil", tool(Tool::Pencil));
    simple("tool.next", [this](const CommandContext&, Why&) {
        ctl_.next_tool();
        return true;
    });

    simple("zoom.in", [this](const CommandContext&, Why&) {
        ctl_.zoom_in();
        return true;
    });
    simple("zoom.out", [this](const CommandContext&, Why&) {
        ctl_.zoom_out();
        return true;
    });
    simple("zoom.vertical_in", [this](const CommandContext&, Why&) {
        ctl_.vertical_zoom(true);
        return true;
    });
    simple("zoom.vertical_out", [this](const CommandContext&, Why&) {
        ctl_.vertical_zoom(false);
        return true;
    });
    simple("zoom.fit_session",
           [this](const CommandContext&, Why& why) { return ctl_.zoom_fit_session(why); });
    simple("zoom.fit_selection",
           [this](const CommandContext&, Why& why) { return ctl_.zoom_fit_selection(why); });
    simple("zoom.toggle", [this](const CommandContext&, Why& why) { return ctl_.zoom_toggle(why); });

    simple("edit.undo", [this](const CommandContext&, Why& why) { return ctl_.undo(why); });
    simple("edit.redo", [this](const CommandContext&, Why& why) { return ctl_.redo(why); });
    // How the operator looks at the history, not the session: the
    // interface's own, so it opens with the engine gone too.
    local.install("window.undo_history", [this](const CommandContext&) {
        setUndoHistoryOpen(!undo_history_open_);
        return true;
    });
    simple("edit.clear", [this](const CommandContext&, Why& why) { return ctl_.clear(why); });
    simple("automation.write_selection", [this](const CommandContext&, Why& why) {
        return ctl_.write_automation_to_selection(why);
    });
    simple("automation.snapshot", [this](const CommandContext&, Why& why) {
        return ctl_.snapshot_automation(why);
    });
    simple("automation.thin",
           [this](const CommandContext&, Why& why) { return ctl_.thin_automation(why); });
    simple("automation.toggle_follows_edit", [this](const CommandContext&, Why&) {
        ctl_.set_automation_follows_edit(!ctl_.automation_follows_edit());
        return true;
    });
    simple("edit.cut", [this](const CommandContext&, Why& why) { return ctl_.cut(why); });
    simple("edit.copy", [this](const CommandContext&, Why& why) { return ctl_.copy(why); });
    // A paste that left lanes out still ran; what it left out goes on the
    // status line rather than nowhere. Queued, because a key's success
    // clears the status line once its handler returns.
    simple("edit.paste", [this](const CommandContext&, Why& why) {
        if (!ctl_.paste(why)) return false;
        if (!why.empty()) {
            QMetaObject::invokeMethod(
                this, [this, said = q(why)]() { emit notice(said); }, Qt::QueuedConnection);
        }
        why.clear();
        return true;
    });
    simple("edit.select_all", [this](const CommandContext&, Why& why) { return ctl_.select_all(why); });

}

}  // namespace sonora::ui
