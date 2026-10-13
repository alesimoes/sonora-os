// SPDX-License-Identifier: GPL-3.0-or-later
//
// The two figures ADR-0009's update "playback on the M4/M7 control server"
// leaves Unverified, measured rather than tested: built with the tests, run
// by hand (not by ctest, since it takes a minute and its figures depend on
// the machine).
//
// 1. Request to transport start, against drivers/fake at real-time pace,
//    18 tracks, the default chunk size and window: from the moment a client
//    sends transport.play (or transport.record) to the state block showing
//    Rolling (or Recording), p50, p99 and max. Also to the Session's own
//    transport, which leaves out the publisher's 10 ms period. Resident: the
//    window at the playhead is already in memory (play again after a stop;
//    record again after a take). Cold: the first request after the server
//    starts, so the plan is not yet published and nothing is resident; the
//    files are in the page cache, as on a session just opened.
// 2. Plan build time on the control thread at 18 tracks x 2000 regions
//    (ADR-0009 Unverified 4), with build_plan() given registered sources,
//    as for every edit after the first.
//
//   measure_playback [DIR]   (DIR: where the 18 source files are written)
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "sonora/closed_take.h"
#include "sonora/engine_control_in_process.h"
#include "sonora/fake_backend.h"
#include "sonora/model/operations.h"
#include "sonora/plan_builder.h"
#include "sonora/session.h"
#include "sonora/wav_writer.h"

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using sonora::api::TransportState;

constexpr int kTracks = 18;
constexpr uint32_t kRate = 48000;
constexpr int64_t kSourceSeconds = 20;

double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

struct Stats {
    std::vector<double> v;
    void print(const char* what) {
        if (v.empty()) {
            std::printf("%-44s no samples\n", what);
            return;
        }
        std::sort(v.begin(), v.end());
        const auto at = [this](double q) {
            return v[std::min(v.size() - 1, static_cast<std::size_t>(q * v.size()))];
        };
        std::printf("%-44s n=%3zu  p50 %7.2f ms  p99 %7.2f ms  max %7.2f ms\n", what, v.size(),
                    at(0.50), at(0.99), v.back());
    }
};

bool write_sources(const fs::path& dir) {
    fs::create_directories(dir / "Audio Files");
    std::vector<float> buf(kRate);
    for (int t = 0; t < kTracks; ++t) {
        const fs::path p = dir / "Audio Files" / ("src_" + std::to_string(t + 1) + ".wav");
        if (fs::exists(p)) continue;
        sonora::WavWriter w;
        if (!w.open(p.string(), kRate, 1, sonora::WavWriter::Format::Float32, 0)) return false;
        for (int64_t s = 0; s < kSourceSeconds; ++s) {
            for (uint32_t f = 0; f < kRate; ++f) {
                buf[f] = 0.1f * static_cast<float>((f + 37 * t) % 97) / 97.0f;
            }
            if (!w.write(buf.data(), buf.size())) return false;
        }
        if (!w.close()) return false;
    }
    return true;
}

// A running engine on drivers/fake at real-time pace, its document every
// source placed at 0, one per track.
struct Rig {
    sonora::Session session;
    sonora::FakeBackend audio;
    std::unique_ptr<sonora::InProcessEngine> engine;
    sonora::api::Inbox inbox;
    bool ok = false;

    static sonora::SessionConfig config(const fs::path& dir) {
        sonora::SessionConfig cfg;
        cfg.dir = dir.string();
        cfg.track_count = kTracks;
        cfg.sample_rate = kRate;
        cfg.ring_seconds = 2.0;
        return cfg;
    }
    static sonora::FakeBackend::Config fake() {
        sonora::FakeBackend::Config c;
        c.mode = sonora::FakeBackend::Mode::Thread;
        return c;
    }

