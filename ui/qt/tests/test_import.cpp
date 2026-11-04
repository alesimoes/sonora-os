// SPDX-License-Identifier: GPL-3.0-or-later
//
// Import Audio (ui.editing.import), through the replica on FakeEngine: a
// 44.1 kHz stereo file becomes a stereo track at the session's 48 kHz whose
// BWF copy sits in the session's "Audio Files/", placed as one undo step
// that leaves the copy where it is; the refusals (three channels, not WAV,
// no folder, a take running, the session changed mid-conversion) write
// nothing and say why; a cancelled import leaves no file; the browser model
// lists what it can and cannot import, with the reason.
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QVariantMap>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "control_adapter.h"
#include "import_model.h"
#include "key_router.h"
#include "peak_source.h"
#include "replica_document.h"
#include "session_model.h"
#include "shell_model.h"
#include "sonora/api/fake_engine.h"
#include "sonora/wav_reader.h"
#include "sonora/wav_writer.h"
#include "timeline_document.h"

using namespace sonora;
using namespace sonora::ui;
using namespace sonora::ui::model;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

struct Rig {
    api::testing::FakeEngine fake{config()};
    ControlAdapter engine{fake};
    ReplicaDocument doc{[this](const api::ModelRequest& r) { return engine.submitModel(r); },
                        make_empty_session(6, 48000)};
    SyntheticPeakSource peaks{48000, 120.0};
    SessionModel session{doc, peaks, true};
    KeyRouter router{KeyMap::built_in_default(), api::testing::FakeEngine::engine_commands(),
                     [this](const std::string& c, const CommandContext& ctx) {
                         return engine.send(c, ctx);
                     }};
    ShellModel shell{engine, router};
    QTemporaryDir folder;  // the session folder
    QTemporaryDir source;  // where the files to import are

    static api::testing::FakeEngine::Config config() {
        api::testing::FakeEngine::Config c;
        c.tracks = 6;
        return c;
    }
    Rig() {
        engine.setModelSink([this](const std::vector<api::ModelUpdate>& u,
                                   const std::vector<api::CommandReply>& r) {
            if (doc.receive(u, r)) session.documentUpdated();
        });
        doc.on_refused = [](const std::string& why) {
            std::fprintf(stderr, "engine refused: %s\n", why.c_str());
        };
        session.installCommands(router.local());
        router.prepare({});
        session.setEngineFolder(folder.path());
    }
    bool connect() {
        std::string error;
        if (!engine.connect(error)) return false;
        settle();
        if (!doc.synced()) return false;
        // The fake engine runs with the tracks it was started with and
        // refuses another past them, as the engine does past its inputs:
        // free one so the import has room.
        std::string why;
        for (int i = 0; i < 4; ++i) {
            if (!doc.submit(model::RemoveTrack{doc.snapshot().edit->tracks.back()->id}, why)) return false;
            settle();
        }
        return doc.synced();
    }
    void settle() {
        for (int i = 0; i < 8; ++i) shell.tick();
    }
    ImportModel* model() { return static_cast<ImportModel*>(session.importer()); }
    QString audio(const QString& name) const { return folder.path() + "/Audio Files/" + name; }
};

QString make_wav(const QString& path, uint32_t rate, int64_t frames, uint16_t channels) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    WavWriter w;
    if (!w.open(path.toStdString(), rate, channels, WavWriter::Format::Int24, 0)) return QString();
    std::vector<float> buf(static_cast<std::size_t>(frames) * channels);
    for (int64_t i = 0; i < frames; ++i) {
        for (uint16_t c = 0; c < channels; ++c) {
            buf[static_cast<std::size_t>(i) * channels + c] =
                static_cast<float>(0.4 * std::sin(6.2831853 * 440.0 * (c + 1) * i / rate));
        }
    }
    if (!w.write(buf.data(), static_cast<std::size_t>(frames)) || !w.close()) return QString();
    return path;
}

std::size_t track_count(const Rig& r) { return r.doc.snapshot().edit->tracks.size(); }

