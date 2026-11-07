// SPDX-License-Identifier: GPL-3.0-or-later
//
// Setup > I/O Setup and File > New Session.
//
// I/O Setup: the names of the hardware ports, kept for the machine and
// refused rather than rewritten; a track's input picked by them as SetMix;
// each refusal (no engine, past the last input, a stereo track at the last
// input, a MIDI track, an armed track while a take records) held in the
// window and cleared when another session opens; a track on an input the
// interface does not have says so.
//
// New Session, on FakeEngine wired as main() wires it: refused over
// unsaved edits, for a bad or taken name; written to <dir>/<name>/ only
// once the engine has answered its Load; and the dialog lights only the
// rate and depth this system runs at.
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QVariantMap>
#include <cstdio>
#include <string>
#include <vector>

#include "control_adapter.h"
#include "io_names.h"
#include "io_setup_model.h"
#include "key_router.h"
#include "peak_source.h"
#include "replica_document.h"
#include "session_model.h"
#include "shell_model.h"
#include "sonora/api/fake_engine.h"
#include "sonora/model/operations.h"
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

// --- the names ----------------------------------------------------------------

int names_default_refuse_and_round_trip() {
    IoNames n;
    CHECK(n.name(IoSide::Input, 0) == "In 1");
    CHECK(n.name(IoSide::Output, 1) == "Out 2");
    CHECK(n.problem(IoSide::Input, 0, 18, "Kick In").empty());
    n.set(IoSide::Input, 0, "Kick In");
    CHECK(n.renamed(IoSide::Input, 0));
    // Two inputs reading the same would be one entry twice in the picker.
    CHECK(!n.problem(IoSide::Input, 1, 18, "Kick In").empty());
    CHECK(n.problem(IoSide::Input, 1, 18, "In 1").empty());   // input 1 is "Kick In" now
    CHECK(!n.problem(IoSide::Input, 2, 18, "In 2").empty());            // input 2's default
    // The same name on the other side is not a clash.
    CHECK(n.problem(IoSide::Output, 0, 2, "Kick In").empty());
    CHECK(!n.problem(IoSide::Input, 1, 18, "").empty());
    CHECK(!n.problem(IoSide::Input, 1, 18, "   ").empty());
    CHECK(!n.problem(IoSide::Input, 1, 18, std::string(IoNames::kMaxLength + 1, 'x')).empty());
    CHECK(!n.problem(IoSide::Input, 1, 18, "a\tb").empty());
    CHECK(!n.problem(IoSide::Input, 18, 18, "Vox").empty());   // there is no input 19
    // Its default given back is no name kept.
    n.set(IoSide::Output, 1, "Mon R");
    n.set(IoSide::Output, 1, "Out 2");
    CHECK(!n.renamed(IoSide::Output, 1));

    n.set(IoSide::Input, 17, "Talkback");
    n.set(IoSide::Output, 0, "Mon L");
    const IoNames back = IoNames::parse(n.serialize());
    CHECK(back == n);
    // A line that does not parse loses itself and not the rest.
    const IoNames rough = IoNames::parse("in\t3\tSnare\nnonsense\nin\tx\tBad\nout\t0\tNo\nin\t4\tHat\r\n");
    CHECK(rough.name(IoSide::Input, 2) == "Snare");
    CHECK(rough.name(IoSide::Input, 3) == "Hat");
    CHECK(!rough.renamed(IoSide::Output, 0));

    QTemporaryDir dir;
    const std::string path = io_names_path(dir.path().toStdString());
    CHECK(path.find("sonora") != std::string::npos);
    CHECK(load_io_names(path) == IoNames());   // nothing kept yet
    std::string why;
    CHECK(save_io_names(path, n, why));
    CHECK(load_io_names(path) == n);
    return 0;
}

// --- the window, on a document of its own --------------------------------------