    explicit Rig(const fs::path& dir) : session(config(dir)), audio(fake()) {
        std::string error;
        if (!session.create_dirs() ||
            !audio.open({"sonora", kTracks, 2, kRate, ""}, session, error) ||
            !audio.activate(error) || audio.connect_inputs_in_order() != kTracks) {
            std::fprintf(stderr, "rig: %s\n", error.c_str());
            return;
        }
        session.disk().start();
        session.reader().start();
        sonora::model::Document doc(sonora::engine_document(session));
        std::vector<sonora::model::ReplicationUpdate> placed;
        for (int t = 0; t < kTracks; ++t) {
            if (!sonora::place_file(doc, session.config().dir,
                                    "Audio Files/src_" + std::to_string(t + 1) + ".wav",
                                    static_cast<std::size_t>(t), placed, error)) {
                std::fprintf(stderr, "rig: %s\n", error.c_str());
                return;
            }
        }
        sonora::EngineControlServer::Options opts;
        opts.initial_document = std::make_shared<sonora::model::Snapshot>(doc.current());
        engine = std::make_unique<sonora::InProcessEngine>(session, &audio, opts);
        ok = engine->connect(error);
        if (!ok) std::fprintf(stderr, "rig: %s\n", error.c_str());
    }
    ~Rig() {
        if (engine) engine->shutdown();
        audio.deactivate();
        session.stop();
        if (engine) engine->server().after_deactivate();
        engine.reset();
        session.reader().stop();
    }

