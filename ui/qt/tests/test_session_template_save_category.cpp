// SPDX-License-Identifier: GPL-3.0-or-later
//
// Save as Template into a category (ADR-0146 §1 and §4): "Category/Name"
// writes <templates>/<Category>/<Name>/<Name>.sonora, making the category's
// folder, and the browser lists it under that category. A deeper path, a
// bad category name, a category that is a template's folder or a link, and
// a name already in the category are refused, and nothing is written. On
// FakeEngine, through the replica, as test_session_templates does.
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QVariantMap>
#include <cstdio>
#include <string>

#include "key_router.h"
#include "session_files.h"
#include "session_model.h"
#include "session_template_rig.h"

using namespace sonora;
using namespace sonora::ui;
using namespace sonora::ui::template_tests;

namespace {

QVariantMap listed(const SessionModel& session, const QString& path) {
    for (const QVariant& v : session.templateList()) {
        const QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("path")).toString() == path) return m;
    }
    return {};
}

int a_template_is_saved_into_a_category() {
    Rig r;
    CHECK(r.connect());
    std::string why;
    const QDir templates(r.templates.path());
    CHECK(r.session.saveSessionAsTemplate(QStringLiteral("Band/Vocal Setup"), false, why));
    r.settle();
    CHECK(QFileInfo(templates.filePath(QStringLiteral("Band/Vocal Setup/Vocal Setup.sonora"))).isFile());
    const QVariantMap m = listed(r.session, QStringLiteral("Band/Vocal Setup"));
    CHECK(m.value(QStringLiteral("category")).toString() == QStringLiteral("Band"));
    CHECK(m.value(QStringLiteral("name")).toString() == QStringLiteral("Vocal Setup"));

    // A second one goes into the folder now there; through the command, as
    // the dialog sends it.
    CommandContext named;
    named.argument = "Band/Drums";
    CHECK(r.router.invoke("app.save_as_template", named).result == DispatchResult::Ok);
    r.settle();
    CHECK(QFileInfo(templates.filePath(QStringLiteral("Band/Drums/Drums.sonora"))).isFile());
    CHECK(listed(r.session, QStringLiteral("Band/Drums")).value(QStringLiteral("category")).toString() ==
          QStringLiteral("Band"));

    // Uncategorised as before, and the same name may be in another category.
    CHECK(r.session.saveSessionAsTemplate(QStringLiteral("Drums"), false, why));
    r.settle();
    CHECK(QFileInfo(templates.filePath(QStringLiteral("Drums/Drums.sonora"))).isFile());
    CHECK(listed(r.session, QStringLiteral("Drums")).value(QStringLiteral("category")).toString().isEmpty());
    return 0;
}

int a_bad_category_is_refused_and_nothing_is_written() {
    Rig r;
    CHECK(r.connect());
    std::string why;
    const QDir templates(r.templates.path());
    CHECK(r.session.saveSessionAsTemplate(QStringLiteral("Band/Vocal Setup"), false, why));
    r.settle();
    CHECK(r.session.saveSessionAsTemplate(QStringLiteral("Solo"), false, why));
    r.settle();
    const auto before = tree(templates.path());
    const uint32_t saves = r.fake.saves();

    const auto refused = [&](const QString& key, const char* expect) {
        std::string w;
        if (r.session.saveSessionAsTemplate(key, false, w)) return false;
        if (w.find(expect) == std::string::npos) {
            std::fprintf(stderr, "  %s: %s\n", qPrintable(key), w.c_str());
            return false;
        }
        return true;
    };
    CHECK(refused(QStringLiteral("Band/Vocal Setup"), "already"));
    CHECK(refused(QStringLiteral("Band/Live/Vocal"), "never another category"));
    CHECK(refused(QStringLiteral("Band/"), "needs a name"));
    CHECK(refused(QStringLiteral("/Vocal"), "the category"));
    CHECK(refused(QStringLiteral("CON/Vocal"), "device name"));
    CHECK(refused(QStringLiteral("Band./Vocal"), "end with '.'"));
    // ADR-0146 §1: a template's folder is never a category.
    CHECK(refused(QStringLiteral("Solo/Vocal"), "is a template"));
    CHECK(QFile(templates.filePath(QStringLiteral("stray"))).open(QIODevice::WriteOnly));
    CHECK(refused(QStringLiteral("stray/Vocal"), "is a file"));
    CHECK(QFile::remove(templates.filePath(QStringLiteral("stray"))));
#ifndef _WIN32
    // A category that is a link would put the template outside the folder.
    QTemporaryDir elsewhere;
    CHECK(elsewhere.isValid());
    CHECK(QFile::link(elsewhere.path(), templates.filePath(QStringLiteral("Linked"))));
    CHECK(refused(QStringLiteral("Linked/Vocal"), "is a link"));
    CHECK(QDir(elsewhere.path()).isEmpty());
    CHECK(QFile::remove(templates.filePath(QStringLiteral("Linked"))));
#endif
    r.settle();
    CHECK(r.fake.saves() == saves);
    CHECK(tree(templates.path()) == before);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    if (a_template_is_saved_into_a_category() != 0) return 1;
    if (a_bad_category_is_refused_and_nothing_is_written() != 0) return 1;
    std::printf("session template categories: ok\n");
    return 0;
}
