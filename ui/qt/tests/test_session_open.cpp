// SPDX-License-Identifier: GPL-3.0-or-later
//
// File > Open on FakeEngine, wired as main() wires it (#109 review): an Open
// the engine refuses gives the window back to the session it had, so Save
// writes that session's file and never the refused one's, and cleanup never
// runs in the refused session's folder. Both Open refusals (the engine's,
// and the interface's own over the size a Load can carry) are held notices,
// not a status line that clears before the operator has looked.
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "control_adapter.h"
#include "key_router.h"
#include "peak_source.h"
#include "replica_document.h"
#include "session_model.h"
#include "shell_model.h"
#include "sonora/api/fake_engine.h"
#include "sonora/model/session_file.h"
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

constexpr uint64_t kCap = 100;

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
    QTemporaryDir dir;

    static api::testing::FakeEngine::Config config() {
        api::testing::FakeEngine::Config c;
        c.tracks = 4;
        c.midi_max_entries = kCap;
        return c;
    }

    Rig() {
        session.setSessionsDir(dir.path());
        engine.setModelSink([this](const std::vector<api::ModelUpdate>& u,
                                   const std::vector<api::CommandReply>& r) {
            if (doc.receive(u, r)) session.documentUpdated();
        });
        // As main() wires them.
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
        session.installCommands(router.local());
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
    bool save(std::string& why) {
        // Save asks the engine for the plugins' state and writes on its reply.
        if (!session.saveSession(why)) return false;
        settle();
        return !session.saveWaiting();
    }
};

// A session named `name` with one MIDI region of `events` events and
// `sysex` SysEx messages, saved into `dir` as the interface saves it. Its
// session file.
// Unchecked, the region is placed past History's refusal of a chunk too
// large for a part, as a damaged or foreign file could hold one.
QString saved_midi_session(const QString& dir, const std::string& name, std::size_t events,
                           std::size_t sysex = 0, bool checked = true) {
    model::Snapshot base = make_empty_session(4, 48000);
    base.settings.name = name;
    model::History h(std::move(base));
    model::AddTrack t;
    t.track = h.current().next_id;
    t.playlist = t.track + 1;
    t.name = "Synth";
    t.playlist_name = "Synth.01";
    t.kind = model::TrackKind::Midi;
    t.channels = 0;
    model::MidiLists seq;
    for (std::size_t i = 0; i < events; ++i) {
        seq.events.push_back(model::MidiEvent{static_cast<model::Ticks>(i), 0xB0, 1, 2});
    }
    std::vector<std::uint8_t> message(model::kMaxSysexBytes, 0x11);
    message.front() = 0xF0;
    message.back() = 0xF7;
    for (std::size_t i = 0; i < sysex; ++i) {
        seq.sysex.push_back(model::MidiSysEx{static_cast<model::Ticks>(i), message});
    }
    model::MidiRegion r;
    r.id = t.track + 2;
    r.name = "Synth_01";
    r.length = model::kTicksPerQuarter;
    r.data = std::make_shared<const model::MidiSequence>(seq);
    std::string why;
    const model::PlaceMidiRegion place{t.playlist, r, 0, {}};
    if (!h.apply(t, why)) return {};
    model::Snapshot made;
    if (checked ? !h.apply(place, why) : !model::apply(h.current(), place, made, why)) return {};
    ModelDocument doc(checked ? h.current() : made);
    SyntheticPeakSource peaks(48000, 120.0);
    SessionModel s(doc, peaks, true);
    s.setSessionsDir(dir);
    if (!s.saveSession(why)) {
        std::fprintf(stderr, "saving %s: %s\n", name.c_str(), why.c_str());
        return {};
    }
    return s.sessionFile();
}

