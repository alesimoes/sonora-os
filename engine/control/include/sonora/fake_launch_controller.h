#pragma once

// A LaunchController for tests and for the UI slice before the engine slice
// lands (ADR-0224 section 5). It runs the real StateMachineLauncher and the
// real drive_voices() against an in-memory region list and a voice source
// that records its calls; run_block() stands in for Session::process(). The
// clip ring and the event ring are plain vectors here: the fake allocates,
// the engine's rings do not.

#include "sonora/launch/state_machine_launcher.h"
#include "sonora/launch_controller.h"
#include "sonora/launch_voice_source.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace sonora {

struct FakeVoiceCall {
    bool start = false;
    std::uint32_t track = 0;
    launch::ClipId clip = launch::kNoClip;
    std::int64_t loop_frames = 0;
    std::int64_t source_offset = 0;
    // Absolute: block_start + offset.
    std::int64_t frame = 0;
};

class FakeLaunchVoices final : public LaunchVoiceSource {
public:
    void start(std::uint32_t track, const ResolvedClip& clip, std::uint32_t offset) override {
        calls.push_back(FakeVoiceCall{true, track, clip.clip, clip.loop_frames,
                                      clip.source_offset, block_start + offset});
    }
    void stop(std::uint32_t track, std::uint32_t offset) override {
        calls.push_back(FakeVoiceCall{false, track, launch::kNoClip, 0, 0, block_start + offset});
    }

    std::int64_t block_start = 0;
    std::vector<FakeVoiceCall> calls;
};

struct FakeLaunchConfig {
    launch::LauncherConfig launcher;
    launch::TransportSnapshot transport;
    // Events the simulated audio-to-control ring holds between pump()s.
    std::size_t event_capacity = 1024;
};

class FakeLaunchController final : public LaunchController {
public:
    explicit FakeLaunchController(const FakeLaunchConfig& config) : config_(config) { reset(); }

    // The fake document and engine state.
    void add_region(std::uint32_t track, launch::ClipId id, std::int64_t source_offset,
                    std::int64_t length, float gain_db = 0.0f) {
        regions_[id] = Region{track, ResolvedClip{id, source_offset, length, gain_db}};
    }
    void set_session_open(bool open) { session_open_ = open; }
    void set_armed(std::uint32_t track, bool armed) { armed_[track] = armed; }
    void set_take_open(bool open) { take_open_ = open; }

    // Session::process() for one block: drain the clip ring, advance the
    // launcher, drive the voices, push the events.
    void run_block(std::int64_t block_start, std::uint32_t frames) {
        for (const PendingClip& p : clip_ring_)
            table_->set(p.track, p.scene, p.clip);
        clip_ring_.clear();
        launcher_->set_transport(config_.transport);
        const launch::TransitionList list = launcher_->advance(block_start, frames);
        voices.block_start = block_start;
        drive_voices(list, *table_, voices, counts);
        for (std::uint32_t i = 0; i < list.count; ++i) {
            const LaunchEvent e = to_event(list[i], block_start);
            if (event_ring_.size() >= config_.event_capacity)
                lost_frames_.push_back(e.frame);
            else
                event_ring_.push_back(e);
        }
    }

    // A take into a cell ended (the engine's record path does this): its
    // region joins the track, is assigned to the cell and launched.
    LaunchReply finish_recording(std::uint32_t track, launch::ClipId region,
                                 std::int64_t length) {
        if (track >= tracks() || recording_[track] < 0)
            return LaunchReply{LaunchRefusal::OutOfGrid};
        const auto scene = static_cast<std::uint32_t>(recording_[track]);
        recording_[track] = launch::kNone;
        add_region(track, region, 0, length);
        const LaunchReply assigned = assign_region(track, scene, region);
        if (!assigned.ok())
            return assigned;
        return trigger_cell(track, scene);
    }

    LaunchReply assign_region(std::uint32_t track, std::uint32_t scene,
                              launch::ClipId region) override {
        if (const LaunchReply r = check_cell(track, scene); !r.ok())
            return r;
        const auto it = regions_.find(region);
        if (it == regions_.end())
            return LaunchReply{LaunchRefusal::NoSuchRegion};
        if (it->second.track != track)
            return LaunchReply{LaunchRefusal::RegionOnOtherTrack};
        if (it->second.clip.loop_frames <= 0)
            return LaunchReply{LaunchRefusal::EmptyRegion};
        return publish(track, scene, it->second.clip);
    }

