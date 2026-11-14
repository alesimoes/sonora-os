// SPDX-License-Identifier: GPL-3.0-or-later
//
// The launcher integration contract (ADR-0224) on FakeLaunchController: the
// refusals, transitions driving the voices at their frames, the clip table
// refusing a start it cannot resolve, clip-to-region resolution of a played
// span, bounce-to-arrangement, record-to-cell, a full queue, a lost event,
// a session reset, and drive_voices() allocating nothing.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

#include "sonora/fake_launch_controller.h"
#include "sonora/launch_controller.h"
#include "sonora/launch_voice_source.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {
std::atomic<long> g_allocations{0};
}

void* operator new(std::size_t n) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n == 0 ? 1 : n))
        return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

using namespace sonora;
using launch::Transition;

// 120 BPM 4/4 at 48 kHz: a bar is 96000 frames.
constexpr std::int64_t kBar = 96000;
constexpr std::uint32_t kBlock = 512;

FakeLaunchConfig config(std::size_t queue = 256, std::size_t events = 1024) {
    FakeLaunchConfig c;
    c.launcher.tracks = 2;
    c.launcher.scenes = 4;
    c.launcher.queue_capacity = queue;
    c.event_capacity = events;
    return c;
}

// Runs whole blocks from `from` until `to` is covered; returns the next
// block's start, so consecutive runs neither overlap nor leave a gap.
std::int64_t run(FakeLaunchController& c, std::int64_t from, std::int64_t to) {
    std::int64_t pos = from;
    for (; pos < to; pos += kBlock)
        c.run_block(pos, kBlock);
    return pos;
}

int refusals() {
    FakeLaunchController c(config());
    c.add_region(1, 10, 0, 48000);
    c.add_region(0, 11, 0, 0);
    c.add_region(0, 12, 0, 48000);
    c.set_session_open(false);
    CHECK(c.assign_region(0, 0, 12).refusal == LaunchRefusal::NoSession);
    CHECK(c.trigger_cell(0, 0).refusal == LaunchRefusal::NoSession);
    CHECK(c.bounce_to_arrangement({0, kBar}).refusal == LaunchRefusal::NoSession);
    c.set_session_open(true);
    CHECK(c.assign_region(2, 0, 12).refusal == LaunchRefusal::OutOfGrid);
    CHECK(c.assign_region(0, 4, 12).refusal == LaunchRefusal::OutOfGrid);
    CHECK(c.trigger_cell(0, 4).refusal == LaunchRefusal::OutOfGrid);
    CHECK(c.stop_track(2).refusal == LaunchRefusal::OutOfGrid);
    CHECK(c.assign_region(0, 0, 99).refusal == LaunchRefusal::NoSuchRegion);
    CHECK(c.assign_region(0, 0, 10).refusal == LaunchRefusal::RegionOnOtherTrack);
    CHECK(c.assign_region(0, 0, 11).refusal == LaunchRefusal::EmptyRegion);
    CHECK(c.assign_region(0, 0, 12).ok());
    CHECK(c.cell(0, 0).clip == 12);
    return 0;
}

// A trigger off the bar lands on the next bar, with the region's loop length
// and source offset; a stop lands on the bar after it.
int transitions_drive_voices() {
    FakeLaunchController c(config());
    c.add_region(0, 10, 1000, 48000);
    CHECK(c.assign_region(0, 0, 10).ok());
    CHECK(c.trigger_cell(0, 0).ok());
    std::int64_t pos = run(c, 1000, kBar + 10 * kBlock + 1000);
    CHECK(c.voices.calls.size() == 1);
    const FakeVoiceCall& start = c.voices.calls[0];
    CHECK(start.start && start.track == 0 && start.clip == 10);
    CHECK(start.frame == kBar);
    CHECK(start.loop_frames == 48000 && start.source_offset == 1000);
    c.pump();
    CHECK(c.cell(0, 0).playing);
    CHECK(c.stop_track(0).ok());
    pos = run(c, pos, 3 * kBar);
    CHECK(c.voices.calls.size() == 2);
    CHECK(!c.voices.calls[1].start && c.voices.calls[1].frame == 2 * kBar);
    c.pump();
    CHECK(!c.cell(0, 0).playing);
    CHECK(c.counts.unresolved == 0 && c.counts.dropped == 0);
    return 0;
}

