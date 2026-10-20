// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADR-0014 Unverified 6: what dense MIDI costs the session file and the undo
// window, events being inline (midi.md §4.6). Run by hand; the figures go
// in the M-1 PR. Not a ctest: a shared CI runner's timings are not the
// appliance's.
//
//   measure_midi_session [controller events per second per track]
//
// 1. 18 instrument tracks, 5 minutes at 120 bpm, 8 notes a second and one
//    continuous controller per track: file bytes, DOM values against the
//    4,000,000 cap, parse time.
// 2. 200 note drags in a region of 50,000 controller events: what the
//    copy-on-write sequences charge the undo budget, how many steps it
//    keeps, and the undo window's bytes against the 64 MiB a Snapshot may
//    carry.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "sonora/json/json.h"
#include "sonora/model/history.h"
#include "sonora/model/op_codec.h"
#include "sonora/model/session_file.h"

using namespace sonora::model;
namespace midi = sonora::midi;

namespace {

using Clock = std::chrono::steady_clock;

constexpr Ticks kBeat = kTicksPerQuarter;   // 0.5 s at 120 bpm

std::size_t values(const sonora::json::Value& v) {
    std::size_t n = 1;
    for (const auto& i : v.items()) n += values(i);
    for (const auto& m : v.members()) n += values(m.second);
    return n;
}

std::shared_ptr<MidiSequence> dense(Ticks span, int notes_per_s, int events_per_s) {
    auto s = std::make_shared<MidiSequence>();
    const Ticks second = 2 * kBeat;
    const Ticks note_step = second / notes_per_s;
    for (Ticks t = 0; t < span; t += note_step) {
        midi::Note n;
        n.start = t;
        n.length = note_step / 2;
        n.pitch = static_cast<std::uint8_t>(36 + (t / note_step) % 48);
        n.velocity = 90;
        s->notes.push_back(n);
    }
    const Ticks event_step = second / events_per_s;
    std::uint8_t v = 0;
    for (Ticks t = 0; t < span; t += event_step) {
        s->events.push_back(MidiEvent{t, 0xB0, 1, static_cast<std::uint8_t>(v++ & 0x7F)});
    }
    return s;
}

bool ok(bool good, const std::string& what, const std::string& why) {
    if (!good) std::fprintf(stderr, "%s: %s\n", what.c_str(), why.c_str());
    return good;
}

}  // namespace

int main(int argc, char** argv) {
    const int events_per_s = argc > 1 ? std::atoi(argv[1]) : 100;
    std::string error;

    // --- 1. the session file ---
    SessionSettings st;
    st.name = "Dense";
    History h(new_session(st), std::size_t{1} << 40);
    const Ticks span = 600 * kBeat;   // 5 minutes
    for (int i = 0; i < 18; ++i) {
        const Id track = h.current().next_id;
        AddTrack t;
        t.track = track;
        t.playlist = track + 1;
        t.name = "Inst " + std::to_string(i + 1);
        t.playlist_name = t.name + ".01";
        t.kind = TrackKind::Instrument;
        t.channels = 2;
        if (!ok(h.apply(t, error), "AddTrack", error)) return 1;
        MidiRegion r;
        r.id = track + 2;
        r.name = t.name;
        r.length = span;
        r.data = dense(span, 8, events_per_s);
        if (!ok(h.apply(PlaceMidiRegion{t.playlist, r, 0}, error), "PlaceMidiRegion", error)) {
            return 1;
        }
    }
    std::string file;
    const auto s0 = Clock::now();
    if (!ok(serialize(h.current(), WriteInfo{"measure", 1}, file, error), "serialize", error)) {
        return 1;
    }
    const auto s1 = Clock::now();
    sonora::json::Value root;
    sonora::json::Error perr;
    sonora::json::Limits unlimited;
    unlimited.max_input_bytes = std::size_t{1} << 40;
    unlimited.max_values = std::size_t{1} << 40;
    unlimited.max_dom_bytes = std::size_t{1} << 40;
    if (!ok(sonora::json::parse(file, root, perr, unlimited), "parse", perr.message)) return 1;
    double best = 1e300;
    bool loaded = false;
    std::string refusal;
    for (int i = 0; i < 3; ++i) {
        Loaded l;
        LoadError e;
        const auto t0 = Clock::now();
        loaded = deserialize(file, l, e);
        const auto t1 = Clock::now();
        best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
        if (!loaded) refusal = e.text();
    }
    std::size_t notes = 0, events = 0;
    for (const auto& t : h.current().edit->tracks) {
        for (const auto& p : t->midi_playlists) {
            for (const MidiRegion& r : p->regions) {
                notes += r.data->notes.size();
                events += r.data->events.size();
            }
        }
    }
    std::printf("session: 18 instrument tracks, 5 min, %zu notes, %zu controller events "
                "(%d/s per track)\n",
                notes, events, events_per_s);
    std::printf("  file %.1f MiB (cap 64), %zu DOM values (cap 4,000,000), save %.0f ms\n",
                static_cast<double>(file.size()) / (1 << 20), values(root),
                std::chrono::duration<double, std::milli>(s1 - s0).count());
    std::printf("  load: %s, best of 3 %.0f ms\n",
                loaded ? "opens" : ("refused: " + refusal).c_str(), best);

    // --- 2. the undo window ---
    History u(new_session(st));
    AddTrack t;
    t.track = 1;
    t.playlist = 2;
    t.name = "Inst";
    t.playlist_name = "Inst.01";
    t.kind = TrackKind::Instrument;
    t.channels = 2;
    if (!ok(u.apply(t, error), "AddTrack", error)) return 1;
    MidiRegion r;
    r.id = 3;
    r.name = "Dense";
    r.length = span;
    // 50,000 controller events over the 5 minutes: about 167 a second.
    r.data = dense(span, 8, 167);
    if (!ok(u.apply(PlaceMidiRegion{2, r, 0}, error), "PlaceMidiRegion", error)) return 1;
    const auto data_of = [&u]() -> const MidiSequence& {
        return *u.current().edit->tracks[0]->midi_playlists[0]->regions[0].data;
    };
    const std::size_t region_events = data_of().events.size();
    double drag_ms = 0;
    for (int i = 0; i < 200; ++i) {
        const MidiSequence& now = data_of();
        EditMidi e;
        e.playlist = 2;
        e.region = 3;
        midi::Note moved = now.notes[static_cast<std::size_t>(i)];
        e.remove_notes = {NoteRemoval{static_cast<std::uint32_t>(i), moved}};
        moved.start += kBeat / 8;
        e.add_notes = {moved};
        const auto t0 = Clock::now();
        if (!ok(u.apply(e, error), "EditMidi", error)) return 1;
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        drag_ms = std::max(drag_ms, ms);
    }
    std::string window;
    const auto w0 = Clock::now();
    if (!ok(encode_window(u, window, error), "encode_window", error)) return 1;
    const auto w1 = Clock::now();
    std::printf("undo: 200 note drags in a region of %zu controller events\n", region_events);
    std::printf("  charged %.1f MiB of a %.0f MiB budget, %zu of 202 steps kept, slowest drag "
                "%.1f ms\n",
                static_cast<double>(u.charged_bytes()) / (1 << 20),
                static_cast<double>(u.budget()) / (1 << 20), u.step_count(), drag_ms);
    std::printf("  window %.1f MiB (a Snapshot carries at most 64), encode %.0f ms\n",
                static_cast<double>(window.size()) / (1 << 20),
                std::chrono::duration<double, std::milli>(w1 - w0).count());
    return 0;
}
