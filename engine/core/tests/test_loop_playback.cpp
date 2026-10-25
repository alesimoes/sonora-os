// SPDX-License-Identifier: GPL-3.0-or-later
//
// Loop playback while not recording (engine.transport.loop; ADR-0016 §2
// leaves its details here). With a loop range set, a roll wraps from B to A
// at the exact sample, inside a block; what plays after the wrap is A's
// audio, resident before the playhead gets there, whether the loop is
// shorter than the reader's window or longer than its whole pool. A new
// range reaches the audio thread only once its start is resident, and the
// old one keeps playing until then. A take is one contiguous span: a plain
// take does not wrap, and a loop take's end gives the reader back to the
// playback loop.
//
// The source's frame f is source_value(f), distinct per frame, so a sample
// from the wrong place is a wrong value.
#define PLAYBACK_TEST_COUNT_HEAP 1
#include "playback_test.h"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "sonora/session.h"

namespace {

namespace fs = std::filesystem;
using namespace playback_test;
using sonora::LoopRange;
using sonora::Session;
using sonora::TransportState;

constexpr uint32_t kChunk = 256;
constexpr uint32_t kMaxBlock = 256;
// Prime, so wraps land at every offset in a block rather than on its edges.
constexpr uint32_t kBlock = 97;
constexpr uint32_t kLongest = 2 * kMaxBlock;
constexpr int64_t kSourceFrames = 40 * kChunk;

struct Rig {
    fs::path dir;
    std::unique_ptr<Session> session;
    std::vector<float> in;
    const float* in_ptrs[1];
    std::vector<float> out[2];
    float* out_ptrs[2];
    // The frame the next output sample should be, as the loop rules say.
    uint64_t timeline = 0;
    uint64_t from = 0;
    uint64_t to = 0;

