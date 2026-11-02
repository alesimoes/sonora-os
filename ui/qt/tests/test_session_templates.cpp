// SPDX-License-Identifier: GPL-3.0-or-later
//
// Session templates in the window (ADR-0030): Save as Template writes the
// session's setup, never its material, into the templates folder with the
// plugin states it names, and the session stays as it was; New from
// Template makes a new session in the sessions folder with its own id, the
// template left as it was; a name in use is refused; a New from Template
// whose Save As fails is held, and its first save moves the session out of
// the template's folder. On FakeEngine, through the replica: the Save As is
// sent only once the engine has taken the Load, and never after a refused
// one.
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QVariantMap>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "control_adapter.h"
#include "engine_saver.h"
#include "key_router.h"
#include "peak_source.h"
#include "replica_document.h"
#include "session_files.h"
#include "session_model.h"
#include "shell_model.h"
#include "sonora/api/fake_engine.h"
#include "sonora/model/session_file.h"
#include "sonora/session_files/session_folder.h"
#include "sonora/sha256.h"
#include "timeline_document.h"

using namespace sonora;
using namespace sonora::ui;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

QByteArray contents(const QString& file) {
    QFile f(file);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

std::map<QString, QByteArray> tree(const QString& dir) {
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

bool read(const QString& file, model::Loaded& out) {
    model::LoadError error;
    const QByteArray b = contents(file);
    if (model::deserialize(std::string_view(b.constData(), static_cast<std::size_t>(b.size())), out, error)) {
        return true;
    }
    std::fprintf(stderr, "  %s: %s\n", qPrintable(file), error.text().c_str());
    return false;
}

bool no_material(const model::Snapshot& s) {
    if (!s.sources->sources.empty() || !s.sources->midi_sources.empty()) return false;
    for (const auto& t : s.edit->tracks) {
        for (const auto& p : t->playlists)
            if (!p->regions.empty()) return false;
    }
    return true;
}

const std::string kBlob = "a compressor's state";

// One track with a take on it and a compressor whose state is kBlob.
model::Snapshot tracked_session(model::Id& insert) {
    model::History h(make_empty_session(1, 48000));
    std::string why;
    const model::TrackEdit& t = *h.current().edit->tracks[0];
    const model::Id track = t.id, playlist = t.active_playlist;
    model::AddSource src;
    src.source.id = h.current().next_id;
    src.source.path = "Audio Files/Audio 1_01.wav";
    src.source.length = 48000;
    src.source.kind = model::SourceKind::Recorded;
    src.source.take = 1;
    src.source.recorded_on_track = track;
    h.apply(src, why);
    model::Region r;
    r.id = h.current().next_id;
    r.name = "Audio 1_01";
    r.length = 48000;
    r.channels.push_back({src.source.id, 0});
    h.apply(model::PlaceRegion{playlist, r, 0}, why);
    model::AddInsert in;
    in.track = track;
    in.insert = insert = h.current().next_id;
    in.plugin = model::PluginRef{model::PluginFormat::Lv2, "urn:test#comp", "Comp", "1"};
    const std::string sha = sha256_hex(kBlob);
    in.state = model::StateRef{model::plugin_state_file(sha), sha, kBlob.size()};
    h.apply(in, why);
    model::SessionSettings st = h.current().settings;
    st.name = "Mix";
    st.session_id = model::new_session_id();
    h.apply(model::SetSettings{st}, why);
    return h.current();
}

// Behind a ModelDocument, the engine's own SessionFolder writes both steps;
// the test moves the engine's folder as its Load and Save As would.
int save_as_template_and_new_from_it() {
    QTemporaryDir sessions_dir, templates_dir;
    CHECK(sessions_dir.isValid() && templates_dir.isValid());
    const QDir sessions(sessions_dir.path()), templates(templates_dir.path());
    model::Id insert = 0;
    const model::Snapshot start = tracked_session(insert);
    ModelDocument doc(start);
    doc.set_saver(testing::engine_saver());
    SyntheticPeakSource peaks(48000, 120.0);
    SessionModel session(doc, peaks, true);
    session.setSessionsDir(sessions.path());
    session.setTemplatesDir(templates.path());
    QStringList held;
    QObject::connect(&session, &SessionModel::heldNotice, [&](const QString& t) { held << t; });

    // The engine's folder, with the state a flush wrote there.
    const QString mix = sessions.filePath(QStringLiteral("Mix"));
    std::string why, caution;
    {
        session_files::SessionFolder f;
        CHECK(f.write_states(mix.toStdString(), {{sha256_hex(kBlob), kBlob}}, why));
    }
    session.setEngineFolder(mix);
    auto folder = std::make_shared<session_files::SessionFolder>();
    bool fail_next = false;
    doc.set_saver_as([&, folder](const std::string& file, uint32_t flags, const std::string& bytes,
                                 const model::Snapshot& document, std::string& w, std::string& c) {
        if (fail_next) {
            fail_next = false;
            w = "the medium is full";
            return false;
        }
        const std::string source = session.engineFolder().toStdString();
        if (!folder->save_as(file, bytes, document, source, (flags & api::kSaveAsNoAudio) == 0, w, c)) {
            return false;
        }
        if ((flags & api::kSaveAsCopy) == 0) {
            session.setEngineFolder(QFileInfo(QString::fromStdString(file)).absolutePath());
        }
        return true;
    });

    // Save as Template: a copy without the audio, into the templates folder.
    CHECK(session.saveSessionAsTemplate(QStringLiteral("Vocal Setup"), false, why));
    const QString tfile = templates.filePath(QStringLiteral("Vocal Setup/Vocal Setup.sonora"));
    const QString tdir = QFileInfo(tfile).absolutePath();
    CHECK(QFileInfo(tfile).isFile());
    CHECK(session.engineFolder() == mix && session.sessionFile().isEmpty());
    CHECK(doc.snapshot() == start);
    const QString state = QString::fromStdString(model::plugin_state_file(sha256_hex(kBlob)));
    CHECK(contents(QDir(tdir).filePath(state)) == QByteArray::fromStdString(kBlob));
    model::Loaded tl;
    CHECK(read(tfile, tl));
    CHECK(no_material(tl.snapshot) && tl.snapshot.settings.session_id.empty());
    CHECK(tl.snapshot.settings.name == "Vocal Setup");
    CHECK(tl.snapshot.mix.find_insert(insert) != nullptr);
    const QVariantList listed = session.templateList();
    CHECK(listed.size() == 1);
    CHECK(listed.value(0).toMap().value(QStringLiteral("file")).toString() == tfile);
    // Not offered by Open.
    CHECK(session.sessionList().isEmpty());
    CHECK(!session.saveSessionAsTemplate(QStringLiteral("Vocal Setup"), false, why));
    CHECK(why.find("already") != std::string::npos);
    CHECK(!session.saveSessionAsTemplate(QStringLiteral("a/b"), false, why));
    const auto template_before = tree(tdir);

    // New from Template: the engine enters the template's folder on the
    // Load, then the Save As takes the session into the sessions folder.
    session.setEngineFolder(tdir);
    CHECK(session.newSessionFromTemplate(tfile, QStringLiteral("Song"), true, why));
    const QString song = sessions.filePath(QStringLiteral("Song/Song.sonora"));
    CHECK(session.sessionFile() == song && QFileInfo(song).isFile());
    CHECK(session.sessionName() == QStringLiteral("Song"));
    CHECK(session.engineFolder() == QFileInfo(song).absolutePath());
    CHECK(!session.dirty());
    CHECK(no_material(doc.snapshot()));
    CHECK(doc.snapshot().settings.session_id != start.settings.session_id);
    CHECK(!doc.snapshot().settings.session_id.empty());
    CHECK(contents(QDir(QFileInfo(song).absolutePath()).filePath(state)) == QByteArray::fromStdString(kBlob));
    model::Loaded sl;
    CHECK(read(song, sl));
    CHECK(sl.snapshot.settings.name == "Song");
    CHECK(sl.snapshot.settings.session_id == doc.snapshot().settings.session_id);
    CHECK(tree(tdir) == template_before);
    CHECK(held.isEmpty());
    CHECK(!session.newSessionFromTemplate(tfile, QStringLiteral("Song"), true, why));
    CHECK(why.find("already") != std::string::npos);

    // The Save As fails: held, the session open unsaved, and its first
    // save goes to the sessions folder, never beside the template.
    session.setEngineFolder(tdir);
    fail_next = true;
    CHECK(session.newSessionFromTemplate(tfile, QStringLiteral("Other"), true, why));
    CHECK(!held.isEmpty() && held.back().contains(QStringLiteral("was not made from the template")));
    CHECK(session.sessionFile().isEmpty() && session.dirty());
    CHECK(!QFileInfo::exists(sessions.filePath(QStringLiteral("Other"))));
    CHECK(session.saveSession(why));
    CHECK(session.sessionFile() == sessions.filePath(QStringLiteral("Other/Other.sonora")));
    CHECK(session.engineFolder() == sessions.filePath(QStringLiteral("Other")));
    CHECK(tree(tdir) == template_before);
    return 0;
}

struct Rig {
    api::testing::FakeEngine fake{config()};
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
    QTemporaryDir sessions, templates;

    static api::testing::FakeEngine::Config config() {
        api::testing::FakeEngine::Config c;
        c.tracks = 4;
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

// Through the replica: the Save As waits for the Load's answer, so it runs
// in the template's folder and never against the session a refused Load
// leaves the engine in.
int the_save_as_waits_for_the_load() {
    Rig r;
    CHECK(r.connect());
    std::string why;
    CHECK(r.session.saveSessionAsTemplate(QStringLiteral("Band"), false, why));
    r.settle();
    const QString tfile = QDir(r.templates.path()).filePath(QStringLiteral("Band/Band.sonora"));
    CHECK(QFileInfo(tfile).isFile());

    const uint32_t saves = r.fake.saves();
    CHECK(r.session.newSessionFromTemplate(tfile, QStringLiteral("Gig"), true, why));
    // Only the Load is out.
    CHECK(r.fake.saves() == saves);
    r.settle();
    const QString gig = QDir(r.sessions.path()).filePath(QStringLiteral("Gig/Gig.sonora"));
    CHECK(r.fake.saves() == saves + 1);
    CHECK(QFileInfo(gig).isFile());
    CHECK(r.fake.folder() == QFileInfo(gig).absolutePath().toStdString());
    CHECK(r.session.sessionFile() == gig);
    CHECK(r.session.engineFolder() == QFileInfo(gig).absolutePath());
    CHECK(r.doc.snapshot().settings.name == "Gig");

    // The template goes before the engine reads it: the Load is refused,
    // and no Save As follows it.
    CHECK(r.session.newSessionFromTemplate(tfile, QStringLiteral("Lost"), true, why));
    CHECK(QFile::remove(tfile));
    r.settle();
    CHECK(r.fake.saves() == saves + 1);
    CHECK(!QFileInfo::exists(QDir(r.sessions.path()).filePath(QStringLiteral("Lost"))));
    CHECK(r.session.sessionFile() == gig);
    CHECK(r.fake.folder() == QFileInfo(gig).absolutePath().toStdString());
    return 0;
}

bool no_take_in(const Rig& r, const QString& folder) {
    for (int32_t take = 0; take < 4; ++take) {
        if (QDir::cleanPath(QString::fromStdString(r.fake.take_folder(take))) == QDir::cleanPath(folder)) {
            return false;
        }
    }
    return true;
}

// #272 review B1: the Save As after New from Template fails, so the engine
// stays in the template's folder. Record and Record Ready are refused,
// held, from a key's command as from the button, and the template's folder
// gains nothing: no take, no "Audio Files/" growth.
int record_is_refused_in_a_template_folder() {
    Rig r;
    CHECK(r.connect());
    std::string why;
    CHECK(r.session.saveSessionAsTemplate(QStringLiteral("Band"), false, why));
    r.settle();
    const QString tfile = QDir(r.templates.path()).filePath(QStringLiteral("Band/Band.sonora"));
    const QString tdir = QFileInfo(tfile).absolutePath();
    CHECK(QFileInfo(tfile).isFile());

    r.fake.script_save_failure("the medium is full");
    CHECK(r.session.newSessionFromTemplate(tfile, QStringLiteral("Gig"), true, why));
    r.settle();
    r.fake.script_save_failure("");
    CHECK(QDir::cleanPath(QString::fromStdString(r.fake.folder())) == QDir::cleanPath(tdir));
    CHECK(r.session.sessionFile().isEmpty());
    const auto before = tree(tdir);

    r.shell.invoke(QStringLiteral("transport.record"), -1);
    r.settle();
    CHECK(r.shell.recordRefusal().contains(QStringLiteral("template")));
    const RouteResult ready = r.router.invoke("transport.record_ready", CommandContext{});
    CHECK(ready.kind != RouteResult::Kind::Sent && ready.message.find("template") != std::string::npos);
    r.settle();
    CHECK(no_take_in(r, tdir));
    CHECK(tree(tdir) == before);

    // Saved out of the template's folder: Record goes to the engine again.
    CHECK(r.session.saveSession(why));
    r.settle();
    CHECK(!r.session.sessionFile().isEmpty());
    CHECK(r.session.commandRefusal("transport.record").empty());
    CHECK(tree(tdir) == before);
    return 0;
}

// The round trip between the Load of the template's folder and the Save As
// out of it: Record pressed there is refused too, before it is sent, so
// the engine never starts a take that would then refuse the Save As.
int record_is_refused_during_the_round_trip() {
    Rig r;
    CHECK(r.connect());
    std::string why;
    CHECK(r.session.saveSessionAsTemplate(QStringLiteral("Band"), false, why));
    r.settle();
    const QString tfile = QDir(r.templates.path()).filePath(QStringLiteral("Band/Band.sonora"));
    const QString tdir = QFileInfo(tfile).absolutePath();
    const auto before = tree(tdir);

    CHECK(r.session.newSessionFromTemplate(tfile, QStringLiteral("Gig"), true, why));
    r.shell.invoke(QStringLiteral("transport.record"), -1);
    CHECK(r.shell.recordRefusal().contains(QStringLiteral("template")));
    r.settle();
    const QString gig = QDir(r.sessions.path()).filePath(QStringLiteral("Gig/Gig.sonora"));
    CHECK(r.session.sessionFile() == gig);
    CHECK(no_take_in(r, tdir));
    CHECK(tree(tdir) == before);
    CHECK(r.session.commandRefusal("transport.record").empty());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    if (save_as_template_and_new_from_it() != 0) return 1;
    if (the_save_as_waits_for_the_load() != 0) return 1;
    if (record_is_refused_in_a_template_folder() != 0) return 1;
    if (record_is_refused_during_the_round_trip() != 0) return 1;
    std::printf("session templates: ok\n");
    return 0;
}
