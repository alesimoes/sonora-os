#include "sonora/launch/state_machine_launcher.h"

namespace sonora::launch {

StateMachineLauncher::StateMachineLauncher(const LauncherConfig& config)
    : grid_(config.tracks, config.scenes),
      columns_(config.tracks),
      transitions_(static_cast<std::size_t>(config.tracks) * 2),
      queue_(config.queue_capacity),
      quantize_(config.quantize) {}

// ---- producer thread --------------------------------------------------------

bool StateMachineLauncher::enqueue(const Command& c) {
    if (queue_.push(c)) return true;
    overruns_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool StateMachineLauncher::assign_clip(std::uint32_t track, std::uint32_t scene, ClipId clip) {
    if (!grid_.contains(track, scene)) return false;
    Command c;
    c.kind = Command::Kind::AssignClip;
    c.track = track;
    c.scene = scene;
    c.clip = clip;
    return enqueue(c);
}

bool StateMachineLauncher::set_quantize(Quantize q) {
    Command c;
    c.kind = Command::Kind::SetQuantize;
    c.quantize = q;
    return enqueue(c);
}

bool StateMachineLauncher::trigger_cell(std::uint32_t track, std::uint32_t scene) {
    if (!grid_.contains(track, scene)) return false;
    Command c;
    c.kind = Command::Kind::TriggerCell;
    c.track = track;
    c.scene = scene;
    return enqueue(c);
}

bool StateMachineLauncher::trigger_scene(std::uint32_t scene) {
    if (scene >= grid_.scenes()) return false;
    Command c;
    c.kind = Command::Kind::TriggerScene;
    c.scene = scene;
    return enqueue(c);
}

bool StateMachineLauncher::stop_track(std::uint32_t track) {
    if (track >= grid_.tracks()) return false;
    Command c;
    c.kind = Command::Kind::StopTrack;
    c.track = track;
    return enqueue(c);
}

bool StateMachineLauncher::stop_all() {
    Command c;
    c.kind = Command::Kind::StopAll;
    return enqueue(c);
}

// ---- audio thread -----------------------------------------------------------

TransitionList StateMachineLauncher::advance(std::int64_t block_start, std::uint32_t frames) {
    transition_count_ = 0;
    dropped_ = 0;
    // Bounded by the capacity so a producer pushing while we drain cannot
    // keep the callback here.
    Command c;
    for (std::size_t i = 0; i < queue_.capacity() && queue_.pop(c); ++i) apply(c, block_start);
    if (frames > 0) {
        for (std::uint32_t t = 0; t < grid_.tracks(); ++t) fire_due(t, block_start, frames);
    }
    return TransitionList{transitions_.data(), transition_count_, dropped_};
}

void StateMachineLauncher::apply(const Command& c, std::int64_t now) {
    // A trigger is quantised from the block it is applied in.
    const std::int64_t at = next_boundary(now, transport_, quantize_);
    switch (c.kind) {
    case Command::Kind::AssignClip:
        grid_.cell(c.track, c.scene).clip = c.clip;
        break;
    case Command::Kind::SetQuantize:
        quantize_ = c.quantize;
        break;
    case Command::Kind::TriggerCell:
        queue_start(c.track, c.scene, at);
        break;
    case Command::Kind::TriggerScene:
        for (std::uint32_t t = 0; t < grid_.tracks(); ++t) queue_start(t, c.scene, at);
        break;
    case Command::Kind::StopTrack:
        queue_stop(c.track, at);
        break;
    case Command::Kind::StopAll:
        for (std::uint32_t t = 0; t < grid_.tracks(); ++t) queue_stop(t, at);
        break;
    }
}

void StateMachineLauncher::queue_start(std::uint32_t track, std::uint32_t scene, std::int64_t at) {
    // An empty slot stops its column, as a scene launch over a gap should.
    if (grid_.cell(track, scene).clip == kNoClip) {
        queue_stop(track, at);
        return;
    }
    clear_marks(track);
    Column& col = columns_[track];
    col.pending = Pending::Start;
    col.pending_scene = static_cast<std::int32_t>(scene);
    col.pending_frame = at;
    mark(track);
}

void StateMachineLauncher::queue_stop(std::uint32_t track, std::int64_t at) {
    clear_marks(track);
    Column& col = columns_[track];
    col.pending_scene = kNone;
    // Nothing sounding: cancelling a queued start needs no transition.
    col.pending = col.playing == kNone ? Pending::None : Pending::Stop;
    col.pending_frame = at;
    mark(track);
}

void StateMachineLauncher::fire_due(std::uint32_t track, std::int64_t block_start,
                                    std::uint32_t frames) {
    Column& col = columns_[track];
    if (col.pending == Pending::None) return;
    if (col.pending_frame >= block_start + static_cast<std::int64_t>(frames)) return;
    // A boundary already behind the block (the transport jumped) lands at once.
    const std::uint32_t offset = col.pending_frame <= block_start
        ? 0u : static_cast<std::uint32_t>(col.pending_frame - block_start);

    clear_marks(track);
    ClipId next = kNoClip;
    if (col.pending == Pending::Start)
        next = grid_.cell(track, static_cast<std::uint32_t>(col.pending_scene)).clip;
    if (col.playing != kNone) {
        emit(Transition::Kind::Stop, track, col.playing, col.playing_clip, offset);
        col.playing = kNone;
        col.playing_clip = kNoClip;
    }
    // The cell may have been emptied after the start was queued.
    if (next != kNoClip) {
        emit(Transition::Kind::Start, track, col.pending_scene, next, offset);
        col.playing = col.pending_scene;
        col.playing_clip = next;
    }
    col.pending = Pending::None;
    col.pending_scene = kNone;
    mark(track);
}

void StateMachineLauncher::emit(Transition::Kind kind, std::uint32_t track, std::int32_t scene,
                                ClipId clip, std::uint32_t offset) {
    if (transition_count_ == transitions_.size()) {
        ++dropped_;
        return;
    }
    Transition& tr = transitions_[transition_count_++];
    tr.kind = kind;
    tr.track = track;
    tr.scene = static_cast<std::uint32_t>(scene);
    tr.clip = clip;
    tr.offset = offset;
}

void StateMachineLauncher::clear_marks(std::uint32_t track) {
    const Column& col = columns_[track];
    if (col.playing != kNone)
        grid_.cell(track, static_cast<std::uint32_t>(col.playing)).state = CellState::Stopped;
    if (col.pending_scene != kNone)
        grid_.cell(track, static_cast<std::uint32_t>(col.pending_scene)).state = CellState::Stopped;
}

void StateMachineLauncher::mark(std::uint32_t track) {
    const Column& col = columns_[track];
    if (col.playing != kNone) {
        grid_.cell(track, static_cast<std::uint32_t>(col.playing)).state =
            col.pending == Pending::Stop ? CellState::QueuedStop : CellState::Playing;
    }
    // Written after the playing cell so a queued restart of it reads Queued.
    if (col.pending == Pending::Start)
        grid_.cell(track, static_cast<std::uint32_t>(col.pending_scene)).state = CellState::Queued;
}

}  // namespace sonora::launch
