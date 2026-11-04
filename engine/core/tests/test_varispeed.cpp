// SPDX-License-Identifier: GPL-3.0-or-later
//
// Varispeed (ADR-0033): a roll at ratio r plays the source at fractional
// positions start + i * r, linearly interpolated, and the playhead advances
// by the source frames consumed, carrying the fraction between blocks.
//
// Source 0 is f / 65536, linear in f, so a linear interpolation reproduces
// the position exactly and a sample from the wrong place is a wrong value.
#define PLAYBACK_TEST_COUNT_HEAP 1
#include "playback_test.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "sonora/session.h"

namespace {

namespace fs = std::filesystem;
using namespace playback_test;
using sonora::Session;

constexpr uint32_t kChunk = 256;
constexpr uint32_t kMaxBlock = 256;
// Prime, so chunks and fractions land at every offset.
constexpr uint32_t kBlock = 97;
constexpr int64_t kSourceFrames = 40 * kChunk;
// Below one source frame's step (1.5e-5), above float rounding.
constexpr double kTol = 2e-6;

struct Rig {
    fs::path dir;
    std::unique_ptr<Session> session;
    std::vector<float> in;
    const float* in_ptrs[1];
    std::vector<float> out[2];
    float* out_ptrs[2];

    explicit Rig(const std::string& name, uint32_t max_block = kMaxBlock) {
        dir = fs::temp_directory_path() / ("sonora_test_varispeed_" + name);
        fs::remove_all(dir);
        sonora::SessionConfig cfg;
        cfg.dir = dir.string();
        cfg.track_count = 1;
        cfg.format = sonora::WavWriter::Format::Float32;
        cfg.ring_seconds = 1.0;
        cfg.playback.chunk_frames = kChunk;
        cfg.playback.window_seconds = 0.1;
        cfg.playback.pool_chunks = 96;
        cfg.playback.max_block_frames = max_block;
        session = std::make_unique<Session>(cfg);
        session->create_dirs();
        in.assign(2 * kMaxBlock, 0.0f);
        in_ptrs[0] = in.data();
        for (int o = 0; o < 2; ++o) {
            out[o].assign(2 * kMaxBlock, 0.0f);
            out_ptrs[o] = out[o].data();
        }
        session->tracks()[0]->set_rec_armed(false);
        session->tracks()[0]->set_pan(-1.0f);
    }
    ~Rig() {
        session.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    bool load() {
        Session& s = *session;
        const std::string path = (dir / "source.wav").string();
        if (!write_source(path, 0, 1, kSourceFrames)) return false;
        uint32_t slot = 0;
        std::string error;
        if (!s.reader().add_source(path, slot, error)) return false;
        SegmentSpec sp;
        sp.end = kSourceFrames;
        sp.source_slot = slot;
        if (!publish_plan(s, make_plan(s.plans().next_epoch(), 1, {sp}, kChunk))) return false;
        s.reader().run_pass();
        for (int b = 0; b < 2; ++b) block();  // the faders ramp up
        return true;
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

    // Rolls `blocks` blocks from `start`, checking every frame against
    // start + k * ratio, and the playhead against the frames consumed.
    bool roll(const char* what, uint64_t start, double ratio, int blocks) {
        Session& s = *session;
        if (!s.post_locate(start)) return false;
        s.reader().run_pass();
        block();
        if (s.transport().playhead() != start) return false;
        s.reader().run_pass();
        s.play();
        uint64_t k = 0;
        for (int b = 0; b < blocks; ++b) {
            block();
            for (uint32_t i = 0; i < kBlock; ++i, ++k) {
                const double want =
                    (static_cast<double>(start) + static_cast<double>(k) * ratio) / 65536.0;
                if (std::fabs(static_cast<double>(out[0][i]) - want) > kTol) {
                    std::fprintf(stderr, "  %s, block %d frame %u: %.7f, want %.7f\n", what, b, i,
                                 static_cast<double>(out[0][i]), want);
                    return false;
                }
            }
            const auto expected = start + static_cast<uint64_t>(std::floor(
                                              static_cast<double>(k) * ratio + 1e-9));
            if (s.transport().playhead() != expected) {
                std::fprintf(stderr, "  %s, block %d: playhead %llu, want %llu\n", what, b,
                             static_cast<unsigned long long>(s.transport().playhead()),
                             static_cast<unsigned long long>(expected));
                return false;
            }
            s.reader().run_pass();
        }
        s.stop_playback();
        return true;
    }
};

int test_faster_and_slower() {
    for (const float r : {1.5f, 0.5f, 2.0f, 0.75f}) {
        Rig rig("speed");
        CHECK(rig.load());
        std::string error;
        CHECK(rig.session->set_varispeed(r, &error));
        reset_heap_counts();
        CHECK(rig.roll("speed", 1000, static_cast<double>(r), 20));
        CHECK(heap_untouched());
        CHECK(rig.session->playback_misses() == 0);
    }
    return 0;
}

// A small max block forces chunks within a block (ratio 2 reads twice the
// frames it writes).
int test_chunks_inside_a_block() {
    Rig rig("chunks", 16);
    CHECK(rig.load());
    CHECK(rig.session->set_varispeed(2.0f));
    CHECK(rig.roll("chunks", 500, 2.0, 12));
    return 0;
}

int test_unity_is_untouched() {
    Rig rig("unity");
    CHECK(rig.load());
    CHECK(rig.session->varispeed() == 1.0f);
    CHECK(rig.roll("unity", 700, 1.0, 10));
    return 0;
}

int test_refusals() {
    Rig rig("refuse");
    Session& s = *rig.session;
    std::string error;
    CHECK(!s.set_varispeed(0.49f, &error));
    CHECK(!error.empty());
    CHECK(!s.set_varispeed(2.01f));
    CHECK(!s.set_varispeed(std::nanf("")));
    CHECK(s.varispeed() == 1.0f);
    CHECK(s.set_varispeed(0.5f));
    CHECK(s.set_varispeed(2.0f));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    struct Test {
        const char* name;
        int (*run)();
    };
    const Test tests[] = {
        {"faster_and_slower", test_faster_and_slower},
        {"chunks_inside_a_block", test_chunks_inside_a_block},
        {"unity_is_untouched", test_unity_is_untouched},
        {"refusals", test_refusals},
    };
    for (const Test& t : tests) {
        if (argc > 1 && std::string(argv[1]) != t.name) continue;
        std::printf("%s\n", t.name);
        if (t.run()) return 1;
    }
    std::printf("varispeed: ok\n");
    return 0;
}