struct Local {
    ModelDocument doc{make_empty_session(4, 48000)};
    SyntheticPeakSource peaks{48000, 120.0};
    SessionModel session{doc, peaks, true};
    IoSetupModel io{doc, session};
    int inputs = 4;
    int outputs = 2;
    bool recording = false;
    std::vector<bool> armed = std::vector<bool>(8, false);
    QTemporaryDir dir;

    Local() {
        io.setEngine([this]() { return inputs; }, [this]() { return outputs; },
                     [this]() { return recording; },
                     [this](int i) { return i >= 0 && i < static_cast<int>(armed.size()) && armed[i]; });
        io.setNamesFile(io_names_path(dir.path().toStdString()));
    }
    std::vector<std::uint16_t> input_of(int row) const {
        const model::Snapshot& s = doc.snapshot();
        return s.mix.find(s.edit->tracks[static_cast<std::size_t>(row)]->id)->input;
    }
    bool add(model::TrackKind kind, std::uint16_t channels, std::vector<std::uint16_t> input) {
        model::AddTrack t;
        t.track = doc.reserve_id();
        t.playlist = doc.reserve_id();
        t.name = kind == model::TrackKind::Midi ? "Synth" : "Keys";
        t.playlist_name = t.name + ".01";
        t.kind = kind;
        t.channels = channels;
        t.mix.input = std::move(input);
        std::string why;
        const bool ok = doc.submit(t, why);
        if (!ok) std::fprintf(stderr, "add: %s\n", why.c_str());
        session.documentUpdated();
        return ok;
    }
};

int names_are_listed_and_kept_for_the_machine() {
    Local l;
    CHECK(l.io.known());
    CHECK(l.io.inputs().size() == 4);
    CHECK(l.io.outputs().size() == 2);
    CHECK(l.io.inputs()[0].toMap()[QStringLiteral("name")].toString() == QStringLiteral("In 1"));
    CHECK(l.io.renameInput(0, QStringLiteral("  Kick In ")));
    CHECK(l.io.inputs()[0].toMap()[QStringLiteral("name")].toString() == QStringLiteral("Kick In"));
    // The track on it reads by the name.
    CHECK(l.io.tracks()[0].toMap()[QStringLiteral("inputText")].toString() == QStringLiteral("Kick In"));
    // A clash is refused and held, and the name is not changed.
    CHECK(!l.io.renameInput(1, QStringLiteral("Kick In")));
    CHECK(l.io.refusal().contains(QStringLiteral("already called")));
    CHECK(l.io.inputs()[1].toMap()[QStringLiteral("name")].toString() == QStringLiteral("In 2"));
    // The next action clears it.
    CHECK(l.io.renameOutput(0, QStringLiteral("Mon L")));
    CHECK(l.io.refusal().isEmpty());
    // Kept: another window on this machine reads them back.
    const IoNames kept = load_io_names(io_names_path(l.dir.path().toStdString()));
    CHECK(kept.name(IoSide::Input, 0) == "Kick In");
    CHECK(kept.name(IoSide::Output, 0) == "Mon L");
    // An empty name gives the default back.
    CHECK(l.io.renameInput(0, QString()));
    CHECK(!load_io_names(io_names_path(l.dir.path().toStdString())).renamed(IoSide::Input, 0));
    return 0;
}

int a_pick_sets_the_track_input() {
    Local l;
    CHECK(l.input_of(1) == std::vector<std::uint16_t>{1});
    CHECK(l.io.setTrackInput(1, 3));
    CHECK(l.input_of(1) == std::vector<std::uint16_t>{3});
    CHECK(l.io.tracks()[1].toMap()[QStringLiteral("input")].toInt() == 3);
    CHECK(l.io.tracks()[1].toMap()[QStringLiteral("connected")].toBool());
    // Mix state, saved and never undone (owner question O5).
    CHECK(!l.doc.can_undo());

    // A stereo track takes the input picked and the one after it, and
    // cannot start at the last.
    CHECK(l.add(model::TrackKind::Audio, 2, {0, 1}));
    const int keys = 4;
    CHECK(l.io.setTrackInput(keys, 2));
    CHECK((l.input_of(keys) == std::vector<std::uint16_t>{2, 3}));
    CHECK(l.io.tracks()[keys].toMap()[QStringLiteral("inputText")].toString() ==
          QStringLiteral("In 3 + In 4"));
    CHECK(!l.io.setTrackInput(keys, 3));
    CHECK(l.io.refusal().contains(QStringLiteral("has only 0 after it")));
    CHECK((l.input_of(keys) == std::vector<std::uint16_t>{2, 3}));
    return 0;
}

