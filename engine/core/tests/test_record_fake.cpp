// SPDX-License-Identifier: GPL-3.0-or-later
//
// An 18-channel take recorded end to end with no audio server: drivers/fake
// feeds the UMC1820's shape into a real Session, its disk thread writes the
// files, and every file is read back. Each track must hold its own
// channel's tone, sample for sample, at the right length with no overruns
// (ADR-0007 §7, M3). The same with the port naming, listing order and ids
// that have gone wrong before; and an injected xrun, server loss or rate
// change must leave a cleanly closed take holding exactly what reached the
// session.
//
// Files are written as 32-bit float so the comparison is exact: a crossed
// channel, a dropped or repeated block, or a block from after the fault is
// a wrong value, not a near miss.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "sonora/fake_backend.h"
#include "sonora/session.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

namespace fs = std::filesystem;
using sonora::FakeBackend;

constexpr int kTracks = 18;
constexpr uint32_t kRate = 48000;

// The samples of a mono float WAV, or nothing if it is not one.
bool read_float_wav(const std::string& path, std::vector<float>& out) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
        std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        return false;
    }
    std::size_t at = 12;
    while (at + 8 <= bytes.size()) {
        uint32_t size = 0;
        std::memcpy(&size, bytes.data() + at + 4, 4);
        if (std::memcmp(bytes.data() + at, "data", 4) == 0) {
            if (at + 8 + size > bytes.size()) return false;
            out.resize(size / sizeof(float));
            std::memcpy(out.data(), bytes.data() + at + 8, out.size() * sizeof(float));
            return true;
        }
        at += 8 + size + (size & 1);
    }
    return false;
}

struct Take {
    fs::path dir;
    sonora::SessionConfig cfg;

    explicit Take(const std::string& name) {
        dir = fs::temp_directory_path() / ("sonora_test_record_fake_" + name);
        fs::remove_all(dir);
        cfg.dir = dir.string();
        cfg.track_count = kTracks;
        cfg.sample_rate = kRate;
        cfg.format = sonora::WavWriter::Format::Float32;
        cfg.ring_seconds = 4.0;
    }
    ~Take() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::string file(int track) const {
        return (dir / "Audio Files" / ("Audio " + std::to_string(track) + "_01.wav")).string();
    }
};

// Track t's file holds channel t's tone for `frames` samples, starting at
// server frame `first` and jumping by `lost` after sample `gap_at`.
int check_files(const Take& take, uint64_t frames, uint64_t first = 0,
                uint64_t gap_at = UINT64_MAX, uint64_t lost = 0) {
    for (int t = 0; t < kTracks; ++t) {
        std::vector<float> s;
        CHECK(read_float_wav(take.file(t + 1), s));
        if (s.size() != frames) {
            std::fprintf(stderr, "track %d: %zu frames, expected %llu\n", t + 1, s.size(),
                         static_cast<unsigned long long>(frames));
            return 1;
        }
        for (uint64_t n = 0; n < frames; ++n) {
            const uint64_t frame = first + n + (n >= gap_at ? lost : 0);
            if (s[n] != FakeBackend::tone(static_cast<uint32_t>(t), frame, kRate)) {
                std::fprintf(stderr, "track %d, sample %llu: not channel %d's tone\n", t + 1,
                             static_cast<unsigned long long>(n), t);
                return 1;
            }
        }
    }
    return 0;
}

// Opens the fake into a session with every track armed and the take
// started, so the first block lands in the files.
int begin(sonora::Session& s, FakeBackend& audio) {
    CHECK(s.create_dirs());
    std::string error;
    CHECK(audio.open({"sonora", kTracks, 0, kRate, ""}, s, error));
    CHECK(audio.activate(error));
    CHECK(audio.connect_inputs_in_order() == kTracks);
    s.disk().start();
    for (const auto& t : s.tracks()) t->set_rec_armed(true);
    CHECK(s.start_recording(1, &error));
    return 0;
}

