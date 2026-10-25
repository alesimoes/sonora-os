// SPDX-License-Identifier: GPL-3.0-or-later
//
// What dense MIDI costs the session file, its sequence files and the undo
// window (ADR-0014 Unverified 6, ADR-0015 Unverified 2). Run by hand; the
// figures go in the PR. Not a ctest: a shared CI runner's timings are not
// the appliance's.
//
//   measure_midi_session [controller events per second per track]
//   measure_midi_session target [tracks] [folder]
//
// 1. 18 instrument tracks, 5 minutes at 120 bpm, 8 notes a second and one
//    continuous controller per track: the session file's bytes and DOM
//    values against the reader's caps, its sequence files' bytes, and the
//    open.
// 2. 200 note drags in a region of 50,000 controller events: what the
//    copy-on-write sequences charge the undo budget, how many steps it
//    keeps, and the undo window's bytes against the 64 MiB a Snapshot may
//    carry. Unchanged by format 6; the chunks of ADR-0015 §1 change it.
// 3. `target`, in a process of its own: ADR-0015 Unverified 2, the
//    2^24-entry design target. `tracks` (default 64) of 8 notes and 100
//    controller events a second, long enough to reach 2^24 together. First
//    save: encode, hash and write apart, the write being each file through
//    tmp, fdatasync and rename into `folder` (default a fresh directory
//    under the temporary directory) and one folder fsync. Open: read, hash
//    and decode apart, then deserialize as a whole, and the peak resident
//    memory.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "sonora/json/json.h"
#include "sonora/model/history.h"
#include "sonora/model/op_codec.h"
#include "sonora/model/session_file.h"
#include "sonora/sha256.h"

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace sonora::model;
namespace midi = sonora::midi;
namespace fs = std::filesystem;

namespace {

using Clock = std::chrono::steady_clock;

constexpr Ticks kBeat = kTicksPerQuarter;   // 0.5 s at 120 bpm

double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

std::size_t values(const sonora::json::Value& v) {
    std::size_t n = 1;
    for (const auto& i : v.items()) n += values(i);
    for (const auto& m : v.members()) n += values(m.second);
    return n;
}

// `voice` makes each track's sequence its own: identical ones would be one
// file, which the measurement must not count on.
std::shared_ptr<MidiSequence> dense(Ticks span, int notes_per_s, int events_per_s, int voice = 0) {
    auto s = std::make_shared<MidiSequence>();
    const Ticks second = 2 * kBeat;
    const Ticks note_step = second / notes_per_s;
    for (Ticks t = 0; t < span; t += note_step) {
        midi::Note n;
        n.start = t;
        n.length = note_step / 2;
        n.pitch = static_cast<std::uint8_t>(36 + (t / note_step + voice) % 48);
        n.velocity = static_cast<std::uint8_t>(60 + voice % 60);
        s->notes.push_back(n);
    }
    const Ticks event_step = second / events_per_s;
    std::uint8_t v = static_cast<std::uint8_t>(voice);
    for (Ticks t = 0; t < span; t += event_step) {
        s->events.push_back(MidiEvent{t, 0xB0, 1, static_cast<std::uint8_t>(v++ & 0x7F)});
    }
    return s;
}

bool ok(bool good, const std::string& what, const std::string& why) {
    if (!good) std::fprintf(stderr, "%s: %s\n", what.c_str(), why.c_str());
    return good;
}

// The session's sequence files, held in memory as a folder would.
struct MemoryFolder : SequenceSource {
    std::map<std::string, std::string> files;
    bool read(const std::string& file, std::string& bytes, std::string& why) override {
        const auto it = files.find(file);
        if (it == files.end()) {
            why = "the file is not there";
            return false;
        }
        bytes = it->second;
        return true;
    }
};

struct DiskFolder : SequenceSource {
    fs::path dir;
    bool read(const std::string& file, std::string& bytes, std::string& why) override {
        std::ifstream in(dir / file, std::ios::binary);
        if (!in) {
            why = "the file is not there";
            return false;
        }
        std::ostringstream s;
        s << in.rdbuf();
        bytes = s.str();
        return true;
    }
};

// `tracks` instrument tracks, each one region of `span` ticks.
bool build(History& h, int tracks, Ticks span, int notes_per_s, int events_per_s) {
    std::string error;
    for (int i = 0; i < tracks; ++i) {
        const Id track = h.current().next_id;
        AddTrack t;
        t.track = track;
        t.playlist = track + 1;
        t.name = "Inst " + std::to_string(i + 1);
        t.playlist_name = t.name + ".01";
        t.kind = TrackKind::Instrument;
        t.channels = 2;
        if (!ok(h.apply(t, error), "AddTrack", error)) return false;
        MidiRegion r;
        r.id = track + 2;
        r.name = t.name;
        r.length = span;
        r.data = dense(span, notes_per_s, events_per_s, i);
        if (!ok(h.apply(PlaceMidiRegion{t.playlist, r, 0, {}}, error), "PlaceMidiRegion", error)) {
            return false;
        }
    }
    return true;
}

double peak_rss_mib() {
#if defined(__linux__)
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmHWM:", 0) == 0) return std::atof(line.c_str() + 6) / 1024.0;
    }
#endif
    return -1;
}