// A re-trigger in the column: the old voice stops and the new one starts on
// the same frame, in that order.
int retrigger_stops_then_starts() {
    FakeLaunchController c(config());
    c.add_region(0, 10, 0, 48000);
    c.add_region(0, 20, 500, 24000);
    CHECK(c.assign_region(0, 0, 10).ok());
    CHECK(c.assign_region(0, 1, 20).ok());
    CHECK(c.trigger_cell(0, 0).ok());
    std::int64_t pos = run(c, 0, kBar + kBlock);
    CHECK(c.trigger_cell(0, 1).ok());
    pos = run(c, pos, 2 * kBar + kBlock);
    CHECK(c.voices.calls.size() == 3);
    CHECK(c.voices.calls[0].start && c.voices.calls[0].clip == 10);
    CHECK(c.voices.calls[0].frame == 0);
    CHECK(!c.voices.calls[1].start && c.voices.calls[1].frame == 2 * kBar);
    CHECK(c.voices.calls[2].start && c.voices.calls[2].clip == 20);
    CHECK(c.voices.calls[2].frame == 2 * kBar && c.voices.calls[2].loop_frames == 24000);
    c.pump();
    CHECK(!c.cell(0, 0).playing && c.cell(0, 1).playing);
    return 0;
}

// The table does not hold the clip the transition names (its resolution was
// lost): the start is refused and counted, never played with another length.
int unresolved_start_refused() {
    FakeLaunchController c(config());
    c.add_region(0, 10, 0, 48000);
    CHECK(c.assign_region(0, 0, 10).ok());
    std::int64_t pos = run(c, 1, 1 + kBlock);
    CHECK(c.audio_table().find(0, 0, 10) != nullptr);
    CHECK(c.audio_table().set(0, 0, ResolvedClip{999, 0, 1000, 0.0f}));
    CHECK(c.trigger_cell(0, 0).ok());
    run(c, pos, kBar + kBlock);
    CHECK(c.voices.calls.empty());
    CHECK(c.counts.unresolved == 1);

    ClipTable t(1, 1);
    CHECK(!t.set(1, 0, ResolvedClip{1, 0, 10, 0.0f}));
    CHECK(t.find(0, 0, launch::kNoClip) == nullptr);
    CHECK(t.set(0, 0, ResolvedClip{1, 0, 0, 0.0f}));
    CHECK(t.find(0, 0, 1) == nullptr);
    return 0;
}

int place_span_resolves_passes() {
    const ResolvedClip clip{7, 50, 1000, 0.0f};
    std::vector<PlacedPass> out;
    place_span(0, clip, 0, 2500, 0, 10000, out);
    CHECK(out.size() == 3);
    CHECK(out[0].position == 0 && out[0].length == 1000 && out[0].source_offset == 50);
    CHECK(out[0].loop == 0);
    CHECK(out[1].position == 1000 && out[1].length == 1000 && out[1].loop == 1);
    CHECK(out[2].position == 2000 && out[2].length == 500 && out[2].loop == 2);
    CHECK(out[2].source_offset == 50 && out[2].clip == 7);

    // A window inside the span: passes keep the span's phase.
    out.clear();
    place_span(1, clip, 0, 2500, 1500, 2200, out);
    CHECK(out.size() == 2);
    CHECK(out[0].track == 1 && out[0].position == 1500 && out[0].length == 500);
    CHECK(out[0].source_offset == 550 && out[0].loop == 1);
    CHECK(out[1].position == 2000 && out[1].length == 200 && out[1].loop == 2);

    // A long span skips the passes before the window without walking them.
    out.clear();
    place_span(0, clip, 0, 2000000, 1000250, 1001250, out);
    CHECK(out.size() == 2);
    CHECK(out[0].position == 1000250 && out[0].length == 750 && out[0].source_offset == 300);
    CHECK(out[0].loop == 1000);
    CHECK(out[1].position == 1001000 && out[1].length == 250 && out[1].loop == 1001);

    out.clear();
    place_span(0, clip, 100, 100, 0, 1000, out);
    place_span(0, clip, 0, 500, 500, 1000, out);
    place_span(0, ResolvedClip{7, 0, 0, 0.0f}, 0, 500, 0, 1000, out);
    CHECK(out.empty());
    return 0;
}