    LaunchReply clear_cell(std::uint32_t track, std::uint32_t scene) override {
        if (const LaunchReply r = check_cell(track, scene); !r.ok())
            return r;
        return publish(track, scene, ResolvedClip{});
    }

    LaunchReply set_quantize(launch::Quantize q) override {
        if (!session_open_)
            return LaunchReply{LaunchRefusal::NoSession};
        return queued(launcher_->set_quantize(q));
    }
    LaunchReply trigger_cell(std::uint32_t track, std::uint32_t scene) override {
        if (const LaunchReply r = check_cell(track, scene); !r.ok())
            return r;
        return queued(launcher_->trigger_cell(track, scene));
    }
    LaunchReply trigger_scene(std::uint32_t scene) override {
        if (const LaunchReply r = check_cell(0, scene); !r.ok())
            return r;
        return queued(launcher_->trigger_scene(scene));
    }
    LaunchReply stop_track(std::uint32_t track) override {
        if (const LaunchReply r = check_cell(track, 0); !r.ok())
            return r;
        return queued(launcher_->stop_track(track));
    }
    LaunchReply stop_all() override {
        if (!session_open_)
            return LaunchReply{LaunchRefusal::NoSession};
        return queued(launcher_->stop_all());
    }

    LaunchReply record_to_cell(const RecordToCellRequest& request) override {
        if (const LaunchReply r = check_cell(request.track, request.scene); !r.ok())
            return r;
        if (!armed_[request.track])
            return LaunchReply{LaunchRefusal::NotArmed};
        if (take_open_)
            return LaunchReply{LaunchRefusal::TakeOpen};
        if (recording_[request.track] >= 0)
            return LaunchReply{LaunchRefusal::CellRecording};
        recording_[request.track] = static_cast<std::int32_t>(request.scene);
        record_requests.push_back(request);
        return LaunchReply{};
    }

    BounceResult bounce_to_arrangement(const BounceToArrangementRequest& request) override {
        if (!session_open_)
            return BounceResult{LaunchRefusal::NoSession};
        if (take_open_)
            return BounceResult{LaunchRefusal::TakeOpen};
        if (request.to_frame <= request.from_frame)
            return BounceResult{LaunchRefusal::BadRange};
        pump();
        std::vector<PlacedPass> passes;
        std::vector<Span> open(tracks());
        for (const LaunchEvent& e : log_) {
            Span& span = open[e.track];
            if (span.clip != nullptr) {
                place_span(e.track, *span.clip, span.start, e.frame, request.from_frame,
                           request.to_frame, passes);
                span.clip = nullptr;
            }
            if (e.kind == launch::Transition::Kind::Start)
                span = Span{played(e), e.frame};
        }
        // Still playing at the bounce: the span runs to the window's end.
        for (std::uint32_t t = 0; t < tracks(); ++t)
            if (open[t].clip != nullptr)
                place_span(t, *open[t].clip, open[t].start, request.to_frame,
                           request.from_frame, request.to_frame, passes);
        BounceResult result;
        result.events_lost = static_cast<std::uint64_t>(std::count_if(
            lost_frames_.begin(), lost_frames_.end(), [&](std::int64_t f) {
                return f >= request.from_frame && f < request.to_frame;
            }));
        if (passes.empty()) {
            result.refusal = LaunchRefusal::NothingToBounce;
            return result;
        }
        result.regions_placed = static_cast<std::uint32_t>(passes.size());
        placed.insert(placed.end(), passes.begin(), passes.end());
        return result;
    }

    void pump() override {
        for (const LaunchEvent& e : event_ring_) {
            for (std::uint32_t s = 0; s < scenes(); ++s)
                view(e.track, s).playing = false;
            if (e.kind == launch::Transition::Kind::Start)
                view(e.track, e.scene).playing = true;
            log_.push_back(e);
        }
        event_ring_.clear();
    }

