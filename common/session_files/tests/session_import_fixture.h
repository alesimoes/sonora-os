// SPDX-License-Identifier: GPL-3.0-or-later
//
// Two sessions for Import Session Data's tests (ADR-0123), built through
// History as the engine builds them, so each is a session the model accepts.
//
// The source: "Kick" (mono, two regions with a fade and a crossfade, soloed
// at -3 dB), "Gtr" (stereo, an insert with a state, a volume lane, a send to
// "Verb", its output on "Sub"), "Keys" (MIDI); Kick and Gtr in a group; a
// marker. The destination: a track also named "Kick", the Kick take's file
// already in its pool (same umid), and a file named as Gtr's take is.
#pragma once

#include <string>

#include "model_test.h"
#include "sonora/model/history.h"
#include "sonora/model/session_file.h"
#include "sonora/session_files/session_import.h"

namespace import_fixture {

using namespace model_test;

inline const char* kKickUmid = "1111111111111111111111111111111111111111111111111111111111111111";
inline const char* kGtrUmid = "2222222222222222222222222222222222222222222222222222222222222222";
inline const char* kStateSha = "abababababababababababababababababababababababababababababababab";

struct SourceSession {
    Snapshot snapshot;
    Id kick = 0, gtr = 0, keys = 0;
};

#define FIXTURE_APPLY(h, op)                                                      \
    do {                                                                          \
        std::string e_;                                                           \
        if (!(h).apply((op), e_)) {                                               \
            std::fprintf(stderr, "fixture: line %d: %s\n", __LINE__, e_.c_str()); \
            return false;                                                         \
        }                                                                         \
    } while (0)

inline AddSource file(Ids& ids, const std::string& name, std::uint16_t channels,
                      const char* umid) {
    AddSource op;
    op.source.id = ids();
    op.source.path = "Audio Files/" + name;
    op.source.channels = channels;
    op.source.length = 480000;
    op.source.kind = SourceKind::Recorded;
    op.source.take = 1;
    op.source.umid = umid;
    return op;
}

inline bool build_kick(History& h, Ids& ids, SourceSession& out) {
    const AddSource kick_file = file(ids, "Kick_01.wav", 1, kKickUmid);
    const AddSource kick2_file = file(ids, "Kick_02.wav", 1, "");
    FIXTURE_APPLY(h, kick_file);
    FIXTURE_APPLY(h, kick2_file);
    AddTrack kick = add_track(ids, "Kick");
    kick.mix.solo = true;
    kick.mix.gain_db = -3.0f;
    out.kick = kick.track;
    FIXTURE_APPLY(h, kick);
    Region a = region(ids(), kick_file.source.id, 0, 48000);
    a.fade_in.length = 480;
    const Region b = region(ids(), kick2_file.source.id, 48000, 48000, 48000);
    FIXTURE_APPLY(h, (PlaceRegion{kick.playlist, a, 0}));
    FIXTURE_APPLY(h, (PlaceRegion{kick.playlist, b, 0}));
    Crossfade x;
    x.left = a.id;
    x.right = b.id;
    x.length = 960;
    FIXTURE_APPLY(h, (SetCrossfade{kick.playlist, x}));
    return true;
}

inline bool build_gtr(History& h, Ids& ids, SourceSession& out) {
    const AddSource gtr_file = file(ids, "Gtr_01.wav", 2, kGtrUmid);
    FIXTURE_APPLY(h, gtr_file);
    const AddTrack gtr = add_track(ids, "Gtr", 2);
    out.gtr = gtr.track;
    FIXTURE_APPLY(h, gtr);
    FIXTURE_APPLY(h, (PlaceRegion{gtr.playlist,
                                  region(ids(), gtr_file.source.id, 96000, 96000, 0, 2), 0}));
    AddInsert eq;
    eq.track = gtr.track;
    eq.insert = ids();
    eq.slot = 2;
    eq.plugin.id = "urn:sonora:test:eq";
    eq.plugin.name = "Test EQ";
    eq.params = {{"gain", 0.5f}};
    StateRef state;
    state.sha256 = kStateSha;
    state.file = plugin_state_file(kStateSha);
    state.bytes = 10;
    eq.state = state;
    FIXTURE_APPLY(h, eq);
    EditAutomation lane{gtr.track, AutomationParam::Volume, 0, kMaxInteger,
                        {{0, -6.0f}, {48000, 0.0f}}};
    FIXTURE_APPLY(h, lane);
    AddBus verb;
    verb.bus = ids();
    verb.name = "Verb";
    FIXTURE_APPLY(h, verb);
    AddBus sub;
    sub.bus = ids();
    sub.name = "Sub";
    FIXTURE_APPLY(h, sub);
    AddSend send;
    send.strip = gtr.track;
    send.send = ids();
    send.bus = verb.bus;
    FIXTURE_APPLY(h, send);
    FIXTURE_APPLY(h, (SetOutput{gtr.track, sub.bus}));
    return true;
}

inline bool build_source(SourceSession& out) {
    History h(new_session(settings()));
    Ids ids(h.current());
    if (!build_kick(h, ids, out) || !build_gtr(h, ids, out)) return false;
    AddTrack keys = add_track(ids, "Keys", 0);
    keys.kind = TrackKind::Midi;
    keys.mix.input.clear();
    out.keys = keys.track;
    FIXTURE_APPLY(h, keys);
    FIXTURE_APPLY(h, (SetGroup{ids(), "Rhythm", {out.kick, out.gtr}, kGroupVolume}));
    Marker verse;
    verse.id = ids();
    verse.position = 1000;
    verse.name = "Verse";
    FIXTURE_APPLY(h, AddMarker{verse});
    out.snapshot = h.current();
    return true;
}

inline bool build_dest(Snapshot& out) {
    History h(new_session(settings()));
    Ids ids(h.current());
    FIXTURE_APPLY(h, add_track(ids, "Kick"));
    FIXTURE_APPLY(h, file(ids, "Kick_01.wav", 1, kKickUmid));
    FIXTURE_APPLY(h, file(ids, "Gtr_01.wav", 1, ""));
    out = h.current();
    return true;
}

inline sonora::session_files::SessionImportRequest request(std::vector<Id> tracks) {
    sonora::session_files::SessionImportRequest r;
    r.tracks = std::move(tracks);
    r.source_folder = "/sessions/Source";
    r.source_format_version = kFormatVersion;
    return r;
}

}  // namespace import_fixture