#if defined(__linux__)
// tmp, fdatasync, rename: what the saver does for each file.
bool write_durably(const fs::path& target, const std::string& bytes) {
    const fs::path tmp = target.string() + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (n <= 0) {
            ::close(fd);
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    const bool synced = ::fdatasync(fd) == 0;
    ::close(fd);
    return synced && ::rename(tmp.c_str(), target.c_str()) == 0;
}

bool sync_dir(const fs::path& dir) {
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) return false;
    const bool synced = ::fsync(fd) == 0;
    ::close(fd);
    return synced;
}
#endif

int session_file_case(int events_per_s) {
    std::string error;
    SessionSettings st;
    st.name = "Dense";
    History h(new_session(st), std::size_t{1} << 40);
    const Ticks span = 600 * kBeat;   // 5 minutes
    if (!build(h, 18, span, 8, events_per_s)) return 1;
    std::string file;
    SequenceDigests digests;
    const auto s0 = Clock::now();
    if (!ok(serialize(h.current(), WriteInfo{"measure", 1}, file, error, &digests), "serialize",
            error)) {
        return 1;
    }
    const double save_ms = ms_since(s0);
    MemoryFolder folder;
    std::size_t sidecar_bytes = 0;
    for (NamedSequence& n : named_sequences(h.current(), digests)) {
        std::string bytes = n.encoded.empty() ? encode_sequence(*n.data) : n.encoded;
        sidecar_bytes += bytes.size();
        folder.files[n.ref.file] = std::move(bytes);
    }
    sonora::json::Value root;
    sonora::json::Error perr;
    if (!ok(sonora::json::parse(file, root, perr), "parse", perr.message)) return 1;
    double best = 1e300;
    bool loaded = false;
    std::string refusal;
    for (int i = 0; i < 3; ++i) {
        Loaded l;
        LoadError e;
        const auto t0 = Clock::now();
        loaded = deserialize(file, l, e, &folder) && l.missing_sequences.empty();
        best = std::min(best, ms_since(t0));
        if (!loaded) refusal = e.text();
    }
    const std::uint64_t entries = state_midi_entries(*h.current().edit);
    std::printf("session: 18 instrument tracks, 5 min, %llu notes and controller events "
                "(%d/s per track)\n",
                static_cast<unsigned long long>(entries), events_per_s);
    std::printf("  session file %.1f KiB, %zu DOM values (caps 64 MiB, 4,000,000); sequence "
                "files %.1f MiB in %zu; serialize %.0f ms\n",
                static_cast<double>(file.size()) / 1024, values(root),
                static_cast<double>(sidecar_bytes) / (1 << 20), folder.files.size(), save_ms);
    std::printf("  open: %s, best of 3 %.0f ms\n",
                loaded ? "opens" : ("refused: " + refusal).c_str(), best);
    return 0;
}