int a_pick_that_would_lie_is_refused_and_held() {
    Local l;
    // Past the last input.
    CHECK(!l.io.setTrackInput(0, 4));
    CHECK(l.io.refusal().contains(QStringLiteral("there is no input 5")));
    CHECK(l.input_of(0) == std::vector<std::uint16_t>{0});
    // A MIDI track records no audio input.
    CHECK(l.add(model::TrackKind::Midi, 0, {}));
    CHECK(!l.io.setTrackInput(4, 0));
    CHECK(l.io.refusal().contains(QStringLiteral("MIDI track")));
    CHECK(l.io.tracks()[4].toMap()[QStringLiteral("input")].toInt() == -1);
    // The engine keeps an armed track's input for its take.
    l.recording = true;
    l.armed[2] = true;
    CHECK(!l.io.setTrackInput(2, 0));
    CHECK(l.io.refusal().contains(QStringLiteral("change its input after the take")));
    CHECK(l.input_of(2) == std::vector<std::uint16_t>{2});
    CHECK(l.io.setTrackInput(3, 0));   // not armed: it is not recording
    l.recording = false;
    CHECK(l.io.setTrackInput(2, 0));
    // No engine: which inputs exist is not known, and nothing is listed.
    l.inputs = -1;
    l.io.refresh();
    CHECK(!l.io.known());
    CHECK(l.io.inputs().isEmpty());
    CHECK(!l.io.setTrackInput(1, 0));
    CHECK(l.io.refusal().contains(QStringLiteral("not answering")));
    CHECK(l.io.tracks()[1].toMap()[QStringLiteral("note")].toString().contains(QStringLiteral("not known")));
    // Held until another session opens.
    l.inputs = 4;
    emit l.session.sessionOpened();
    CHECK(l.io.refusal().isEmpty());
    return 0;
}

int an_input_the_interface_lacks_says_so() {
    Local l;
    l.inputs = 2;   // input 3 unplugged, or a session from a bigger interface
    l.io.refresh();
    const QVariantMap row = l.io.tracks()[2].toMap();
    CHECK(!row[QStringLiteral("connected")].toBool());
    CHECK(row[QStringLiteral("note")].toString().contains(QStringLiteral("not connected")));
    CHECK(l.io.tracks()[1].toMap()[QStringLiteral("connected")].toBool());
    return 0;
}

