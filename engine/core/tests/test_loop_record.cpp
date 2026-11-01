// SPDX-License-Identifier: GPL-3.0-or-later
//
// Loop record (ADR-0016): one file per armed track through every wrap, each
// wrap marked in band by a Wrap boundary record at the block offset where
// the playhead wraps, its `at` shifted by the input latency (C7), and
// persisted by the disk thread to the take's wrap list at each checkpoint
// (C1). A wrap whose record is lost to a full boundary ring keeps its place
// in the list (C2); past the pass cap the wraps stay on disk only (O2). The
// loop range is refused when shorter than the handle or one block (C4).
//
// Each input sample is its track's capture count, so a file is right
// exactly when sample i is i, whatever the playhead did.
#define PLAYBACK_TEST_COUNT_HEAP 1
#include "playback_test.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "sonora/loop_take.h"
#include "sonora/session.h"
#include "sonora/wav_reader.h"
#include "sonora/wrap_sidecar.h"

// The word hook is private to Session; only a test defines this.
namespace sonora {
struct SessionTestAccess {
    static void set_word_hook(Session& s, Session::WordHook hook, void* context) {
        s.word_hook_ = hook;
        s.word_hook_context_ = context;
    }
};
}  // namespace sonora

namespace {

namespace fs = std::filesystem;
using namespace playback_test;
using sonora::LoopRange;
using sonora::Session;
using sonora::TransportState;
using sonora::WrapSidecar;

constexpr uint32_t kChunk = 256;
constexpr uint32_t kBlock = 64;
constexpr uint32_t kMaxBlock = 256;
// A callback longer than the mix's block, which a backend may still deliver.
constexpr uint32_t kLongest = 2 * kMaxBlock;

struct Rig {
    fs::path dir;
    std::unique_ptr<Session> session;
    std::vector<std::vector<float>> in;
    std::vector<const float*> in_ptrs;
    std::vector<float> out[2];
    float* out_ptrs[2];
    uint64_t count = 0;   // samples each track's input has produced