    Rig(const std::string& name, uint32_t pool_chunks) {
        dir = fs::temp_directory_path() / ("sonora_test_loop_playback_" + name);
        fs::remove_all(dir);
        sonora::SessionConfig cfg;
        cfg.dir = dir.string();
        cfg.track_count = 1;
        cfg.format = sonora::WavWriter::Format::Float32;
        cfg.ring_seconds = 1.0;
        cfg.playback.chunk_frames = kChunk;
        // W = 960 + 256 frames.
        cfg.playback.window_seconds = 0.02;
        cfg.playback.pool_chunks = pool_chunks;
        cfg.playback.max_block_frames = kMaxBlock;
        session = std::make_unique<Session>(cfg);
        session->create_dirs();
        in.assign(kLongest, 0.0f);
        in_ptrs[0] = in.data();
        for (int o = 0; o < 2; ++o) {
            out[o].assign(kLongest, 0.0f);
            out_ptrs[o] = out[o].data();
        }
        Session& s = *session;
        s.tracks()[0]->set_rec_armed(false);
        s.tracks()[0]->set_pan(-1.0f);
    }
    ~Rig() {
        session.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    bool load() {
        Session& s = *session;
        const std::string path = (dir / "source.wav").string();
        if (!write_source(path, 5, 1, kSourceFrames)) return false;
        uint32_t slot = 0;
        std::string error;
        if (!s.reader().add_source(path, slot, error)) return false;
        SegmentSpec sp;
        sp.end = kSourceFrames;
        sp.source_slot = slot;
        if (!publish_plan(s, make_plan(s.plans().next_epoch(), 1, {sp}, kChunk))) return false;
        s.reader().run_pass();
        // The faders ramp up from silence in the session's first block.
        for (int b = 0; b < 2; ++b) block();
        return true;
    }

    // Through the residency gate, as engine/control's pump does it.
    bool loop(uint64_t a, uint64_t b) {
        Session& s = *session;
        std::string error;
        if (!s.set_loop(a, b, &error)) {
            std::fprintf(stderr, "  set_loop: %s\n", error.c_str());
            return false;
        }
        for (int k = 0; k < 8 && s.loop_pending(); ++k) {
            s.reader().run_pass();
            s.pump_loop();
        }
        return !s.loop_pending();
    }

    bool locate(uint64_t frame) {
        Session& s = *session;
        if (!s.post_locate(frame)) return false;
        s.reader().run_pass();
        block();
        timeline = frame;
        s.reader().run_pass();
        return s.transport().playhead() == frame;
    }

    void block(uint32_t n = kBlock) {
        sonora::backend::AudioBlock b{};
        b.inputs = in_ptrs;
        b.input_count = 1;
        b.outputs = out_ptrs;
        b.output_count = 2;
        b.nframes = n;
        sonora::backend::ProcessCallback& cb = *session;
        t_counting = true;
        cb.process(b);
        t_counting = false;
    }

    // One rolled block, checked frame by frame against the timeline as a
    // loop [from, to) moves it: it wraps only when the block starts before
    // B. Then a reader pass, as real time would give it.
    bool rolled(const char* what, int b) {
        const bool looping = to > from && timeline < to;
        block();
        for (uint32_t i = 0; i < kBlock; ++i) {
            const float want = source_value(5, 0, static_cast<int64_t>(timeline));
            if (out[0][i] != want) {
                std::fprintf(stderr, "  %s, block %d frame %u: %.6f, want frame %llu's %.6f\n",
                             what, b, i, static_cast<double>(out[0][i]),
                             static_cast<unsigned long long>(timeline),
                             static_cast<double>(want));
                return false;
            }
            if (++timeline == to && looping) timeline = from;
        }
        session->reader().run_pass();
        return true;
    }

    bool roll(const char* what, int blocks) {
        for (int b = 0; b < blocks; ++b) {
            if (!rolled(what, b)) return false;
        }
        return session->transport().playhead() == timeline;
    }
};

// Blocks enough for `wraps` passes of [a, b) from `start`.
int blocks_for(uint64_t start, uint64_t a, uint64_t b, int wraps) {
    return static_cast<int>((b - start + static_cast<uint64_t>(wraps) * (b - a)) / kBlock) + 1;
}

int test_wraps_at_the_sample() {
    Rig rig("sample", 96);
    Session& s = *rig.session;
    CHECK(rig.load());
    CHECK(rig.loop(1000, 1300));
    rig.from = 1000;
    rig.to = 1300;
    CHECK(rig.locate(900));
    reset_heap_counts();
    s.play();
    CHECK(rig.roll("wrap", blocks_for(900, 1000, 1300, 6)));
    CHECK(heap_untouched());
    CHECK(s.playback_misses() == 0);
    s.stop_playback();
    return 0;
}

// The playhead before A rolls into the loop; at or past B it plays on.
int test_past_the_end_plays_on() {
    Rig rig("past_end", 96);
    Session& s = *rig.session;
    CHECK(rig.load());
    CHECK(rig.loop(1000, 1300));
    CHECK(rig.locate(1400));
    s.play();
    rig.from = 1000;
    rig.to = 1300;
    CHECK(rig.roll("past", 20));
    CHECK(s.transport().playhead() == 1400 + 20 * kBlock);
    CHECK(s.playback_misses() == 0);
    return 0;
}

// A loop shorter than the reader's window (300 frames against 1216): the
// window ahead of the playhead and the loop start's are both [A, B), read
// once and never again, and nothing past B is read for it. A window at A
// running on past B would want chunks 3 to 8, which with those at the
// playhead overflow this pool.
int test_shorter_than_the_window() {
    Rig rig("short", 8);
    Session& s = *rig.session;
    CHECK(rig.load());
    CHECK(rig.loop(1000, 1300));
    rig.from = 1000;
    rig.to = 1300;
    CHECK(rig.locate(1000));
    s.play();
    CHECK(rig.roll("first pass", blocks_for(1000, 1000, 1300, 1)));
    const uint64_t read = s.reader().chunks_read();
    CHECK(rig.roll("later passes", blocks_for(1000, 1000, 1300, 12)));
    CHECK(s.reader().chunks_read() == read);
    CHECK(s.reader().cache_full() == 0);
    CHECK(s.playback_misses() == 0);
    return 0;
}

// The PR #117 B1 case, rolling rather than recording: a loop longer than
// the pool holds. A's chunks would be evicted long before B.
int test_longer_than_the_pool() {
    Rig rig("long", 20);
    Session& s = *rig.session;
    CHECK(rig.load());
    CHECK(rig.loop(1000, 7000));
    rig.from = 1000;
    rig.to = 7000;
    CHECK(rig.locate(900));
    s.play();
    CHECK(rig.roll("long", blocks_for(900, 1000, 7000, 3)));
    CHECK(s.playback_misses() == 0);
    return 0;
}

// A new range waits for its start to be resident: with no reader pass the
// audio thread goes on wrapping at the old one, whose start is still held,
// and once a pass has read the new start the next block wraps there.
int test_a_new_range_waits_for_its_start() {
    Rig rig("change", 20);
    Session& s = *rig.session;
    CHECK(rig.load());
    CHECK(rig.loop(1000, 2000));
    rig.from = 1000;
    rig.to = 2000;
    CHECK(rig.locate(1000));
    s.play();
    CHECK(rig.roll("old", 15));
    std::string error;
    CHECK(s.set_loop(6000, 7000, &error));
    CHECK(s.loop_pending());
    // Two old passes with no reader pass and no post: nothing new plays.
    for (int b = 0; b < blocks_for(rig.timeline, 1000, 2000, 2); ++b) {
        s.pump_loop();
        CHECK(s.loop_pending());
        const bool looping = rig.timeline < rig.to;
        rig.block();
        for (uint32_t i = 0; i < kBlock; ++i) {
            CHECK(rig.out[0][i] == source_value(5, 0, static_cast<int64_t>(rig.timeline)));
            if (++rig.timeline == rig.to && looping) rig.timeline = rig.from;
        }
    }
    CHECK(s.playback_misses() == 0);
    s.reader().run_pass();
    s.pump_loop();
    CHECK(!s.loop_pending());
    rig.from = 6000;
    rig.to = 7000;
    // From below A it rolls on into the new range, then wraps there.
    CHECK(rig.roll("new", blocks_for(rig.timeline, 6000, 7000, 3)));
    CHECK(rig.timeline >= 6000 && rig.timeline < 7000);
    CHECK(s.playback_misses() == 0);
    return 0;
}

// The old range plays whole while the new one's start cannot be read: this
// pool holds the old loop and the window ahead, never the new start too, so
// the new range stays pending. Were the old start not held, the reader
// would evict it for the window past the old B, and the next wrap would
// play silence.
int test_the_old_range_plays_whole_meanwhile() {
    Rig rig("old_whole", 8);
    Session& s = *rig.session;
    CHECK(rig.load());
    CHECK(rig.loop(1000, 2000));
    rig.from = 1000;
    rig.to = 2000;
    CHECK(rig.locate(1000));
    s.play();
    CHECK(rig.roll("before", blocks_for(1000, 1000, 2000, 1)));
    std::string error;
    CHECK(s.set_loop(6000, 7000, &error));
    for (int b = 0; b < blocks_for(rig.timeline, 1000, 2000, 3); ++b) {
        s.pump_loop();
        CHECK(s.loop_pending());
        CHECK(rig.rolled("meanwhile", b));
    }
    CHECK(s.playback_misses() == 0);
    // Set back before it was sent: nothing is pending.
    CHECK(s.set_loop(1000, 2000, &error));
    CHECK(!s.loop_pending());
    CHECK(rig.roll("after", blocks_for(rig.timeline, 1000, 2000, 2)));
    CHECK(s.playback_misses() == 0);
    return 0;
}

// A range moved behind the playhead, or cleared, lets the roll play on.
int test_shrink_and_clear_while_rolling() {
    Rig rig("shrink", 96);
    Session& s = *rig.session;
    CHECK(rig.load());
    CHECK(rig.loop(1000, 3000));
    rig.from = 1000;
    rig.to = 3000;
    CHECK(rig.locate(1000));
    s.play();
    CHECK(rig.roll("wide", 16));
    CHECK(rig.timeline > 2000);
    CHECK(rig.loop(1000, 2000));
    rig.to = 2000;
    CHECK(rig.roll("behind", 20));
    CHECK(rig.timeline > 3000);
    s.stop_playback();
    CHECK(rig.locate(1000));
    s.play();
    CHECK(rig.roll("narrow", blocks_for(1000, 1000, 2000, 1)));
    CHECK(rig.loop(0, 0));
    rig.from = 0;
    rig.to = 0;
    CHECK(rig.roll("cleared", 20));
    CHECK(rig.timeline > 2000);
    CHECK(s.playback_misses() == 0);
    return 0;
}

int test_short_loops_are_refused() {
    Rig rig("refused", 96);
    Session& s = *rig.session;
    std::string error;
    CHECK(!s.set_loop(1000, 1000 + kMaxBlock - 1, &error));
    CHECK(!error.empty());
    CHECK(!s.loop_pending());
    CHECK(s.set_loop(1000, 1000 + kMaxBlock, &error));
    return 0;
}

// A take is one contiguous span (timeline-model.md): a plain take with a
// playback loop set records straight through B.
int test_a_plain_take_does_not_wrap() {
    Rig rig("plain", 96);
    Session& s = *rig.session;
    CHECK(rig.load());
    CHECK(rig.loop(1000, 1300));
    CHECK(rig.locate(1000));
    s.tracks()[0]->set_rec_armed(true);
    std::string error;
    CHECK(s.start_recording(s.next_free_take(), &error));
    for (int b = 0; b < 10; ++b) {
        rig.block();
        s.reader().run_pass();
    }
    CHECK(s.transport().playhead() == 1000 + 10 * kBlock);
    CHECK(s.stop(&error));
    return 0;
}

// A loop take holds its own range in the reader and lets it go at stop;
// the playback loop must be held again after, or a loop longer than the
// pool plays silence at the top of every pass.
int test_a_loop_take_gives_the_hold_back() {
    Rig rig("after_take", 20);
    Session& s = *rig.session;
    CHECK(rig.load());
    CHECK(rig.loop(1000, 7000));
    LoopRange l;
    l.from = 1000;
    l.to = 7000;
    l.start = 1000;
    s.tracks()[0]->set_rec_armed(true);
    std::string error;
    CHECK(s.start_loop(s.next_free_take(), l, &error));
    s.reader().run_pass();
    for (int b = 0; b < 4; ++b) {
        rig.block();
        s.reader().run_pass();
    }
    CHECK(s.stop(&error));
    s.tracks()[0]->set_rec_armed(false);
    CHECK(rig.locate(900));
    rig.from = 1000;
    rig.to = 7000;
    s.play();
    CHECK(rig.roll("after", blocks_for(900, 1000, 7000, 2)));
    CHECK(s.playback_misses() == 0);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    struct Test {
        const char* name;
        int (*run)();
    };
    const Test tests[] = {
        {"wraps_at_the_sample", test_wraps_at_the_sample},
        {"past_the_end_plays_on", test_past_the_end_plays_on},
        {"shorter_than_the_window", test_shorter_than_the_window},
        {"longer_than_the_pool", test_longer_than_the_pool},
        {"a_new_range_waits_for_its_start", test_a_new_range_waits_for_its_start},
        {"the_old_range_plays_whole_meanwhile", test_the_old_range_plays_whole_meanwhile},
        {"shrink_and_clear_while_rolling", test_shrink_and_clear_while_rolling},
        {"short_loops_are_refused", test_short_loops_are_refused},
        {"a_plain_take_does_not_wrap", test_a_plain_take_does_not_wrap},
        {"a_loop_take_gives_the_hold_back", test_a_loop_take_gives_the_hold_back},
    };
    for (const Test& t : tests) {
        if (argc > 1 && std::string(argv[1]) != t.name) continue;
        std::printf("%s\n", t.name);
        if (t.run()) return 1;
    }
    std::printf("loop_playback: ok\n");
    return 0;
}