int undo_case() {
    std::string error;
    SessionSettings st;
    st.name = "Dense";
    const Ticks span = 600 * kBeat;
    History u(new_session(st));
    if (!build(u, 1, span, 8, 167)) return 1;   // 50,000 events: about 167 a second
    const Id playlist = u.current().edit->tracks[0]->active_playlist;
    const Id region = u.current().edit->tracks[0]->midi_playlists[0]->regions[0].id;
    const auto data_of = [&u]() -> const MidiSequence& {
        return *u.current().edit->tracks[0]->midi_playlists[0]->regions[0].data;
    };
    const std::size_t region_events = data_of().events.size();
    double drag_ms = 0;
    for (int i = 0; i < 200; ++i) {
        const MidiSequence& now = data_of();
        EditMidi e;
        e.playlist = playlist;
        e.region = region;
        midi::Note moved = now.notes[static_cast<std::size_t>(i)];
        e.remove_notes = {NoteRemoval{static_cast<std::uint32_t>(i), moved}};
        moved.start += kBeat / 8;
        e.add_notes = {moved};
        const auto t0 = Clock::now();
        if (!ok(u.apply(e, error), "EditMidi", error)) return 1;
        drag_ms = std::max(drag_ms, ms_since(t0));
    }
    std::string window;
    const auto w0 = Clock::now();
    if (!ok(encode_window(u, window, error), "encode_window", error)) return 1;
    const double window_ms = ms_since(w0);
    std::printf("undo: 200 note drags in a region of %zu controller events\n", region_events);
    std::printf("  charged %.1f MiB of a %.0f MiB budget, %zu of 202 steps kept, %llu MIDI "
                "entries counted of the %llu budget, slowest drag %.1f ms\n",
                static_cast<double>(u.charged_bytes()) / (1 << 20),
                static_cast<double>(u.budget()) / (1 << 20), u.step_count(),
                static_cast<unsigned long long>(u.midi_entries()),
                static_cast<unsigned long long>(u.midi_max_entries()), drag_ms);
    std::printf("  window %.1f MiB (a Snapshot carries at most 64), encode %.0f ms\n",
                static_cast<double>(window.size()) / (1 << 20), window_ms);
    return 0;
}

int target_case(int tracks, const std::string& where) {
    std::string error;
    SessionSettings st;
    st.name = "Target";
    // 108 entries a second a track: long enough that all of them reach 2^24.
    const double seconds = static_cast<double>(std::uint64_t{1} << 24) / (108.0 * tracks);
    const Ticks span = static_cast<Ticks>(seconds) * 2 * kBeat + 2 * kBeat;
    History h(new_session(st), std::size_t{1} << 40, std::uint64_t{1} << 40);
    const auto b0 = Clock::now();
    if (!build(h, tracks, span, 8, 100)) return 1;
    const double build_ms = ms_since(b0);
    const std::uint64_t entries = state_midi_entries(*h.current().edit);
    std::printf("target: %d instrument tracks of %.1f min, %llu notes and events (2^24 = "
                "16777216), built in %.0f ms, peak RSS so far %.0f MiB\n",
                tracks, seconds / 60, static_cast<unsigned long long>(entries), build_ms,
                peak_rss_mib());

    // First save, its three costs apart.
    std::vector<std::string> encoded;
    const auto e0 = Clock::now();
    for (const auto& t : h.current().edit->tracks) {
        for (const auto& p : t->midi_playlists) {
            for (const MidiRegion& r : p->regions) encoded.push_back(encode_sequence(*r.data));
        }
    }
    const double encode_ms = ms_since(e0);
    std::size_t total = 0;
    for (const std::string& b : encoded) total += b.size();
    std::vector<std::string> names;
    const auto h0 = Clock::now();
    for (const std::string& b : encoded) names.push_back(sonora::sha256_hex(b));
    const double hash_ms = ms_since(h0);
    std::printf("  first save: %.1f MiB of sequence files; encode %.0f ms, hash %.0f ms "
                "(%.0f MiB/s)\n",
                static_cast<double>(total) / (1 << 20), encode_ms, hash_ms,
                static_cast<double>(total) / (1 << 20) / (hash_ms / 1000));
    const fs::path dir = where.empty()
                             ? fs::temp_directory_path() /
                                   ("sonora-measure-" +
                                    std::to_string(Clock::now().time_since_epoch().count()))
                             : fs::path(where);
    const fs::path seqs = dir / "MIDI Sequences";
    fs::create_directories(seqs);
    double write_ms = -1;
#if defined(__linux__)
    const auto w0 = Clock::now();
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        if (!write_durably(seqs / (names[i] + ".msq"), encoded[i])) {
            std::fprintf(stderr, "write %s failed\n", names[i].c_str());
            return 1;
        }
    }
    if (!sync_dir(seqs)) std::fprintf(stderr, "folder fsync failed\n");
    write_ms = ms_since(w0);