    uint64_t send(const char* id) {
        sonora::api::CommandRequest r;
        r.command = id;
        return engine->send(r);
    }
    bool reply_ok(uint64_t seq) {
        for (int i = 0; i < 5000; ++i) {
            engine->poll(inbox);
            for (const auto& r : inbox.replies) {
                if (r.seq == seq) return r.result == sonora::DispatchResult::Ok;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }
    bool run(const char* id) { return reply_ok(send(id)); }
    bool block_shows(TransportState s) {
        for (int i = 0; i < 5000; ++i) {
            engine->poll(inbox);
            if (engine->state().transport().state == s) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }

    // Sends `id` and times it to `want` in the Session's transport and in
    // the state block, polling every 100 us.
    bool time(const char* id, TransportState want, double& session_ms, double& block_ms) {
        const auto t0 = Clock::now();
        send(id);
        session_ms = -1.0;
        while (ms_since(t0) < 5000.0) {
            if (session_ms < 0.0 &&
                static_cast<uint32_t>(session.transport().state()) == static_cast<uint32_t>(want)) {
                session_ms = ms_since(t0);
            }
            if (engine->state().transport().state == want) {
                block_ms = ms_since(t0);
                if (session_ms < 0.0) session_ms = block_ms;
                return true;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        return false;
    }
};

int measure_latency(const fs::path& dir) {
    if (!write_sources(dir)) {
        std::fprintf(stderr, "cannot write the sources under %s\n", dir.string().c_str());
        return 1;
    }
    Stats play_s, play_b, rec_s, rec_b, cold_play_s, cold_play_b, cold_rec_s, cold_rec_b;
    double s = 0.0, b = 0.0;

    {
        Rig rig(dir);
        if (!rig.ok) return 1;
        // Resident play: stop, and play again from where it stopped.
        if (!rig.time("transport.play", TransportState::Rolling, s, b)) return 1;
        for (int i = 0; i < 200; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            if (!rig.run("transport.stop") || !rig.block_shows(TransportState::Stopped)) return 1;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (!rig.time("transport.play", TransportState::Rolling, s, b)) return 1;
            play_s.v.push_back(s);
            play_b.v.push_back(b);
        }
        if (!rig.run("transport.stop") || !rig.block_shows(TransportState::Stopped)) return 1;
        // Resident record: a take, and another right after it. Each one
        // closed is added to the document and replans before the next.
        if (!rig.run("track.arm_all")) return 1;
        for (int i = 0; i < 50; ++i) {
            if (!rig.time("transport.record", TransportState::Recording, s, b)) return 1;
            rec_s.v.push_back(s);
            rec_b.v.push_back(b);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (!rig.run("transport.stop") || !rig.block_shows(TransportState::Stopped)) return 1;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    // Cold: the first request to a server just started.
    for (int i = 0; i < 30; ++i) {
        {
            Rig rig(dir);
            if (!rig.ok) return 1;
            if (!rig.time("transport.play", TransportState::Rolling, s, b)) return 1;
            cold_play_s.v.push_back(s);
            cold_play_b.v.push_back(b);
        }
        {
            Rig rig(dir);
            if (!rig.ok) return 1;
            if (!rig.run("track.arm_all")) return 1;
            if (!rig.time("transport.record", TransportState::Recording, s, b)) return 1;
            cold_rec_s.v.push_back(s);
            cold_rec_b.v.push_back(b);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            if (!rig.run("transport.stop")) return 1;
        }
    }
    std::printf("request to transport start, drivers/fake, %d tracks, chunk 16384,"
                " window 4 s, quantum 128:\n", kTracks);
    play_s.print("play, resident, to Session transport");
    play_b.print("play, resident, to state block");
    rec_s.print("record, resident, to Session transport");
    rec_b.print("record, resident, to state block");
    cold_play_s.print("play, cold, to Session transport");
    cold_play_b.print("play, cold, to state block");
    cold_rec_s.print("record, cold, to Session transport");
    cold_rec_b.print("record, cold, to state block");
    return 0;
}

int measure_plan_build() {
    using namespace sonora::model;
    constexpr int kRegions = 2000;
    constexpr Frames kLength = 24000;   // half a second
    constexpr Frames kStride = 36000;   // with a gap between regions
    SessionSettings settings;
    settings.sample_rate = kRate;
    Snapshot s = new_session(settings);
    std::string error;
    const auto apply_or_fail = [&s, &error](const Operation& op) {
        Snapshot next;
        if (!apply(s, op, next, error)) return false;
        s = std::move(next);
        return true;
    };
    for (int t = 0; t < kTracks; ++t) {
        AddSource src;
        src.source.id = s.next_id;
        src.source.path = "Audio Files/src_" + std::to_string(t + 1) + ".wav";
        src.source.length = kStride * kRegions;
        src.source.format = AudioFormat::Float32;
        src.source.kind = SourceKind::Imported;
        if (!apply_or_fail(src)) break;
        AddTrack tr;
        tr.track = s.next_id;
        tr.playlist = s.next_id + 1;
        tr.name = "Audio " + std::to_string(t + 1);
        tr.playlist_name = tr.name + ".01";
        tr.mix.input.assign(1, static_cast<uint16_t>(t));
        if (!apply_or_fail(tr)) break;
    }
    if (!error.empty()) {
        std::fprintf(stderr, "plan build: %s\n", error.c_str());
        return 1;
    }
    // The playlists filled directly: 36,000 PlaceRegion applies would
    // measure the model, not the builder.
    auto edit = std::make_shared<EditState>(*s.edit);
    Id next = s.next_id;
    for (int t = 0; t < kTracks; ++t) {
        auto track = std::make_shared<TrackEdit>(*edit->tracks[t]);
        auto playlist = std::make_shared<Playlist>(*track->playlists[0]);
        const Id source = s.sources->sources[t].id;
        for (int r = 0; r < kRegions; ++r) {
            Region region;
            region.id = next++;
            region.name = "r";
            region.position = r * kStride;
            region.length = kLength;
            region.source_offset = r * kStride;
            region.channels.push_back({source, 0});
            region.fade_in.length = 64;
            region.fade_out.length = 64;
            playlist->regions.push_back(std::move(region));
        }
        track->playlists[0] = playlist;
        edit->tracks[t] = track;
    }
    s.edit = edit;
    s.next_id = next;

    sonora::BuildLimits limits;
    limits.engine_tracks = kTracks;
    limits.channels_per_track = 1;
    limits.max_segments = 65536;
    limits.max_chunk_slots = 1u << 20;
    limits.chunk_frames = 16384;
    limits.window_frames = 4 * kRate;
    limits.pool_chunks = 4096;
    const sonora::SourceSlot slot = [&s](const Source& src, uint32_t& out, std::string&) {
        out = static_cast<uint32_t>(src.id - s.sources->sources.front().id);
        return true;
    };
    Stats build;
    sonora::PlanReport report;
    for (int i = 0; i < 30; ++i) {
        sonora::PlaybackPlan* plan = nullptr;
        const auto t0 = Clock::now();
        if (!sonora::build_plan(s, limits, slot, static_cast<uint64_t>(i + 1), plan, report,
                                error)) {
            std::fprintf(stderr, "plan build: %s\n", error.c_str());
            return 1;
        }
        build.v.push_back(ms_since(t0));
        sonora::destroy_plan(plan);
    }
    std::printf("plan build, %d tracks x %d regions (%u segments, %u chunk slots):\n", kTracks,
                kRegions, report.segments, report.chunk_slots);
    build.print("build_plan()");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const fs::path dir = argc > 1 ? fs::path(argv[1])
                                  : fs::temp_directory_path() / "sonora_measure_playback";
    if (measure_plan_build()) return 1;
    if (measure_latency(dir)) return 1;
    return 0;
}
