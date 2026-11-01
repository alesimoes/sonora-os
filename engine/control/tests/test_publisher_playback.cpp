// SPDX-License-Identifier: GPL-3.0-or-later
//
// State block layout 5 from a session that plays (ADR-0009 update of
// 2026-11-01): the publisher takes the monitor bus's peaks, each side on its
// own, copies the playback miss count, and says "Locating…" while the
// controller holds a locate back for its window.
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "sonora/api/state_block.h"
#include "sonora/api/state_view.h"
#include "sonora/model/history.h"
#include "sonora/model/operations.h"
#include "sonora/playback_controller.h"
#include "sonora/session.h"
#include "sonora/state_publisher.h"
#include "sonora/wav_writer.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

using namespace sonora::model;
using sonora::PlaybackController;
using sonora::Session;
using sonora::StatePublisher;
namespace api = sonora::api;

constexpr uint32_t kChunk = 256;
constexpr uint32_t kBlock = 64;
const std::string kDir = "test_publisher_playback_session";

bool write_source(Id id, int64_t frames) {
    const std::string path = kDir + "/Audio Files/src_" + std::to_string(id) + ".wav";
    std::remove(path.c_str());
    sonora::WavWriter w;
    if (!w.open(path, 48000, 1, sonora::WavWriter::Format::Float32, 0)) return false;
    // Above 0.5 everywhere, so any block played from it peaks there.
    std::vector<float> buf(static_cast<std::size_t>(frames), 0.75f);
    return w.write(buf.data(), buf.size()) && w.close();
}

void run_block(Session& s) {
    std::vector<float> l(kBlock), r(kBlock);
    float* outs[2] = {l.data(), r.data()};
    sonora::backend::AudioBlock b{};
    b.outputs = outs;
    b.output_count = 2;
    b.nframes = kBlock;
    s.process(b);
}

int test_layout_5_from_a_playing_session() {
    std::error_code ec;
    std::filesystem::remove_all(kDir, ec);
    sonora::SessionConfig cfg;
    cfg.dir = kDir;
    cfg.track_count = 1;
    cfg.ring_seconds = 0.1;
    cfg.playback.chunk_frames = kChunk;
    cfg.playback.window_seconds = 0.02;
    cfg.playback.pool_chunks = 128;
    cfg.playback.max_block_frames = 256;
    cfg.playback.max_segments = 64;
    Session s(cfg);
    s.create_dirs();
    CHECK(write_source(1, 40 * kChunk));

    History h(new_session(SessionSettings{}));
    std::string error;
    AddSource src;
    src.source.id = 1;
    src.source.path = "Audio Files/src_1.wav";
    src.source.length = 40 * kChunk;
    src.source.format = AudioFormat::Float32;
    src.source.kind = SourceKind::Imported;
    CHECK(h.apply(src, error));
    AddTrack track;
    track.track = 100;
    track.playlist = 101;
    track.name = "A";
    track.playlist_name = "A.01";
    track.mix.input.push_back(0);
    track.mix.pan = -1.0f;   // hard left: the right side of the bus stays silent
    CHECK(h.apply(track, error));
    Region r;
    r.id = 210;
    r.name = "r";
    r.position = 0;
    r.length = 40 * kChunk;
    r.channels.push_back({1, 0});
    CHECK(h.apply(PlaceRegion{101, r, 0}, error));

    PlaybackController ctl(s);
    ctl.apply_mix(h.current());
    CHECK(ctl.show(h.current(), error));
    CHECK(ctl.settle(2.0));

    api::StateBlockMemory mem(1);
    api::StateWriter w;
    CHECK(w.init(mem.data(), mem.bytes(), 1, 1, 10'000'000));
    StatePublisher p(s, nullptr, w, 10'000'000);
    api::StateView a, b;
    CHECK(a.attach(mem.data(), mem.bytes(), error));
    CHECK(b.attach(mem.data(), mem.bytes(), error));

    run_block(s);
    ctl.play();
    CHECK(ctl.settle(2.0));
    run_block(s);
    run_block(s);
    p.publish_once();
    const float left = a.take_bus_peak(0);
    CHECK(left > 0.5f);
    CHECK(a.take_bus_peak(1) == 0.0f);
    CHECK(b.take_bus_peak(0) == left);   // A's read took nothing from B
    // The publisher took it from the session: nothing is left to take twice.
    CHECK(s.take_bus_peak(0) == 0.0f);
    p.publish_once();
    CHECK(a.take_bus_peak(0) == 0.0f);   // no block since
    CHECK(a.transport().playback_misses == 0);

    // A locate sent past the gate lands where nothing is resident: the
    // blocks there are silence, and every one is counted.
    CHECK(s.post_locate(30 * kChunk));
    run_block(s);
    run_block(s);
    CHECK(s.playback_misses() > 0);
    p.publish_once();
    CHECK(a.transport().playback_misses == s.playback_misses());

    // The controller holds a locate back until its window is resident.
    CHECK(ctl.locate(10 * kChunk, error));
    CHECK(ctl.locating());
    p.set_locating(ctl.locating());
    p.publish_once();
    CHECK((a.transport().transport_flags & api::kTransportLocating) != 0);
    s.reader().run_pass();
    ctl.pump();
    CHECK(!ctl.locating());
    p.set_locating(ctl.locating());
    p.publish_once();
    CHECK((a.transport().transport_flags & api::kTransportLocating) == 0);

    ctl.stop();
    run_block(s);
    std::filesystem::remove_all(kDir, ec);
    return 0;
}

}  // namespace

int main() {
    if (test_layout_5_from_a_playing_session() != 0) return 1;
    std::printf("publisher playback: ok\n");
    return 0;
}