int end_to_end() {
    Rig r;
    CHECK(r.connect());
    const std::size_t before = track_count(r);
    const std::size_t sources = r.doc.snapshot().sources->sources.size();
    const QString f = make_wav(r.source.path() + "/Guitar DI.wav", 44100, 44100, 2);
    CHECK(!f.isEmpty());

    ImportModel* m = r.model();
    m->show();
    // Browsing starts on the session's audio, which does not exist yet; the
    // folder it falls back to is the session's.
    CHECK(m->folder() == QDir::cleanPath(r.folder.path()));
    m->open(r.source.path());
    m->toggle(f);
    CHECK(m->selectedCount() == 1);
    int placed = 0;
    QObject::connect(m, &ImportModel::imported, [&](int) { ++placed; });
    CHECK(m->startImport());
    CHECK(m->finishNow());
    CHECK(placed == 1);
    r.settle();

    // One new stereo track, its source an imported 48 kHz BWF in Audio Files.
    CHECK(track_count(r) == before + 1);
    const Snapshot& s = r.doc.snapshot();
    const TrackEdit& t = *s.edit->tracks.back();
    CHECK(t.name == "Guitar DI" && t.channels == 2);
    CHECK(s.sources->sources.size() == sources + 1);
    const Source& src = s.sources->sources.back();
    CHECK(src.path == "Audio Files/Guitar DI.wav" && src.kind == SourceKind::Imported);
    CHECK(src.channels == 2 && src.length == 48000);
    const Region& reg = s.edit->tracks.back()->playlists[0]->regions.front();
    CHECK(reg.position == 0 && reg.length == 48000 && reg.channels.size() == 2);
    CHECK(reg.channels[0].source == src.id && reg.channels[1].channel == 1);
    WavReader rd;
    std::string why;
    CHECK(rd.open(r.audio("Guitar DI.wav").toStdString(), why));
    CHECK(rd.sample_rate() == 48000 && rd.frames() == 48000 && rd.channels() == 2);
    CHECK(QFileInfo(r.source.path() + "/Guitar DI.wav").isFile());  // the original is untouched
    CHECK(!QFileInfo::exists(r.audio("Guitar DI.wav.part")));
    CHECK(r.doc.undo_label() == "Import Audio");

    // One undo takes the track and region back; the copy stays, as a take's does.
    CHECK(r.session.controller().undo(why));
    r.settle();
    CHECK(track_count(r) == before);
    CHECK(QFileInfo(r.audio("Guitar DI.wav")).isFile());
    CHECK(r.session.controller().redo(why));
    r.settle();
    CHECK(track_count(r) == before + 1);

    // The same name again: a second file, not an overwrite.
    m->toggle(f);
    CHECK(m->startImport() && m->finishNow());
    r.settle();
    CHECK(track_count(r) == before + 2);
    CHECK(r.doc.snapshot().edit->tracks.back()->name == "Guitar DI 2");
    CHECK(QFileInfo(r.audio("Guitar DI 2.wav")).isFile());
    return 0;
}

int several_files_at_the_cursor_are_one_step() {
    Rig r;
    CHECK(r.connect());
    const std::size_t before = track_count(r);
    const QString a = make_wav(r.source.path() + "/a/tone.wav", 48000, 4800, 1);
    const QString b = make_wav(r.source.path() + "/b/tone.wav", 96000, 9600, 1);
    CHECK(!a.isEmpty() && !b.isEmpty());
    r.session.controller().set_insertion(24000);
    ImportModel* m = r.model();
    m->show();
    m->setAtInsertion(true);
    m->open(r.source.path() + "/a");
    m->toggle(a);
    m->open(r.source.path() + "/b");
    m->toggle(b);
    CHECK(m->selectedCount() == 2);
    CHECK(m->startImport() && m->finishNow());
    r.settle();
    CHECK(track_count(r) == before + 2);
    const Snapshot& s = r.doc.snapshot();
    CHECK(s.edit->tracks[before]->name == "tone" && s.edit->tracks[before + 1]->name == "tone 2");
    CHECK(s.edit->tracks[before]->playlists[0]->regions.front().position == 24000);
    CHECK(s.edit->tracks[before + 1]->playlists[0]->regions.front().length == 4800);  // 96 -> 48 kHz
    std::string why;
    CHECK(r.session.controller().undo(why));
    r.settle();
    CHECK(track_count(r) == before);  // both, one step
    return 0;
}