int bounce_writes_what_played() {
    FakeLaunchController c(config());
    c.add_region(0, 10, 0, 48000);
    CHECK(c.assign_region(0, 0, 10).ok());
    CHECK(c.trigger_cell(0, 0).ok());
    std::int64_t pos = run(c, 1000, kBar + kBlock);
    CHECK(c.stop_track(0).ok());
    pos = run(c, pos, 2 * kBar + kBlock);

    CHECK(c.bounce_to_arrangement({kBar, kBar}).refusal == LaunchRefusal::BadRange);
    CHECK(c.bounce_to_arrangement({3 * kBar, 4 * kBar}).refusal ==
          LaunchRefusal::NothingToBounce);
    c.set_take_open(true);
    CHECK(c.bounce_to_arrangement({0, 3 * kBar}).refusal == LaunchRefusal::TakeOpen);
    c.set_take_open(false);

    BounceResult r = c.bounce_to_arrangement({0, 3 * kBar});
    CHECK(r.ok() && r.regions_placed == 2 && r.events_lost == 0);
    CHECK(c.placed.size() == 2);
    CHECK(c.placed[0].position == kBar && c.placed[0].length == 48000 && c.placed[0].loop == 0);
    CHECK(c.placed[1].position == kBar + 48000 && c.placed[1].loop == 1);

    // Still playing at the bounce: cut at the window's end.
    CHECK(c.trigger_cell(0, 0).ok());
    run(c, pos, 3 * kBar + 22000);
    c.placed.clear();
    r = c.bounce_to_arrangement({0, 3 * kBar + 22000});
    CHECK(r.ok() && r.regions_placed == 3);
    CHECK(c.placed[2].position == 3 * kBar && c.placed[2].length == 22000);
    return 0;
}

int record_to_cell() {
    FakeLaunchController c(config());
    CHECK(c.record_to_cell({0, 2, 1}).refusal == LaunchRefusal::NotArmed);
    c.set_armed(0, true);
    c.set_take_open(true);
    CHECK(c.record_to_cell({0, 2, 1}).refusal == LaunchRefusal::TakeOpen);
    c.set_take_open(false);
    CHECK(c.record_to_cell({0, 4, 1}).refusal == LaunchRefusal::OutOfGrid);
    CHECK(c.record_to_cell({0, 2, 1}).ok());
    CHECK(c.record_to_cell({0, 3, 1}).refusal == LaunchRefusal::CellRecording);
    CHECK(c.cell(0, 2).recording && !c.cell(0, 3).recording);
    CHECK(c.record_requests.size() == 1 && c.record_requests[0].bars == 1);

    CHECK(c.finish_recording(0, 30, kBar).ok());
    CHECK(!c.cell(0, 2).recording && c.cell(0, 2).clip == 30);
    run(c, 1, kBar + kBlock);
    CHECK(c.voices.calls.size() == 1);
    CHECK(c.voices.calls[0].clip == 30 && c.voices.calls[0].frame == kBar);
    CHECK(c.voices.calls[0].loop_frames == kBar);
    return 0;
}

