// SPDX-License-Identifier: GPL-3.0-or-later
//
// The New from Template browser (ADR-0030), as app.new_from_template opens
// it, over templates in three categories written for the picture: the first chosen, with
// the new session's name offered; one made at another sample rate, shown
// as unusable with the reason; and one whose Delete is asking to confirm.
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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
        // Two categories and one top-level template (ADR-0146); "Archive"
        // holds one a later build wrote, which is listed greyed.
        const QString band = dir.filePath(QStringLiteral("Band"));
        const QString voice = dir.filePath(QStringLiteral("Voice"));
        write_template(band, QStringLiteral("Band Tracking"), make_empty_session(16, rate),
                       QDateTime(day, QTime(19, 42)));
        write_template(band, QStringLiteral("Vocal Overdubs"), make_empty_session(4, rate),
                       QDateTime(day.addDays(-2), QTime(16, 18)));
        write_template(voice, QStringLiteral("Podcast"), make_empty_session(2, other),
                       QDateTime(day.addDays(-40), QTime(9, 30)));
        write_template(voice, QStringLiteral("Voice-over"), make_empty_session(2, rate),
                       QDateTime(day.addDays(-9), QTime(10, 0)));
        write_template(dir.path(), QStringLiteral("Mix Setup"), model::template_of(now),
                       QDateTime(day.addDays(-6), QTime(11, 5)));
        const QByteArray future = "{\"sonora_session\": " +
                                  QByteArray::number(qint64{model::kFormatVersion} + 1) + "}";
        const QString archive = dir.filePath(QStringLiteral("Archive"));
        const QString future_file = session_file_path(archive, QStringLiteral("Surround Mix"));
        QDir().mkpath(QFileInfo(future_file).absolutePath());
        QFile ff(future_file);
        // Closed before the browser lists the folder: left open, the bytes
        // were still buffered and the row never appeared.
        if (ff.open(QIODevice::WriteOnly)) {
            ff.write(future);
            ff.close();
        }
        c.session.setTemplatesDir(dir.path());
        c.model.invoke(QStringLiteral("app.new_from_template"), -1);
        QQuickItem* root = c.view.rootObject();
        if (auto* panel = root->findChild<QQuickItem*>(QStringLiteral("templatePanel")))
            panel->setProperty("confirming", QStringLiteral("Band/Vocal Overdubs"));
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