#endif
    SequenceDigests digests;
    std::string file;
    const auto j0 = Clock::now();
    if (!ok(serialize(h.current(), WriteInfo{"measure", 1}, file, error, &digests), "serialize",
            error)) {
        return 1;
    }
    const double json_ms = ms_since(j0);
    std::printf("  write %zu files (tmp, fdatasync, rename) and one folder fsync %.0f ms in %s; "
                "the session file %.1f KiB, serialize %.0f ms (hashing again, no memo)\n",
                encoded.size(), write_ms, seqs.string().c_str(),
                static_cast<double>(file.size()) / 1024,
                json_ms);
    encoded.clear();
    encoded.shrink_to_fit();

    // Open, its costs apart, then whole; best of three each, the page cache
    // warm after the first (on the stick it is cold: OS 1).
    DiskFolder disk;
    disk.dir = dir;
    double read_ms = 1e300, check_ms = 1e300, decode_ms = 1e300, open_ms = 1e300;
    bool opened = false;
    std::string refusal;
    for (int pass = 0; pass < 3; ++pass) {
        std::vector<std::string> read_back;
        const auto r0 = Clock::now();
        for (const std::string& n : names) {
            std::string bytes, why;
            if (!disk.read(midi_sequence_file(n), bytes, why)) {
                std::fprintf(stderr, "read %s: %s\n", n.c_str(), why.c_str());
                return 1;
            }
            read_back.push_back(std::move(bytes));
        }
        read_ms = std::min(read_ms, ms_since(r0));
        const auto c0 = Clock::now();
        for (const std::string& b : read_back) {
            if (sonora::sha256_hex(b).empty()) return 1;
        }
        check_ms = std::min(check_ms, ms_since(c0));
        {
            std::vector<MidiSequence> decoded(read_back.size());
            const auto d0 = Clock::now();
            for (std::size_t i = 0; i < read_back.size(); ++i) {
                if (!ok(decode_sequence(read_back[i], decoded[i], error), "decode", error)) return 1;
            }
            decode_ms = std::min(decode_ms, ms_since(d0));
        }
        read_back.clear();
        Loaded l;
        LoadError le;
        const auto o0 = Clock::now();
        opened = deserialize(file, l, le, &disk) && l.missing_sequences.empty();
        open_ms = std::min(open_ms, ms_since(o0));
        if (!opened) refusal = le.text();
    }
    std::printf("  open, apart (best of 3): read %.0f ms, hash %.0f ms, decode %.0f ms\n", read_ms,
                check_ms, decode_ms);
    std::printf("  open, whole (best of 3): %s in %.0f ms; peak RSS %.0f MiB, with the document "
                "that was saved still held\n",
                opened ? "opens" : refusal.c_str(), open_ms, peak_rss_mib());
    if (where.empty()) fs::remove_all(dir);
    return opened ? 0 : 1;
}

}  // namespace

// `target` alone, in its own process, so its peak memory is its own.
int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "target") {
        const int tracks = argc > 2 ? std::atoi(argv[2]) : 64;
        return target_case(tracks, argc > 3 ? argv[3] : "");
    }
    const int events_per_s = argc > 1 ? std::atoi(argv[1]) : 100;
    if (session_file_case(events_per_s) != 0) return 1;
    return undo_case();
}