// A full queue refuses the command; a refused assignment leaves the audio
// side's table on the clip the grid still holds.
int full_queue() {
    FakeLaunchController c(config(4));
    c.add_region(0, 10, 0, 48000);
    c.add_region(0, 20, 0, 48000);
    CHECK(c.assign_region(0, 0, 10).ok());
    std::int64_t pos = run(c, 0, kBlock);
    int pushed = 0;
    while (c.stop_all().ok() && pushed < 100)
        ++pushed;
    CHECK(pushed < 100);
    CHECK(c.trigger_cell(0, 0).refusal == LaunchRefusal::QueueFull);
    CHECK(c.assign_region(0, 0, 20).refusal == LaunchRefusal::QueueFull);
    CHECK(c.cell(0, 0).clip == 10);
    run(c, pos, pos + kBlock);
    CHECK(c.audio_table().find(0, 0, 10) != nullptr);
    CHECK(c.audio_table().find(0, 0, 20) == nullptr);
    return 0;
}

// A full event ring: the bounce says how many transitions it could not see.
int lost_event_reported() {
    FakeLaunchController c(config(256, 1));
    c.add_region(0, 10, 0, 48000);
    c.add_region(1, 11, 0, 48000);
    CHECK(c.assign_region(0, 0, 10).ok());
    CHECK(c.assign_region(1, 0, 11).ok());
    CHECK(c.trigger_scene(0).ok());
    run(c, 1, kBar + kBlock);
    CHECK(c.voices.calls.size() == 2);
    const BounceResult r = c.bounce_to_arrangement({0, 2 * kBar});
    CHECK(r.ok() && r.events_lost == 1);
    CHECK(r.regions_placed == 2);
    return 0;
}

int reset_forgets_the_session() {
    FakeLaunchController c(config());
    c.add_region(0, 10, 0, 48000);
    CHECK(c.assign_region(0, 0, 10).ok());
    CHECK(c.trigger_cell(0, 0).ok());
    run(c, 0, kBlock);
    c.pump();
    CHECK(c.cell(0, 0).playing);
    c.reset();
    CHECK(c.cell(0, 0).clip == launch::kNoClip && !c.cell(0, 0).playing);
    CHECK(c.voices.calls.empty());
    CHECK(c.bounce_to_arrangement({0, kBar}).refusal == LaunchRefusal::NothingToBounce);
    return 0;
}

class CountingVoices final : public LaunchVoiceSource {
public:
    void start(std::uint32_t, const ResolvedClip&, std::uint32_t) override { ++starts; }
    void stop(std::uint32_t, std::uint32_t) override { ++stops; }
    int starts = 0;
    int stops = 0;
};

int drive_voices_allocates_nothing() {
    ClipTable table(2, 2);
    CHECK(table.set(0, 1, ResolvedClip{5, 0, 1000, 0.0f}));
    Transition items[3];
    items[0] = Transition{Transition::Kind::Stop, 0, 0, 4, 10};
    items[1] = Transition{Transition::Kind::Start, 0, 1, 5, 10};
    items[2] = Transition{Transition::Kind::Start, 1, 0, 6, 20};
    const launch::TransitionList list{items, 3, 1};
    CountingVoices voices;
    DriveCounts counts;
    const long before = g_allocations.load();
    for (int i = 0; i < 8000; ++i)
        drive_voices(list, table, voices, counts);
    CHECK(g_allocations.load() == before);
    CHECK(voices.starts == 8000 && voices.stops == 8000);
    CHECK(counts.unresolved == 8000 && counts.dropped == 8000);
    return 0;
}

}  // namespace

int main() {
    int failures = 0;
    failures += refusals();
    failures += transitions_drive_voices();
    failures += retrigger_stops_then_starts();
    failures += unresolved_start_refused();
    failures += place_span_resolves_passes();
    failures += bounce_writes_what_played();
    failures += record_to_cell();
    failures += full_queue();
    failures += lost_event_reported();
    failures += reset_forgets_the_session();
    failures += drive_voices_allocates_nothing();
    if (failures == 0)
        std::printf("launch_controller_contract: all passed\n");
    return failures == 0 ? 0 : 1;
}
