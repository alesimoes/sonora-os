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

void SessionModel::installWindowCommands(CommandRegistry& local, const InstallSimple& simple) {
    using Why = std::string;
    local.install("window.memory_locations", [this](const CommandContext&) {
        setMemoryLocationsOpen(!memory_locations_open_);
        return true;
    });
    local.install("window.event_list", [this](const CommandContext&) {
        setEventListOpen(!event_list_open_);
        return true;
    });
    local.install("window.latency", [this](const CommandContext&) {
        setLatencyOpen(!latency_open_);
        return true;
    });
    local.install("window.bounce", [this](const CommandContext&) {
        setBounceOpen(!bounce_open_);
        return true;
    });
    local.install("window.preferences", [this](const CommandContext&) {
        setPreferencesOpen(!prefs_open_);
        return true;
    });
    local.install("window.configurations", [this](const CommandContext&) {
        setWindowConfigsOpen(!window_configs_open_);
        return true;
    });
    // The argument is the slot. A bad or empty one is refused with its
    // reason: the screen must not move on a guess.
    const auto slot_of = [](const CommandContext& ctx, int& slot, Why& why) {
        bool ok = false;
        slot = QString::fromStdString(ctx.argument).trimmed().toInt(&ok);
        if (!ok) why = "name a window configuration, 1 to " + std::to_string(WindowLayouts::kSlots);
        return ok;
    };
    simple("window.config_store", [this, slot_of](const CommandContext& ctx, Why& why) {
        int slot = 0;
        return slot_of(ctx, slot, why) && storeWindowConfig(slot, why);
    });
    simple("window.config_recall", [this, slot_of](const CommandContext& ctx, Why& why) {
        int slot = 0;
        return slot_of(ctx, slot, why) && recallWindowConfig(slot, why);
    });
}

void SessionModel::installFileCommands(CommandRegistry& /*local*/, const InstallSimple& simple) {
    using Why = std::string;
    // Saving is the engine's once it has session commands (session_files.h);
    // until then the interface writes the file itself.
    simple("app.save", [this](const CommandContext&, Why& why) { return saveSession(why); });
    // Pro Tools' Save As and Save Copy In ask for the name first; the key
    // sends none, so the window's dialog asks and sends it back.
    const auto save_as = [this](bool copy) {
        return [this, copy](const CommandContext& ctx, Why& why) {
            if (ctx.argument.empty()) {
                if (overlay_.recording) {
                    why = std::string("stop recording before ") + (copy ? "Save Copy In" : "Save As");
                    return false;
                }
                const QString root = saveAsRoot();
                const std::string& n = ctl_.snapshot().settings.name;
                const QString base = n.empty() ? QStringLiteral("Untitled") : q(n);
                emit saveAsRequested(copy, unused_session_name(root, copy ? base + QStringLiteral(" copy") : base),
                                     root);
                return true;
            }
            return saveSessionAs(q(ctx.argument), copy, why);
        };
    };
    simple("app.save_as", save_as(false));
    simple("app.save_copy_in", save_as(true));
    // Pro Tools' Save As Template asks for the name as Save As does; the
    // markers are the dialog's check box, kept on the model.
    simple("app.save_as_template", [this](const CommandContext& ctx, Why& why) {
        if (ctx.argument.empty()) {
            if (overlay_.recording) {
                why = "stop recording before Save as Template";
                return false;
            }
            const std::string& n = ctl_.snapshot().settings.name;
            const QString base = n.empty() ? QStringLiteral("Untitled") : q(n);
            emit saveAsTemplateRequested(
                unused_session_name(templates_dir_, base + QStringLiteral(" template")), templates_dir_);
            return true;
        }
        return saveSessionAsTemplate(q(ctx.argument), template_markers_, why);
    });
    simple("app.new_from_template", [this](const CommandContext&, Why&) {
        emit templateBrowserRequested();
        return true;
    });

    simple("session.import_audio", [this](const CommandContext&, Why& why) {
        if (overlay_.recording) {
            why = "stop recording before Import Audio";
            return false;
        }
        emit importBrowserRequested();
        return true;
    });

    simple("window.toggle_mix_edit", [this](const CommandContext&, Why&) {
        setWindow(window_ == QStringLiteral("edit") ? QStringLiteral("mix") : QStringLiteral("edit"));
        return true;
    });
}

}  // namespace sonora::ui