    Rig(const std::string& name, int tracks, uint32_t boundary_records = 0,
        uint32_t pass_cap = 99, uint32_t pool_chunks = 96) {
        dir = fs::temp_directory_path() / ("sonora_test_loop_record_" + name);
        fs::remove_all(dir);
        sonora::SessionConfig cfg;
        cfg.dir = dir.string();
        cfg.track_count = tracks;
        cfg.format = sonora::WavWriter::Format::Float32;
        cfg.ring_seconds = 1.0;
        cfg.boundary_records = boundary_records;
        cfg.loop_pass_cap = pass_cap;
        cfg.playback.chunk_frames = kChunk;
        cfg.playback.window_seconds = 0.02;
        cfg.playback.pool_chunks = pool_chunks;
        cfg.playback.max_block_frames = kMaxBlock;
        session = std::make_unique<Session>(cfg);
        session->create_dirs();
        in.assign(tracks, std::vector<float>(kLongest, 0.0f));
        for (auto& v : in) in_ptrs.push_back(v.data());
        for (auto& t : session->tracks()) t->set_rec_armed(true);
        for (int o = 0; o < 2; ++o) {
            out[o].assign(kLongest, 0.0f);
            out_ptrs[o] = out[o].data();
        }
    }
    ~Rig() {
        session.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    void block(uint32_t n = kBlock) {
        for (auto& v : in) {
            for (uint32_t i = 0; i < n; ++i) v[i] = static_cast<float>(count + i);
        }
        count += n;
        sonora::backend::AudioBlock b{};
        b.inputs = in_ptrs.data();
        b.input_count = static_cast<uint32_t>(in_ptrs.size());
        b.outputs = out_ptrs;
        b.output_count = 2;
        b.nframes = n;
        sonora::backend::ProcessCallback& cb = *session;
        t_counting = true;
        cb.process(b);
        t_counting = false;
    }

    std::string file(int track, int take) const {
        char name[64];
        std::snprintf(name, sizeof name, "Audio %d_%02d.wav", track + 1, take);
        return (dir / "Audio Files" / name).string();
    }
};

LoopRange loop_of(uint64_t from, uint64_t to, uint64_t pre_roll, uint32_t latency = 0) {
    LoopRange l;
    l.from = from;
    l.to = to;
    l.start = from > pre_roll ? from - pre_roll : 0;
    l.latency = latency;
    return l;
}

// Sample i of the file is first + i: nothing lost, repeated or reordered.
bool file_is_counted(const std::string& path, int64_t frames, uint64_t time_reference,
                     const std::string& loop_text, uint64_t first = 0) {
    sonora::WavReader r;
    std::string error;
    if (!r.open(path, error)) {
        std::fprintf(stderr, "  %s: %s\n", path.c_str(), error.c_str());
        return false;
    }
    if (r.frames() != frames || r.time_reference() != time_reference) {
        std::fprintf(stderr, "  %s: %lld frames at %llu\n", path.c_str(),
                     static_cast<long long>(r.frames()),
                     static_cast<unsigned long long>(r.time_reference()));
        return false;
    }
    if (r.description().find(loop_text) == std::string::npos) {
        std::fprintf(stderr, "  %s: description \"%s\"\n", path.c_str(), r.description().c_str());
        return false;
    }
    std::vector<float> s(static_cast<std::size_t>(frames));
    if (frames > 0 && r.read(0, frames, s.data()) != frames) return false;
    for (int64_t i = 0; i < frames; ++i) {
        if (s[static_cast<std::size_t>(i)] != static_cast<float>(first + static_cast<uint64_t>(i))) {
            std::fprintf(stderr, "  %s: sample %lld is %.0f\n", path.c_str(),
                         static_cast<long long>(i), static_cast<double>(s[static_cast<std::size_t>(i)]));
            return false;
        }
    }
    return true;
}

// The wraps a loop take of `blocks` blocks has, from its start: the first at
// B - start, each later one B - A after it, every one plus L.
std::vector<uint64_t> expected_wraps(const LoopRange& l, uint64_t frames) {
    std::vector<uint64_t> w;
    for (uint64_t at = l.to - l.start; at <= frames; at += l.to - l.from) {
        w.push_back(at + l.latency);
    }
    return w;
}

Session::ClosedTake only_take(Session& s) {
    std::vector<Session::ClosedTake> closed = s.closed_takes();
    return closed.size() == 1 ? closed.front() : Session::ClosedTake{};
}

int wraps_case(const char* name, const LoopRange& l, int blocks, uint32_t n = kBlock) {
    Rig rig(name, 2);
    Session& s = *rig.session;
    const int take = s.next_free_take();
    std::string error;
    CHECK(s.start_loop(take, l, &error));
    reset_heap_counts();
    rig.block(n);
    CHECK(s.take_origin() == l.start);
    uint64_t timeline = l.start + n;
    while (timeline >= l.to) timeline -= l.to - l.from;
    for (int b = 1; b < blocks; ++b) {
        rig.block(n);
        timeline += n;
        while (timeline >= l.to) timeline -= l.to - l.from;
        // The playhead never leaves the loop once it is in it.
        CHECK(s.transport().playhead() == timeline);
    }
    CHECK(heap_untouched());
    CHECK(s.stop(&error));
    const uint64_t frames = static_cast<uint64_t>(blocks) * n;
    const std::vector<uint64_t> want = expected_wraps(l, frames);
    const Session::ClosedTake c = only_take(s);
    CHECK(c.take == take);
    CHECK(c.looped && !c.punched);
    CHECK(c.loop.from == l.from && c.loop.to == l.to && c.loop.latency == l.latency);
    CHECK(c.loop_wraps == want.size());
    CHECK(c.files.size() == 2);
    const std::string loop_text =
        "loop=" + std::to_string(l.from) + "-" + std::to_string(l.to);
    for (const auto& f : c.files) {
        CHECK(f.frames == frames);
        CHECK(f.wraps == want);
        CHECK(f.wraps_past_cap == 0);
        CHECK(file_is_counted(f.path, static_cast<int64_t>(frames), l.start, loop_text));
        sonora::WrapList list;
        CHECK(sonora::read_wrap_sidecar(WrapSidecar::path_for(f.path), list, error));
        CHECK(list.loop_from == l.from && list.loop_to == l.to);
        CHECK(list.wraps == want);
    }
    CHECK(s.boundary_overruns() == 0);
    std::printf("  %s: %zu wraps; heap counter: %s\n", name, want.size(), heap_counter_scope());
    return 0;
}

// Wraps inside a block, at offsets 1 and kBlock - 1 and in between, three
// passes or more each, with and without input latency.
int test_wraps_are_marked_in_band() {
    if (wraps_case("inside", loop_of(1000, 1300, 100), 30)) return 1;
    if (wraps_case("offset_1", loop_of(1000, 1000 + 4 * kBlock + 1, 3 * kBlock), 40)) return 1;
    if (wraps_case("offset_last", loop_of(1000, 1000 + 5 * kBlock - 1, kBlock), 40)) return 1;
    if (wraps_case("latency", loop_of(2000, 2300, 64, 37), 30)) return 1;
    return 0;
}

// B lands on a block's end: the wrap is marked there, and the next block
// starts at A.
int test_wrap_on_a_block_edge() {
    return wraps_case("edge", loop_of(4 * kBlock, 9 * kBlock, 2 * kBlock), 32);
}

// Pre-roll 0, or a start already at A: the take starts on A, and the first
// pass is still whole.
int test_no_pre_roll() { return wraps_case("no_pre_roll", loop_of(640, 960, 0), 24); }

// The take starts at A - pre-roll wherever the playhead was, rolling or not.
int test_locates_to_the_pre_roll() {
    Rig rig("locate", 1);
    Session& s = *rig.session;
    s.play();
    for (int b = 0; b < 5; ++b) rig.block();
    const LoopRange l = loop_of(1000, 1300, 200);
    std::string error;
    const int take = s.next_free_take();
    CHECK(s.start_loop(take, l, &error));
    rig.block();
    CHECK(s.take_origin() == l.start);
    for (int b = 0; b < 9; ++b) rig.block();
    CHECK(s.stop(&error));
    const Session::ClosedTake c = only_take(s);
    CHECK(c.files.size() == 1);
    CHECK(c.files[0].wraps == expected_wraps(l, 10 * kBlock));
    CHECK(file_is_counted(c.files[0].path, 10 * kBlock, l.start, "loop=1000-1300", 5 * kBlock));
    return 0;
}

// C4: shorter than the handle or one block, empty, or started after A, is
// refused with no file made; and a loop take never starts over a take.
int test_short_loops_are_refused() {
    Rig rig("refused", 1);
    Session& s = *rig.session;
    const int take = s.next_free_take();
    const auto refused = [&](const LoopRange& l, const char* says) {
        std::string error;
        if (s.start_loop(take, l, &error)) return false;
        if (error.find(says) == std::string::npos) {
            std::fprintf(stderr, "  refusal: %s\n", error.c_str());
            return false;
        }
        return s.transport().state() == TransportState::Stopped;
    };
    CHECK(refused(loop_of(1000, 1000, 0), "empty"));
    CHECK(refused(loop_of(1000, 1000 + kMaxBlock - 1, 0), "one block"));
    LoopRange handle = loop_of(1000, 1000 + 400, 0);
    handle.handle = 401;
    CHECK(refused(handle, "handle"));
    LoopRange late = loop_of(1000, 1400, 0);
    late.start = 1001;
    CHECK(refused(late, "pre-roll"));
    std::error_code ec;
    CHECK(fs::is_empty(rig.dir / "Audio Files", ec));

    std::string error;
    CHECK(s.start_recording(take, &error));
    rig.block();
    CHECK(!s.start_loop(take + 1, loop_of(1000, 1400, 0), &error));
    CHECK(error.find("during") != std::string::npos);
    CHECK(s.stop(&error));
    // Exactly one block long, and exactly the handle, are long enough.
    LoopRange tight = loop_of(5000, 5000 + kMaxBlock, 0);
    tight.handle = kMaxBlock;
    CHECK(s.start_loop(take + 2, tight, &error));
    for (int b = 0; b < 12; ++b) rig.block();
    CHECK(s.stop(&error));
    return 0;
}

// C1: a wrap is in the list on disk once the take checkpoints, while the
// take still records.
int test_wraps_reach_disk_at_the_checkpoint() {
    Rig rig("checkpoint", 1);
    Session& s = *rig.session;
    const LoopRange l = loop_of(1000, 1300, 100);
    std::string error;
    const int take = s.next_free_take();
    CHECK(s.start_loop(take, l, &error));
    for (int b = 0; b < 12; ++b) rig.block();
    s.disk().checkpoint_all();
    sonora::WrapList list;
    CHECK(sonora::read_wrap_sidecar(WrapSidecar::path_for(rig.file(0, take)), list, error));
    CHECK(list.wraps == expected_wraps(l, 12 * kBlock));
    CHECK(s.transport().state() == TransportState::Recording);
    CHECK(s.stop(&error));
    return 0;
}

// C2: a Wrap record the full boundary ring refused leaves its place in the
// list marked lost, and the next wrap is still the next one; the audio
// thread's own count says how many wraps the take had.
int test_lost_wrap_keeps_its_place() {
    // The ring holds seven records: the four opens stay in it, undrained,
    // so the first wrap's four records find room for three.
    Rig rig("lost", 4, 8);
    Session& s = *rig.session;
    const LoopRange l = loop_of(1000, 1300, 100);
    std::string error;
    const int take = s.next_free_take();
    CHECK(s.start_loop(take, l, &error));
    // The first wrap lands in block 6 (start 900, B 1300 at file frame 400).
    for (int b = 0; b < 7; ++b) rig.block();
    CHECK(s.boundary_overruns() == 1);
    for (int b = 7; b < 12; ++b) {
        s.disk().drain_once();
        rig.block();
    }
    CHECK(s.stop(&error));
    const Session::ClosedTake c = only_take(s);
    const std::vector<uint64_t> want = expected_wraps(l, 12 * kBlock);
    CHECK(want.size() >= 2);
    CHECK(c.loop_wraps == want.size());
    CHECK(c.files.size() == 4);
    int lost = 0;
    for (const auto& f : c.files) {
        CHECK(f.wraps.size() == want.size());
        for (std::size_t k = 1; k < want.size(); ++k) CHECK(f.wraps[k] == want[k]);
        if (f.wraps[0] == WrapSidecar::kLostWrap) {
            ++lost;
        } else {
            CHECK(f.wraps[0] == want[0]);
        }
        sonora::WrapList list;
        CHECK(sonora::read_wrap_sidecar(WrapSidecar::path_for(f.path), list, error));
        CHECK(list.wraps == f.wraps);
    }
    CHECK(lost == 1);
    return 0;
}

// O2: past the cap the take keeps recording; the list in memory stops at
// the cap and says how many more there were, and the file on disk has them
// all.
int test_pass_cap() {
    Rig rig("cap", 1, 0, 3);
    Session& s = *rig.session;
    const LoopRange l = loop_of(1000, 1300, 0);
    std::string error;
    const int take = s.next_free_take();
    CHECK(s.start_loop(take, l, &error));
    for (int b = 0; b < 30; ++b) rig.block();
    CHECK(s.stop(&error));
    const std::vector<uint64_t> want = expected_wraps(l, 30 * kBlock);
    CHECK(want.size() == 6);
    const Session::ClosedTake c = only_take(s);
    CHECK(c.files.size() == 1);
    CHECK(c.files[0].frames == 30 * kBlock);
    CHECK((c.files[0].wraps == std::vector<uint64_t>(want.begin(), want.begin() + 3)));
    CHECK(c.files[0].wraps_past_cap == 3);
    sonora::WrapList list;
    CHECK(sonora::read_wrap_sidecar(WrapSidecar::path_for(c.files[0].path), list, error));
    CHECK(list.wraps == want);
    return 0;
}

// What plays across the wrap is the timeline at B, then at A: an unarmed
// track's region reads the frames just before B, then A's.
int playback_case(const char* name, const LoopRange& l, int blocks, uint32_t pool_chunks) {
    Rig rig(name, 2, 0, 99, pool_chunks);
    Session& s = *rig.session;
    s.tracks()[0]->set_rec_armed(true);
    s.tracks()[1]->set_rec_armed(false);
    s.tracks()[0]->set_monitor(sonora::Track::Monitor::Off);
    s.tracks()[1]->set_pan(-1.0f);
    const std::string path = (rig.dir / "source.wav").string();
    CHECK(write_source(path, 5, 1, 40 * kChunk));
    uint32_t slot = 0;
    std::string error;
    CHECK(s.reader().add_source(path, slot, error));
    SegmentSpec sp;
    sp.track = 1;
    sp.end = 40 * kChunk;
    sp.source_slot = slot;
    CHECK(publish_plan(s, make_plan(s.plans().next_epoch(), 2, {sp}, kChunk)));
    // The faders ramp up from silence in the session's first block.
    for (int b = 0; b < 2; ++b) rig.block();
    const int take = s.next_free_take();
    CHECK(s.start_loop(take, l, &error));
    s.reader().run_pass();
    uint64_t timeline = l.start;
    for (int b = 0; b < blocks; ++b) {
        rig.block();
        s.reader().run_pass();
        for (uint32_t i = 0; i < kBlock; ++i) {
            const float want = source_value(5, 0, static_cast<int64_t>(timeline));
            if (rig.out[0][i] != want) {
                std::fprintf(stderr, "  block %d frame %u: %.6f, want frame %llu's %.6f\n", b, i,
                             static_cast<double>(rig.out[0][i]),
                             static_cast<unsigned long long>(timeline), static_cast<double>(want));
                CHECK(false);
            }
            if (++timeline == l.to) timeline = l.from;
        }
    }
    CHECK(s.playback_misses() == 0);
    CHECK(s.stop(&error));
    return 0;
}

int test_playback_follows_the_wrap() {
    return playback_case("playback", loop_of(1000, 1300, 100), 20, 96);
}

// Code review of PR #117, B1. A loop longer than two reader windows (W is
// 1216 frames here) over a pool too small to hold it: A's chunks would be
// evicted long before B, and every track not recording would play silence
// at the top of each pass. The reader holds [A, A + W) for the take.
int test_playback_holds_the_loop_start() {
    const LoopRange l = loop_of(1000, 7000, 100);
    // Three wraps and a little more.
    const int blocks = static_cast<int>((l.to - l.start + 2 * (l.to - l.from) + 600) / kBlock);
    return playback_case("loop_start", l, blocks, 20);
}

// RT audit of PR #117: a callback longer than one pass wraps more than once.
// Each wrap gets its own record, consecutive ordinals, `at` one loop
// length after the last.
int test_wraps_in_one_callback() {
    if (wraps_case("two_in_one", loop_of(1000, 1000 + kMaxBlock, 0), 8, kLongest)) return 1;
    return wraps_case("two_in_one_latency", loop_of(1000, 1000 + kMaxBlock, 2 * kMaxBlock + 17, 9),
                      9, kLongest);
}

// RT audit of PR #117, highest risk. The disk thread held for the whole
// stall the ring is sized for (a slow sync on a stick), with 18 tracks
// recording the shortest loop allowed (one block), loses no wrap: the
// boundary ring is sized by the wrap rate. The stall is the config's, not a
// shorter one, so the test holds the ring to the bound it claims.
int test_a_stalled_disk_thread_loses_no_wrap() {
    Rig rig("stall", 18);
    Session& s = *rig.session;
    const LoopRange l = loop_of(1000, 1000 + kMaxBlock, 0);
    std::string error;
    const int take = s.next_free_take();
    CHECK(s.start_loop(take, l, &error));
    const sonora::SessionConfig defaults;
    const int blocks =
        static_cast<int>(defaults.disk_stall_seconds * defaults.sample_rate / kBlock);
    for (int b = 0; b < blocks; ++b) rig.block();
    CHECK(s.boundary_overruns() == 0);
    CHECK(s.stop(&error));
    const uint64_t frames = static_cast<uint64_t>(blocks) * kBlock;
    const std::vector<uint64_t> want = expected_wraps(l, frames);
    CHECK(want.size() > 50);
    const Session::ClosedTake c = only_take(s);
    CHECK(c.loop_wraps == want.size());
    CHECK(c.files.size() == 18);
    for (const auto& f : c.files) {
        CHECK(f.frames == frames);
        CHECK(f.wraps == want);
    }
    return 0;
}

// The passes engine/control places for one file of a closed loop take.
std::vector<sonora::LoopPass> passes_of(const Session::ClosedTake& c,
                                        const sonora::DiskThread::ClosedFile& f) {
    sonora::WavReader r;
    std::string error;
    if (!r.open(f.path, error)) return {};
    bool derived = false;
    const std::vector<uint64_t> wraps =
        sonora::complete_wraps(f.wraps, c.loop_wraps, f.frames, c.loop.from, c.loop.to,
                               r.time_reference(), c.loop.latency, derived);
    return sonora::loop_passes(wraps, f.frames, c.loop.from, c.loop.to, r.time_reference(),
                               c.loop.latency);
}

// BACKLOG Track 1, loop record: a track whose window opens late, after the
// take's first wrap, and records no later wrap still has its pass numbered
// as the take's second, as every other track numbers it. The Open record
// says how many wraps the take had made.
int test_a_late_track_numbers_its_passes_from_the_take() {
    // The ring holds one record: track 1's open finds no room until it is
    // drained, after the first wrap (block 6: start 900, B 1300).
    Rig rig("late_open", 2, 2);
    Session& s = *rig.session;
    const LoopRange l = loop_of(1000, 1300, 100);
    std::string error;
    const int take = s.next_free_take();
    CHECK(s.start_loop(take, l, &error));
    for (int b = 0; b < 7; ++b) rig.block();
    for (int b = 7; b < 10; ++b) {
        s.disk().drain_once();
        rig.block();
    }
    CHECK(s.stop(&error));
    const Session::ClosedTake c = only_take(s);
    CHECK(c.loop_wraps == 1);
    CHECK(c.files.size() == 2);
    const sonora::DiskThread::ClosedFile* late = nullptr;
    for (const auto& f : c.files) {
        if (f.track == 1) late = &f;
    }
    CHECK(late != nullptr);
    CHECK(late->frames == 3 * kBlock);
    // Its first sample was played at A + 48, in the second pass.
    CHECK(file_is_counted(late->path, 3 * kBlock, 1048, "loop=1000-1300", 7 * kBlock));
    // The take's wrap before the file holds its place, in memory and on
    // disk, so recovery numbers the pass the same.
    CHECK((late->wraps == std::vector<uint64_t>{WrapSidecar::kBeforeFile}));
    CHECK(late->wraps_before_file == 1);
    for (const auto& f : c.files) {
        if (f.track == 0) CHECK(f.wraps_before_file == 0);
    }
    sonora::WrapList list;
    CHECK(sonora::read_wrap_sidecar(WrapSidecar::path_for(late->path), list, error));
    CHECK(list.wraps == late->wraps);
    const std::vector<sonora::LoopPass> p = passes_of(c, *late);
    CHECK(p.size() == 1);
    CHECK(p[0].pass == 2);
    CHECK(p[0].offset == 0 && p[0].at == 48 && p[0].length == 3 * kBlock && !p[0].complete);
    return 0;
}

// Holds the callback it is called from, right after the transport is
// loaded, while armed.
struct Hold {
    std::atomic<bool> armed{false};
    std::atomic<bool> held{false};
    std::atomic<bool> release{false};
};
void hold_hook(void* context) noexcept SONORA_RT {
    auto* h = static_cast<Hold*>(context);
    if (!h->armed.exchange(false, std::memory_order_acq_rel)) return;
    h->held.store(true, std::memory_order_release);
    while (!h->release.load(std::memory_order_acquire)) {
    }
}

// Code review of PR #117, N2. A callback that loaded Rolling before
// start_loop() stopped the transport must not drain the loop's locate and
// roll a block on from it: pass 1 would start after the pre-roll's start,
// and with a pre-roll under a block, after A. start_loop() waits it out.
int test_a_callback_in_flight_keeps_pass_one_whole() {
    Rig rig("in_flight", 1);
    Session& s = *rig.session;
    Hold hold;
    sonora::SessionTestAccess::set_word_hook(s, hold_hook, &hold);
    s.play();
    for (int b = 0; b < 4; ++b) rig.block();
    CHECK(s.transport().state() == TransportState::Rolling);
    hold.armed.store(true, std::memory_order_release);
    std::thread audio([&] { rig.block(); });
    for (int i = 0; i < 5000 && !hold.held.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(hold.held.load(std::memory_order_acquire));
    const LoopRange l = loop_of(1000, 1300, 10);
    std::atomic<bool> started{false};
    bool ok = false;
    std::string error;
    std::thread control([&] {
        ok = s.start_loop(s.next_free_take(), l, &error);
        started.store(true, std::memory_order_release);
    });
    // Long enough for start_loop() to finish if it does not wait for the
    // callback; it must, so this is the callback's release.
    for (int i = 0; i < 200 && !started.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    hold.release.store(true, std::memory_order_release);
    audio.join();
    control.join();
    CHECK(ok);
    rig.block();
    CHECK(s.take_origin() == l.start);
    sonora::SessionTestAccess::set_word_hook(s, nullptr, nullptr);
    CHECK(s.stop(&error));
    return 0;
}

// The Description says the take was a loop record, in place of punch=, and
// reads back from a name that holds "loop=" itself.
int test_description_says_loop() {
    LoopRange l = loop_of(48000, 96000, 0);
    const std::string d = sonora::take_description(3, 7, "Demo loop=1-2", nullptr, &l);
    CHECK(d == "sonora take=3 track=7 loop=48000-96000 session=Demo loop=1-2");
    uint64_t a = 0, b = 0;
    CHECK(sonora::loop_of_description(d, a, b));
    CHECK(a == 48000 && b == 96000);
    CHECK(!sonora::loop_of_description("sonora take=3 track=7 session=Demo loop=1-2", a, b));
    CHECK(!sonora::loop_of_description("sonora take=3 punch=10-20 session=x", a, b));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    struct Test {
        const char* name;
        int (*run)();
    };
    const Test tests[] = {
        {"wraps_are_marked_in_band", test_wraps_are_marked_in_band},
        {"wrap_on_a_block_edge", test_wrap_on_a_block_edge},
        {"no_pre_roll", test_no_pre_roll},
        {"locates_to_the_pre_roll", test_locates_to_the_pre_roll},
        {"short_loops_are_refused", test_short_loops_are_refused},
        {"wraps_reach_disk_at_the_checkpoint", test_wraps_reach_disk_at_the_checkpoint},
        {"lost_wrap_keeps_its_place", test_lost_wrap_keeps_its_place},
        {"pass_cap", test_pass_cap},
        {"playback_follows_the_wrap", test_playback_follows_the_wrap},
        {"playback_holds_the_loop_start", test_playback_holds_the_loop_start},
        {"wraps_in_one_callback", test_wraps_in_one_callback},
        {"a_stalled_disk_thread_loses_no_wrap", test_a_stalled_disk_thread_loses_no_wrap},
        {"a_callback_in_flight_keeps_pass_one_whole",
         test_a_callback_in_flight_keeps_pass_one_whole},
        {"a_late_track_numbers_its_passes_from_the_take",
         test_a_late_track_numbers_its_passes_from_the_take},
        {"description_says_loop", test_description_says_loop},
    };
    for (const Test& t : tests) {
        if (argc > 1 && std::string(argv[1]) != t.name) continue;
        std::printf("%s\n", t.name);
        if (t.run()) return 1;
    }
    std::printf("loop_record: ok\n");
    return 0;
}