// ADR-0060: an output path is an aux bus on a pair of the opened outputs,
// made, moved, named and removed as undoable document edits.
int output_paths_are_made_moved_named_and_removed() {
    Local l;
    l.outputs = 10;
    l.io.refresh();
    CHECK(l.io.pairs().size() == 5);
    CHECK(l.io.pairs()[0].toMap()[QStringLiteral("by")].toString() == QStringLiteral("Master"));
    CHECK(l.io.paths().isEmpty());

    CHECK(l.io.addPath(QStringLiteral("Vocal Cue"), 2));
    CHECK(l.io.paths().size() == 1);
    QVariantMap p = l.io.paths()[0].toMap();
    CHECK(p[QStringLiteral("name")].toString() == QStringLiteral("Vocal Cue"));
    CHECK(p[QStringLiteral("pair")].toInt() == 2);
    CHECK(p[QStringLiteral("connected")].toBool());
    CHECK(p[QStringLiteral("pairText")].toString() == QStringLiteral("Out 3 + Out 4"));
    CHECK(l.io.pairs()[1].toMap()[QStringLiteral("by")].toString() == QStringLiteral("Vocal Cue"));
    // Made in one step, so one undo takes the bus and its pair back.
    CHECK(l.doc.can_undo());
    CHECK(l.doc.snapshot().edit->buses[0].hardware == 2);

    // A strip routed to it plays there.
    const model::Id bus = l.doc.snapshot().edit->buses[0].id;
    const model::Id track = l.doc.snapshot().edit->tracks[0]->id;
    std::string why;
    CHECK(l.doc.submit(model::SetOutput{track, bus}, why));
    l.io.refresh();
    CHECK(l.io.paths()[0].toMap()[QStringLiteral("strips")].toInt() == 1);

    // Refused and held, with nothing changed: the Master's pair, a taken
    // pair, a pair the interface lacks, one that starts on an even output.
    CHECK(!l.io.addPath(QStringLiteral("Mon"), 0));
    CHECK(l.io.refusal().contains(QStringLiteral("the Master plays on outputs 1 and 2")));
    CHECK(!l.io.addPath(QStringLiteral("Again"), 2));
    CHECK(l.io.refusal().contains(QStringLiteral("already carry the path")));
    CHECK(!l.io.addPath(QStringLiteral("Far"), 10));
    CHECK(l.io.refusal().contains(QStringLiteral("there is no pair of outputs")));
    CHECK(!l.io.addPath(QStringLiteral("Odd"), 3));
    CHECK(l.doc.snapshot().edit->buses.size() == 1);

    // Moved, named; a bus of the same name is refused.
    CHECK(l.io.setPathPair(0, 4));
    CHECK(l.io.paths()[0].toMap()[QStringLiteral("pair")].toInt() == 4);
    CHECK(l.io.renamePath(0, QStringLiteral("  Wedge ")));
    CHECK(l.io.paths()[0].toMap()[QStringLiteral("name")].toString() == QStringLiteral("Wedge"));
    CHECK(!l.io.renamePath(0, QStringLiteral("   ")));
    CHECK(l.io.addPath(QString(), 6));
    CHECK(l.io.paths()[1].toMap()[QStringLiteral("name")].toString() == QStringLiteral("Out 7-8"));
    CHECK(!l.io.renamePath(1, QStringLiteral("Wedge")));
    CHECK(l.io.refusal().contains(QStringLiteral("already called")));

    // The interface has fewer outputs than the session's path needs, or
    // does not answer: said in the row, and nothing new is made.
    l.outputs = 4;
    l.io.refresh();
    CHECK(!l.io.paths()[0].toMap()[QStringLiteral("connected")].toBool());
    CHECK(l.io.paths()[0].toMap()[QStringLiteral("note")].toString().contains(QStringLiteral("not connected")));
    l.outputs = 0;
    l.io.refresh();
    CHECK(!l.io.addPath(QStringLiteral("Late"), 2));
    CHECK(l.io.refusal().contains(QStringLiteral("not answering")));
    l.outputs = 10;
    l.io.refresh();

    // Removed: the strip goes back to the Master in the same step.
    CHECK(l.io.removePath(1));
    CHECK(l.io.removePath(0));
    CHECK(l.io.paths().isEmpty());
    CHECK(l.doc.snapshot().edit->tracks[0]->output == model::kMasterOutput);
    CHECK(l.doc.snapshot().edit->buses.empty());
    return 0;
}

// --- New Session, on FakeEngine ------------------------------------------------

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
        return c;
    }

    Rig() {
        session.setSessionsDir(dir.path());
        engine.setModelSink([this](const std::vector<api::ModelUpdate>& u,
                                   const std::vector<api::CommandReply>& r) {
            if (doc.receive(u, r)) session.documentUpdated();
        });
        doc.on_load_abandoned = [this](const std::string& refused, bool later_load) {
            if (refused.empty()) {
                session.openWasLost();
            } else {
                session.openWasRefused(QString::fromStdString(refused), later_load);
            }
        };
        shell.setTransportSink([this](const TransportSnapshot& t, bool alive) {
            session.onTransport(t, alive);
        });
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
};

