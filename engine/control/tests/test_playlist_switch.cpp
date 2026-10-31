// SPDX-License-Identifier: GPL-3.0-or-later
//
// Choosing another playlist while rolling (ADR-0026 §3): the track plays
// the old playlist, exactly, until the plan built from the new one is
// resident and published, and the new one, exactly, from the first block
// after; whatever the block sizes, so the switch lands on a block edge and
// every frame is one playlist's or the other's. Nothing is allocated or
// freed in Session::process() throughout, and the disk reader never misses.
#define PLAYBACK_TEST_COUNT_HEAP 1
#include "playback_test.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "sonora/model/history.h"
#include "sonora/model/operations.h"
#include "sonora/playback_controller.h"
#include "sonora/session.h"

#define CHECK_APPLY(history, op)                                              \
    do {                                                                      \
        std::string check_apply_error_;                                       \
        if (!(history).apply((op), check_apply_error_)) {                     \
            std::fprintf(stderr, "FAIL %s:%d: apply refused: %s\n", __FILE__, \
                         __LINE__, check_apply_error_.c_str());               \
            return 1;                                                         \
        }                                                                     \
    } while (0)

namespace {

using namespace sonora::model;
using playback_test::source_value;
using sonora::PlaybackController;
using sonora::Session;
using sonora::TransportState;

constexpr uint32_t kChunk = 256;
constexpr uint32_t kMaxBlock = 256;
constexpr int64_t kLength = 160 * kChunk;
const std::string kDir = "test_playlist_switch_session";

constexpr Id kTrack = 100;
constexpr Id kFirst = 101;    // the track's first playlist: source 1
constexpr Id kSecond = 140;   // source 2
constexpr Id kEmpty = 160;

std::unique_ptr<Session> make_session() {
    sonora::SessionConfig cfg;
    cfg.dir = kDir;
    cfg.track_count = 2;
    cfg.ring_seconds = 0.1;
    cfg.playback.chunk_frames = kChunk;
    cfg.playback.window_seconds = 0.02;
    cfg.playback.pool_chunks = 128;
    cfg.playback.max_block_frames = kMaxBlock;
    cfg.playback.max_segments = 64;
    auto s = std::make_unique<Session>(cfg);
    s->create_dirs();
    return s;
}

bool write_source(Id id) {
    return playback_test::write_source(kDir + "/Audio Files/src_" + std::to_string(id) + ".wav",
                                       static_cast<uint32_t>(id), 1, kLength);
}

AddSource source_op(Id id) {
    AddSource op;
    op.source.id = id;
    op.source.path = "Audio Files/src_" + std::to_string(id) + ".wav";
    op.source.length = kLength;
    op.source.format = AudioFormat::Float32;
    op.source.kind = SourceKind::Imported;
    return op;
}

Region region(Id id, Id source) {
    Region r;
    r.id = id;
    r.name = "r" + std::to_string(id);
    r.length = kLength;
    r.channels.push_back({source, 0});
    return r;
}

// Block sizes that do not divide the chunk, nor each other.
struct Blocks {
    std::vector<uint32_t> sizes;
    std::size_t next = 0;
    uint32_t operator()() {
        const uint32_t n = sizes[next];
        next = (next + 1) % sizes.size();
        return n;
    }
};

struct Out {
    std::vector<float> l = std::vector<float>(kMaxBlock);
    std::vector<float> r = std::vector<float>(kMaxBlock);
};

// One block of `n` frames: left is the hard-left track, and must be
// source `source` (0: silence) exactly, frame by frame; right is silent.
bool heard(Session& s, Out& o, uint32_t n, Id source) {
    const int64_t at = static_cast<int64_t>(s.transport().playhead());
    float* outs[2] = {o.l.data(), o.r.data()};
    sonora::backend::AudioBlock b{};
    b.outputs = outs;
    b.output_count = 2;
    b.nframes = n;
    playback_test::t_counting = true;
    s.process(b);
    playback_test::t_counting = false;
    for (uint32_t i = 0; i < n; ++i) {
        const float want =
            source == 0 ? 0.0f : source_value(static_cast<uint32_t>(source), 0, at + i);
        if (o.l[i] != want || o.r[i] != 0.0f) {
            std::fprintf(stderr, "  frame %lld: L %f R %f, want %f from source %llu\n",
                         static_cast<long long>(at + i), o.l[i], o.r[i], want,
                         static_cast<unsigned long long>(source));
            return false;
        }
    }
    return true;
}

bool show(PlaybackController& ctl, const Snapshot& snapshot) {
    std::string error;
    ctl.apply_mix(snapshot);
    if (ctl.show(snapshot, error)) return true;
    std::fprintf(stderr, "  show: %s\n", error.c_str());
    return false;
}

// The document chose another playlist: `before` plays on, exactly, until
// the new plan is published; `after` from the next block on.
bool switch_to(PlaybackController& ctl, Session& s, Out& o, Blocks& blocks,
               const Snapshot& next, Id before, Id after) {
    if (!show(ctl, next)) return false;
    for (int blk = 0; blk < 3; ++blk) {
        if (!heard(s, o, blocks(), before)) return false;
    }
    bool swapped = false;
    for (int i = 0; i < 64 && !swapped; ++i) {
        const uint64_t published = ctl.plans_published();
        s.reader().run_pass();
        ctl.pump();
        swapped = ctl.plans_published() != published;
        if (!heard(s, o, blocks(), swapped ? after : before)) return false;
    }
    if (!swapped) {
        std::fprintf(stderr, "  the new plan was never published\n");
        return false;
    }
    for (int blk = 0; blk < 6; ++blk) {
        s.reader().run_pass();
        ctl.pump();
        if (!heard(s, o, blocks(), after)) return false;
    }
    return true;
}

int run(const std::vector<uint32_t>& sizes) {
    auto session = make_session();
    Session& s = *session;
    CHECK(write_source(1) && write_source(2));
    History h(new_session(SessionSettings{}));
    CHECK_APPLY(h, source_op(1));
    CHECK_APPLY(h, source_op(2));
    AddTrack a;
    a.track = kTrack;
    a.playlist = kFirst;
    a.name = "A";
    a.playlist_name = "A.01";
    a.mix.input = {0};
    a.mix.pan = -1.0f;
    CHECK_APPLY(h, a);
    CHECK_APPLY(h, (PlaceRegion{kFirst, region(110, 1), 0}));
    // Pro Tools' New Playlist, then a region on it, then back to the first:
    // the second exists before the transport rolls, as it does in use.
    CHECK_APPLY(h, (Compound{{AddPlaylist{kTrack, kSecond, "A.02"},
                              SetActivePlaylist{kTrack, kSecond}},
                             "New Playlist"}));
    CHECK_APPLY(h, (PlaceRegion{kSecond, region(150, 2), 0}));
    CHECK_APPLY(h, (AddPlaylist{kTrack, kEmpty, "A.03"}));
    CHECK_APPLY(h, (SetActivePlaylist{kTrack, kFirst}));

    PlaybackController ctl(s);
    CHECK(show(ctl, h.current()));
    CHECK(ctl.settle(2.0));
    Out o;
    Blocks blocks{sizes};
    CHECK(heard(s, o, blocks(), 0));   // stopped: nothing plays
    ctl.play();
    CHECK(ctl.settle(2.0));
    CHECK(s.transport().state() == TransportState::Rolling);
    playback_test::reset_heap_counts();
    for (int blk = 0; blk < 4; ++blk) {
        s.reader().run_pass();
        ctl.pump();
        CHECK(heard(s, o, blocks(), 1));
    }

    CHECK_APPLY(h, (SetActivePlaylist{kTrack, kSecond}));
    CHECK(switch_to(ctl, s, o, blocks, h.current(), 1, 2));
    // Undo is a switch too, back to the first.
    CHECK(h.undo());
    CHECK(switch_to(ctl, s, o, blocks, h.current(), 2, 1));
    CHECK(h.redo());
    CHECK(switch_to(ctl, s, o, blocks, h.current(), 1, 2));
    // An empty playlist is silence from its swap.
    CHECK_APPLY(h, (SetActivePlaylist{kTrack, kEmpty}));
    CHECK(switch_to(ctl, s, o, blocks, h.current(), 2, 0));
    CHECK_APPLY(h, (SetActivePlaylist{kTrack, kFirst}));
    CHECK(switch_to(ctl, s, o, blocks, h.current(), 0, 1));

    CHECK(playback_test::heap_untouched());
    CHECK(s.playback_misses() == 0);
    std::printf("  blocks of %zu sizes from %u: heap counter %s\n", sizes.size(), sizes[0],
                playback_test::heap_counter_scope());
    return 0;
}

int test_switch_is_exact_and_allocates_nothing() { return run({64}); }

// The same switches with blocks that split chunks and each other: the
// frames heard are the same, only the edge between blocks moves.
int test_switch_is_block_split_invariant() {
    if (run({37, 1, 256, 100, 13}) != 0) return 1;
    return run({255, 2, 64, 129});
}

}  // namespace

int main() {
    if (test_switch_is_exact_and_allocates_nothing() != 0) return 1;
    if (test_switch_is_block_split_invariant() != 0) return 1;
    std::printf("test_playlist_switch: ok\n");
    return 0;
}
