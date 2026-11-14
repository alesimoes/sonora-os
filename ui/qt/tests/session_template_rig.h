// SPDX-License-Identifier: GPL-3.0-or-later
//
// The session-template tests' shared rig: a SessionModel behind a replica of
// a FakeEngine, wired as main() wires them, with its own sessions and
// templates folders. Header-only, test code only.
#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "control_adapter.h"
#include "key_router.h"
#include "peak_source.h"
#include "replica_document.h"
#include "session_model.h"
#include "shell_model.h"
#include "sonora/api/fake_engine.h"
#include "timeline_document.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace sonora::ui::template_tests {

inline QByteArray contents(const QString& file) {
    QFile f(file);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

inline std::map<QString, QByteArray> tree(const QString& dir) {
    std::map<QString, QByteArray> out;
    const QDir root(dir);
    std::function<void(const QString&)> walk = [&](const QString& at) {
        const QDir d(at);
        for (const QFileInfo& i : d.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)) {
            const QString rel = root.relativeFilePath(i.absoluteFilePath());
            if (i.isDir()) {
                out[rel + QLatin1Char('/')] = QByteArray();
                walk(i.absoluteFilePath());
            } else {
                out[rel] = contents(i.absoluteFilePath());
            }
        }
    };
    walk(dir);
    return out;
}

struct Rig {
    // First: the fake's sessions folder is where a template's session is made.
    QTemporaryDir sessions, templates;
    api::testing::FakeEngine fake{config(sessions.path().toStdString())};
    ControlAdapter engine{fake};
    ReplicaDocument doc{[this](const api::ModelRequest& r) { return engine.submitModel(r); },
                        make_empty_session(4, 48000)};
    SyntheticPeakSource peaks{48000, 120.0};
    SessionModel session{doc, peaks, true};
    KeyRouter router{KeyMap::built_in_default(), api::testing::FakeEngine::engine_commands(),
                     [this](const std::string& c, const CommandContext& ctx) {
                         return engine.send(c, ctx);
                     }};
    ShellModel shell{engine, router};

    static api::testing::FakeEngine::Config config(const std::string& sessions_root) {
        api::testing::FakeEngine::Config c;
        c.tracks = 4;
        c.sessions_root = sessions_root;
        return c;
    }

    Rig() {
        session.setSessionsDir(sessions.path());
        session.setTemplatesDir(templates.path());
        // As main() wires them.
        engine.setFolderSink([this](const std::string& folder, bool untitled) {
            session.setEngineFolder(QString::fromStdString(folder), untitled);
        });
        engine.setModelSink([this](const std::vector<api::ModelUpdate>& u,
                                   const std::vector<api::CommandReply>& r) {
            if (doc.receive(u, r)) session.documentUpdated();
        });
        doc.on_refused = [this](const std::string& why) {
            emit session.notice(QString::fromStdString(why));
        };
        doc.on_load_abandoned = [this](const std::string& refused, bool later_load) {
            if (refused.empty()) {
                session.openWasLost();
            } else {
                session.openWasRefused(QString::fromStdString(refused), later_load);
            }
        };
        QObject::connect(&session, &SessionModel::openNotice, &shell, &ShellModel::setOpenNotice);
        QObject::connect(&shell, &ShellModel::sessionFolder, &session, &SessionModel::setEngineFolder);
        session.installCommands(router.local());
        router.setRefusal([this](const std::string& command) { return session.commandRefusal(command); });
        router.prepare({});
    }
    bool connect() {
        std::string error;
        if (!engine.connect(error)) return false;
        settle();
        return doc.synced();
    }
    void settle() {
        for (int i = 0; i < 8; ++i) shell.tick();
    }
};

}  // namespace sonora::ui::template_tests