QByteArray contents(const QString& file) {
    QFile f(file);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// Every file in a session's "MIDI Sequences/", with its bytes.
std::vector<std::pair<QString, QByteArray>> sequences_of(const QString& session_file) {
    std::vector<std::pair<QString, QByteArray>> out;
    const QDir d(QDir(QFileInfo(session_file).absolutePath()).filePath(QStringLiteral("MIDI Sequences")));
    for (const QString& n : d.entryList(QDir::Files, QDir::Name)) {
        out.emplace_back(n, contents(d.filePath(n)));
    }
    return out;
}

bool holds(const QStringList& notices, const char* text) {
    for (const QString& n : notices) {
        if (n.contains(QString::fromUtf8(text))) return true;
    }
    return false;
}

// Open A, open B, the engine refuses B, Save: B's session file and its
// sequence files are as they were, and A's file is the one saved.
int a_refused_open_gives_the_window_back() {
    Rig r;
    CHECK(r.connect());
    std::string why;
    CHECK(r.save(why));
    const QString a = r.session.sessionFile();
    CHECK(!a.isEmpty());
    const QString b = saved_midi_session(r.dir.path(), "Big", kCap + 1);
    CHECK(!b.isEmpty() && b != a);
    const QByteArray b_bytes = contents(b);
    const auto b_sequences = sequences_of(b);
    CHECK(!b_bytes.isEmpty() && b_sequences.size() == 1);

    CHECK(r.session.openSessionFile(a, false, why));
    r.settle();
    CHECK(r.session.sessionFile() == a);
    const std::size_t tracks = r.doc.snapshot().edit->tracks.size();
    CHECK(r.session.openSessionFile(b, false, why));
    CHECK(r.session.sessionFile() == b);   // shown at once, as the engine is asked
    r.settle();

    // Refused: the window names A again, and shows it.
    CHECK(r.doc.in_flight() == 0);
    CHECK(r.doc.snapshot().edit->tracks.size() == tracks);
    CHECK(r.session.sessionFile() == a);
    CHECK(r.session.repairs().isEmpty());
    CHECK(holds(r.shell.notices(), "was not opened"));
    CHECK(holds(r.shell.notices(), "midi_max_entries"));

    // An edit, so Save has something new to write.
    model::AddMarker m;
    m.marker.id = r.doc.reserve_id();
    m.marker.position = 480;
    m.marker.name = "after";
    CHECK(r.doc.submit(m, why));
    r.settle();
    const QByteArray a_before = contents(a);
    CHECK(r.save(why));
    CHECK(r.session.sessionFile() == a);
    CHECK(contents(a) != a_before);
    CHECK(contents(b) == b_bytes);
    CHECK(sequences_of(b) == b_sequences);

    // The next Open takes the held refusal away.
    CHECK(r.session.openSessionFile(a, false, why));
    r.settle();
    CHECK(!holds(r.shell.notices(), "was not opened"));
    return 0;
}

// A session holding one chunk over what a part can carry (a damaged or
// foreign file: apply refuses to make one) is refused as it is read, by
// name, as any file that does not read is, and nothing is shown or sent.
int an_open_with_a_chunk_over_a_part_is_refused() {
    Rig r;
    CHECK(r.connect());
    std::string why;
    CHECK(r.save(why));
    const QString a = r.session.sessionFile();
    const QString big = saved_midi_session(r.dir.path(), "Huge", 0, 4200, false);
    CHECK(!big.isEmpty());
    CHECK(!r.session.openSessionFile(big, false, why));
    CHECK(why.find("one chunk may hold") != std::string::npos);
    CHECK(r.session.sessionFile() == a);
    CHECK(r.doc.in_flight() == 0);
    return 0;
}

// An edit made while an Open is unanswered is refused by name. A request
// carries no session tag, so if the engine refuses the Open, it would apply
// that edit to the session it kept, where the ids usually mean something else.
int an_edit_while_an_open_is_unanswered_is_refused() {
    Rig r;
    CHECK(r.connect());
    std::string why;
    CHECK(r.save(why));
    const QString a = r.session.sessionFile();
    const std::size_t markers = r.doc.snapshot().edit->markers->markers.size();
    const std::size_t tracks = r.doc.snapshot().edit->tracks.size();
    const QString b = saved_midi_session(r.dir.path(), "Big", kCap + 1);
    CHECK(!b.isEmpty());
    CHECK(r.session.openSessionFile(b, false, why));

    model::AddMarker m;
    m.marker.id = r.doc.reserve_id();
    m.marker.position = 480;
    m.marker.name = "during";
    why.clear();
    CHECK(!r.doc.submit(m, why));
    CHECK(why.find("still opening") != std::string::npos);
    why.clear();
    CHECK(!r.doc.undo(why) && why.find("still opening") != std::string::npos);
    why.clear();
    CHECK(!r.doc.redo(why) && why.find("still opening") != std::string::npos);
    CHECK(r.doc.in_flight() == 2);   // the Load and its one sequence file's part

    r.settle();   // the engine refuses B
    CHECK(r.session.sessionFile() == a);
    CHECK(r.doc.in_flight() == 0);
    CHECK(r.doc.snapshot().edit->markers->markers.size() == markers);
    CHECK(r.doc.snapshot().edit->tracks.size() == tracks);

    // Answered, edits go through again.
    m.marker.id = r.doc.reserve_id();
    CHECK(r.doc.submit(m, why));
    r.settle();
    CHECK(r.doc.snapshot().edit->markers->markers.size() == markers + 1);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    if (a_refused_open_gives_the_window_back() != 0) return 1;
    if (an_open_with_a_chunk_over_a_part_is_refused() != 0) return 1;
    if (an_edit_while_an_open_is_unanswered_is_refused() != 0) return 1;
    std::printf("session open: ok\n");
    return 0;
}