uint32_t overruns(sonora::Session& s) {
    uint32_t n = 0;
    for (const auto& t : s.tracks()) n += t->overruns();
    return n;
}

int record_and_check(const std::string& name, FakeBackend::Config cfg) {
    Take take(name);
    sonora::Session s(take.cfg);
    FakeBackend audio(cfg);
    CHECK(begin(s, audio) == 0);
    const uint32_t blocks = 2 * kRate / cfg.quantum;  // two seconds
    CHECK(audio.run_blocks(blocks) == blocks);
    audio.deactivate();
    CHECK(s.stop());
    CHECK(overruns(s) == 0);
    CHECK(audio.xruns() == 0);
    CHECK(audio.unwritten_output_periods() == 0);
    CHECK(check_files(take, uint64_t{blocks} * cfg.quantum) == 0);
    return 0;
}

int test_18_channels_keep_their_identity() {
    return record_and_check("plain", FakeBackend::Config{});
}

// capture_1..capture_18 listed lexicographically (capture_10 before
// capture_2) with ids out of channel order: the order that crossed channels
// silently, every way at once.
int test_identity_survives_naming_order_and_ids() {
    FakeBackend::Config cfg;
    cfg.naming = FakeBackend::Naming::Numbered;
    cfg.list_order = FakeBackend::ListOrder::Lexicographic;
    cfg.ids_out_of_order = true;
    cfg.quantum = 256;
    return record_and_check("injected", cfg);
}

// An xrun is a hole the server reports: the take goes on, the frames lost
// are not in it, and what follows is the audio from after the hole.
int test_xrun_leaves_a_counted_hole() {
    Take take("xrun");
    sonora::Session s(take.cfg);
    FakeBackend audio;
    CHECK(begin(s, audio) == 0);
    CHECK(audio.run_blocks(100) == 100);
    audio.inject_xrun(4096);
    CHECK(audio.run_blocks(100) == 100);
    audio.deactivate();
    CHECK(s.stop());
    CHECK(audio.xruns() == 1);
    CHECK(!audio.fatal());
    CHECK(check_files(take, 200 * 128, 0, 100 * 128, 4096) == 0);
    return 0;
}

// The fatal faults: nothing reaches the session after the fault, and the
// take closes holding exactly the blocks before it.
template <typename Inject>
int fatal_mid_take(const std::string& name, Inject inject, const std::string& reason) {
    Take take(name);
    sonora::Session s(take.cfg);
    FakeBackend audio;
    CHECK(begin(s, audio) == 0);
    CHECK(audio.run_blocks(150) == 150);
    inject(audio);
    CHECK(audio.run_blocks(50) == 0);
    CHECK(audio.fatal());
    CHECK(audio.fatal_reason() == reason);
    audio.deactivate();
    CHECK(s.stop());
    CHECK(overruns(s) == 0);
    CHECK(check_files(take, 150 * 128) == 0);
    return 0;
}

int test_server_loss_closes_the_take() {
    return fatal_mid_take(
        "server_loss", [](FakeBackend& a) { a.inject_server_loss(); },
        "the audio server went away");
}

int test_rate_change_closes_the_take() {
    return fatal_mid_take(
        "rate", [](FakeBackend& a) { a.inject_rate_change(96000); },
        "the audio server changed its sample rate mid-session (48000 -> 96000 Hz)");
}

int test_unplug_closes_the_take() {
    return fatal_mid_take(
        "unplug", [](FakeBackend& a) { a.unplug("UMC1820 Pro"); },
        "capture device UMC1820 Pro went away (UMC1820 Pro:capture_AUX0 is gone)");
}

}  // namespace

int main() {
    int failed = 0;
    failed += test_18_channels_keep_their_identity();
    failed += test_identity_survives_naming_order_and_ids();
    failed += test_xrun_leaves_a_counted_hole();
    failed += test_server_loss_closes_the_take();
    failed += test_rate_change_closes_the_take();
    failed += test_unplug_closes_the_take();
    if (failed == 0) std::printf("record_fake: all tests passed\n");
    return failed == 0 ? 0 : 1;
}