    LaunchCellView cell(std::uint32_t track, std::uint32_t scene) const override {
        if (track >= tracks() || scene >= scenes())
            return LaunchCellView{};
        LaunchCellView v = views_[index(track, scene)];
        v.recording = recording_[track] == static_cast<std::int32_t>(scene);
        return v;
    }

    void reset() override {
        launcher_ = std::make_unique<launch::StateMachineLauncher>(config_.launcher);
        table_ = std::make_unique<ClipTable>(tracks(), scenes());
        views_.assign(static_cast<std::size_t>(tracks()) * scenes(), LaunchCellView{});
        resolved_.assign(views_.size(), ResolvedClip{});
        armed_.assign(tracks(), false);
        recording_.assign(tracks(), launch::kNone);
        clip_ring_.clear();
        event_ring_.clear();
        lost_frames_.clear();
        log_.clear();
        voices.calls.clear();
        counts = DriveCounts{};
        record_requests.clear();
        placed.clear();
        take_open_ = false;
    }

    std::uint32_t tracks() const { return config_.launcher.tracks; }
    std::uint32_t scenes() const { return config_.launcher.scenes; }
    // Lets a test overwrite the audio side's table, as an overrun clip ring
    // would leave it.
    ClipTable& audio_table() { return *table_; }

    FakeLaunchVoices voices;
    DriveCounts counts;
    std::vector<RecordToCellRequest> record_requests;
    // Every pass a bounce wrote, in order.
    std::vector<PlacedPass> placed;

private:
    struct Region {
        std::uint32_t track = 0;
        ResolvedClip clip;
    };
    struct PendingClip {
        std::uint32_t track = 0;
        std::uint32_t scene = 0;
        ResolvedClip clip;
    };
    struct Span {
        const ResolvedClip* clip = nullptr;
        std::int64_t start = 0;
    };

    LaunchReply check_cell(std::uint32_t track, std::uint32_t scene) const {
        if (!session_open_)
            return LaunchReply{LaunchRefusal::NoSession};
        if (track >= tracks() || scene >= scenes())
            return LaunchReply{LaunchRefusal::OutOfGrid};
        return LaunchReply{};
    }
    static LaunchReply queued(bool pushed) {
        return LaunchReply{pushed ? LaunchRefusal::None : LaunchRefusal::QueueFull};
    }
    // The clip goes to the audio side before the grid learns its id.
    LaunchReply publish(std::uint32_t track, std::uint32_t scene, const ResolvedClip& clip) {
        clip_ring_.push_back(PendingClip{track, scene, clip});
        if (!launcher_->assign_clip(track, scene, clip.clip)) {
            // The grid keeps the old clip, so the table must too.
            clip_ring_.push_back(PendingClip{track, scene, resolved_[index(track, scene)]});
            return LaunchReply{LaunchRefusal::QueueFull};
        }
        resolved_[index(track, scene)] = clip;
        view(track, scene).clip = clip.clip;
        return LaunchReply{};
    }
    // The clip a start event played: the region as resolved, which may since
    // have been edited in the document.
    const ResolvedClip* played(const LaunchEvent& e) const {
        const ResolvedClip& r = resolved_[index(e.track, e.scene)];
        if (r.clip == e.clip)
            return &r;
        const auto it = regions_.find(e.clip);
        return it == regions_.end() ? nullptr : &it->second.clip;
    }
    std::size_t index(std::uint32_t track, std::uint32_t scene) const {
        return static_cast<std::size_t>(scene) * tracks() + track;
    }
    LaunchCellView& view(std::uint32_t track, std::uint32_t scene) {
        return views_[index(track, scene)];
    }

    FakeLaunchConfig config_;
    std::unique_ptr<launch::StateMachineLauncher> launcher_;
    std::unique_ptr<ClipTable> table_;
    std::map<launch::ClipId, Region> regions_;
    std::vector<LaunchCellView> views_;
    std::vector<ResolvedClip> resolved_;
    std::vector<bool> armed_;
    std::vector<std::int32_t> recording_;
    std::vector<PendingClip> clip_ring_;
    std::vector<LaunchEvent> event_ring_;
    std::vector<std::int64_t> lost_frames_;
    std::vector<LaunchEvent> log_;
    bool session_open_ = true;
    bool take_open_ = false;
};

}  // namespace sonora
