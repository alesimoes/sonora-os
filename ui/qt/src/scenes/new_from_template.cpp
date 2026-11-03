// SPDX-License-Identifier: GPL-3.0-or-later
//
// The New from Template browser (ADR-0030), as app.new_from_template opens
// it, over four templates written for the picture: the first chosen, with
// the new session's name offered; one made at another sample rate, shown
// as unusable with the reason; and one whose Delete is asking to confirm.
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QQuickItem>
#include <cstdio>
#include <string>

#include "screenshot_scene.h"
#include "session_files.h"
#include "sonora/model/session_file.h"
#include "sonora/model/session_template.h"
#include "timeline_document.h"

namespace sonora::ui {
namespace {

void write_template(const QString& dir, const QString& name, model::Snapshot s, const QDateTime& when) {
    s.settings.name = name.toStdString();
    std::string bytes, error;
    if (!model::serialize(s, model::WriteInfo{}, bytes, error)) {
        std::fprintf(stderr, "sonora-ui: scene: template %s: %s\n", qPrintable(name), error.c_str());
        return;
    }
    const QString file = session_file_path(dir, name);
    QDir().mkpath(QFileInfo(file).absolutePath());
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(bytes.data(), static_cast<qint64>(bytes.size()));
    f.flush();
    // A fixed date, so the picture does not change with the day it is shot.
    f.setFileTime(when, QFileDevice::FileModificationTime);
}

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-new-from-template");
    s.take = false;
    s.apply = [](ShotContext& c) {
        QDir dir(QDir::temp().filePath(QStringLiteral("sonora-scene-templates")));
        dir.removeRecursively();
        QDir().mkpath(dir.path());
        const model::Snapshot& now = c.session.controller().snapshot();
        const uint32_t rate = now.settings.sample_rate;
        const uint32_t other = rate == 44100 ? 48000 : 44100;
        const QDate day(2026, 10, 30);
        write_template(dir.path(), QStringLiteral("Band Tracking"), make_empty_session(16, rate),
                       QDateTime(day, QTime(19, 42)));
        write_template(dir.path(), QStringLiteral("Mix Setup"), model::template_of(now),
                       QDateTime(day.addDays(-6), QTime(11, 5)));
        write_template(dir.path(), QStringLiteral("Podcast"), make_empty_session(2, other),
                       QDateTime(day.addDays(-40), QTime(9, 30)));
        write_template(dir.path(), QStringLiteral("Vocal Overdubs"), make_empty_session(4, rate),
                       QDateTime(day.addDays(-2), QTime(16, 18)));
        c.session.setTemplatesDir(dir.path());
        c.model.invoke(QStringLiteral("app.new_from_template"), -1);
        QQuickItem* root = c.view.rootObject();
        if (auto* panel = root->findChild<QQuickItem*>(QStringLiteral("templatePanel")))
            panel->setProperty("confirming", QStringLiteral("Vocal Overdubs"));
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