int refusals_write_nothing() {
    Rig r;
    CHECK(r.connect());
    const std::size_t before = track_count(r);
    const QString three = make_wav(r.source.path() + "/surround.wav", 48000, 4800, 3);
    const QString good = make_wav(r.source.path() + "/ok.wav", 48000, 4800, 1);
    QFile bad(r.source.path() + "/notes.wav");
    CHECK(bad.open(QIODevice::WriteOnly) && bad.write("not audio at all") > 0);
    bad.close();
    AudioImporter imp(r.session.controller());
    std::string why;
    const std::string folder = r.folder.path().toStdString();

    CHECK(!imp.start({three.toStdString()}, folder, false, why));
    CHECK(why.find("3 channels") != std::string::npos && why.find("surround.wav") != std::string::npos);
    CHECK(!imp.start({bad.fileName().toStdString()}, folder, false, why));
    CHECK(why.find("notes.wav") != std::string::npos);
    // One bad file among good ones refuses the lot.
    CHECK(!imp.start({good.toStdString(), three.toStdString()}, folder, false, why));
    CHECK(!imp.start({good.toStdString()}, "", false, why));
    CHECK(why.find("where this session's audio goes") != std::string::npos);
    CHECK(!imp.start({}, folder, false, why));
    CHECK(imp.state() == AudioImporter::State::Idle);
    CHECK(!QFileInfo::exists(r.audio("ok.wav")));

    // The session moves while the file converts: copy deleted, nothing placed.
    CHECK(imp.start({good.toStdString()}, folder, false, why));
    while (imp.state() == AudioImporter::State::Running) QCoreApplication::processEvents();
    CHECK(imp.state() == AudioImporter::State::Done);
    CHECK(!imp.commit("/somewhere/else", why));
    CHECK(why.find("session changed") != std::string::npos);
    CHECK(!QFileInfo::exists(r.audio("ok.wav")) && !QFileInfo::exists(r.audio("ok.wav.part")));
    CHECK(track_count(r) == before);

    // Cancelled before it ran: no file, no refusal text of the importer's.
    CHECK(imp.start({good.toStdString()}, folder, false, why));
    imp.cancel();
    while (imp.state() == AudioImporter::State::Running) QCoreApplication::processEvents();
    CHECK(imp.state() == AudioImporter::State::Cancelled || imp.state() == AudioImporter::State::Done);
    imp.reset();
    CHECK(!QFileInfo::exists(r.audio("ok.wav")) && !QFileInfo::exists(r.audio("ok.wav.part")));
    CHECK(track_count(r) == before);
    return 0;
}

int the_browser_says_what_it_cannot_import() {
    Rig r;
    CHECK(r.connect());
    make_wav(r.source.path() + "/mono.wav", 44100, 44100, 1);
    make_wav(r.source.path() + "/surround.wav", 48000, 4800, 6);
    QFile other(r.source.path() + "/song.flac");
    CHECK(other.open(QIODevice::WriteOnly) && other.write("fLaC") > 0);
    other.close();
    QFile text(r.source.path() + "/readme.txt");
    CHECK(text.open(QIODevice::WriteOnly));
    text.close();
    QDir().mkpath(r.source.path() + "/Drums");

    ImportModel* m = r.model();
    m->show();
    m->open(r.source.path());
    QVariantMap by_name[8];
    int n = 0;
    QStringList names;
    for (const QVariant& v : m->entries()) {
        by_name[n++] = v.toMap();
        names << v.toMap()["name"].toString();
    }
    CHECK((names == QStringList{"Drums", "mono.wav", "song.flac", "surround.wav"}));  // no readme.txt
    CHECK(by_name[0]["dir"].toBool());
    CHECK(by_name[1]["usable"].toBool());
    CHECK(by_name[1]["detail"].toString().contains("converted to 48 kHz"));
    CHECK(!by_name[2]["usable"].toBool() && by_name[2]["detail"].toString().contains("only WAV"));
    CHECK(!by_name[3]["usable"].toBool() && by_name[3]["detail"].toString().contains("6 channels"));
    // A file that cannot be imported cannot be chosen, and Import with
    // nothing chosen says so, held until dismissed.
    m->toggle(by_name[3]["path"].toString());
    m->toggle(by_name[2]["path"].toString());
    CHECK(m->selectedCount() == 0);
    CHECK(!m->startImport());
    CHECK(m->refusal().contains("choose the files"));
    m->dismissRefusal();
    CHECK(m->refusal().isEmpty());

    // A take is running: refused before anything is written, and held.
    bool recording = true;
    ImportModel::Hooks h;
    h.session_folder = [&] { return r.folder.path(); };
    h.recording = [&] { return recording; };
    ImportModel taking(r.session.controller(), h);
    taking.show();
    taking.open(r.source.path());
    taking.toggle(by_name[1]["path"].toString());
    CHECK(taking.selectedCount() == 1);
    CHECK(!taking.startImport());
    CHECK(taking.refusal().contains("Stop recording"));
    CHECK(!QFileInfo::exists(r.audio("mono.wav")));
    recording = false;
    CHECK(taking.startImport() && taking.finishNow());
    r.settle();
    CHECK(QFileInfo(r.audio("mono.wav")).isFile());
    return 0;
}

int the_command_asks_the_window() {
    Rig r;
    CHECK(r.connect());
    int asked = 0;
    QObject::connect(&r.session, &SessionModel::importBrowserRequested, [&] { ++asked; });
    CHECK(r.router.invoke("session.import_audio", CommandContext{}).result == DispatchResult::Ok);
    CHECK(asked == 1);
    const CommandSpec* spec = find_command("session.import_audio");
    CHECK(spec != nullptr && spec->domain == CommandDomain::Interface);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (end_to_end() != 0) return 1;
    if (several_files_at_the_cursor_are_one_step() != 0) return 1;
    if (refusals_write_nothing() != 0) return 1;
    if (the_browser_says_what_it_cannot_import() != 0) return 1;
    if (the_command_asks_the_window() != 0) return 1;
    std::printf("import: ok\n");
    return 0;
}