int a_new_session_is_written_once_the_engine_has_it() {
    Rig r;
    CHECK(r.connect());
    const QString file = QDir(r.dir.path()).filePath(QStringLiteral("Song/Song.sonora"));
    CHECK(r.session.newSession(QStringLiteral("Song"), false).isEmpty());
    // Shown at once, with no file yet: the Load is not answered. Nothing on
    // disk holds it, so the title reads it as unsaved until the Save As.
    CHECK(r.doc.opening());
    CHECK(r.session.dirty());
    CHECK(r.session.sessionName() == QStringLiteral("Song · edited"));
    CHECK(!QFileInfo::exists(file));
    r.settle();
    CHECK(!r.doc.opening());
    CHECK(QFileInfo::exists(file));
    CHECK(r.session.sessionFile() == file);
    CHECK(!r.session.dirty());
    CHECK(r.doc.snapshot().edit->tracks.size() == 4);
    // The name is taken now.
    const QString again = r.session.newSession(QStringLiteral("Song"), false);
    CHECK(again.contains(QStringLiteral("already in")));
    return 0;
}

int a_new_session_is_refused_where_open_is() {
    Rig r;
    CHECK(r.connect());
    CHECK(r.session.newSession(QStringLiteral("a/b"), false).contains(QStringLiteral("cannot make")));
    // Unsaved edits are not thrown away unasked.
    r.session.setGain(0, -6.0);
    r.settle();
    CHECK(r.session.dirty());
    CHECK(r.session.newSession(QStringLiteral("Next"), false).contains(QStringLiteral("unsaved edits")));
    CHECK(r.session.sessionName() != QStringLiteral("Next"));
    CHECK(r.session.newSession(QStringLiteral("Next"), true).isEmpty());
    r.settle();
    CHECK(QFileInfo::exists(QDir(r.dir.path()).filePath(QStringLiteral("Next/Next.sonora"))));
    return 0;
}

int the_dialog_offers_only_what_this_system_runs_at() {
    Rig r;
    CHECK(r.connect());
    const QVariantMap c = r.session.newSessionChoices();
    CHECK(c[QStringLiteral("where")].toString() == r.dir.path());
    CHECK(c[QStringLiteral("name")].toString() == QStringLiteral("Untitled"));
    int lit = 0;
    for (const QVariant& v : c[QStringLiteral("rates")].toList()) {
        const QVariantMap m = v.toMap();
        if (m[QStringLiteral("on")].toBool()) {
            ++lit;
            CHECK(m[QStringLiteral("text")].toString() == QStringLiteral("48 kHz"));
        } else {
            CHECK(!m[QStringLiteral("why")].toString().isEmpty());
        }
    }
    CHECK(lit == 1);
    const QVariantList depths = c[QStringLiteral("depths")].toList();
    CHECK(depths.size() == 3);
    CHECK(!depths[0].toMap()[QStringLiteral("on")].toBool());   // 16-bit
    CHECK(depths[1].toMap()[QStringLiteral("on")].toBool());    // 24-bit, the fake's
    CHECK(!depths[2].toMap()[QStringLiteral("on")].toBool());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    if (names_default_refuse_and_round_trip() != 0) return 1;
    if (names_are_listed_and_kept_for_the_machine() != 0) return 1;
    if (a_pick_sets_the_track_input() != 0) return 1;
    if (a_pick_that_would_lie_is_refused_and_held() != 0) return 1;
    if (an_input_the_interface_lacks_says_so() != 0) return 1;
    if (output_paths_are_made_moved_named_and_removed() != 0) return 1;
    if (a_new_session_is_written_once_the_engine_has_it() != 0) return 1;
    if (a_new_session_is_refused_where_open_is() != 0) return 1;
    if (the_dialog_offers_only_what_this_system_runs_at() != 0) return 1;
    std::printf("io setup and new session: ok\n");
    return 0;
}
