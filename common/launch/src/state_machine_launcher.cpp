#include "sonora/launch/state_machine_launcher.h"

#include <cmath>

namespace sonora::launch {

StateMachineLauncher::StateMachineLauncher(const LauncherConfig& config)
    : grid_(config.tracks, config.scenes),
      columns_(config.tracks),
      transitions_(static_cast<std::size_t>(config.tracks) * 2),
      queue_(config.queue_capacity),
      quantize_(config.quantize) {}

// ---- producer thread --------------------------------------------------------

bool StateMachineLauncher::enqueue(Command c) {
    c.arrival = published_.load(std::memory_order_acquire);
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

bool StateMachineLauncher::arm_record(std::uint32_t track, std::uint32_t scene, ClipId clip,
                                      std::uint32_t bars) {
    // A capture needs the id of the region it will become.
    if (!grid_.contains(track, scene) || clip == kNoClip) return false;
    Command c;
    c.kind = Command::Kind::ArmRecord;
    c.track = track;
    c.scene = scene;
    c.clip = clip;
    c.bars = bars;
    return enqueue(c);
}

// ---- audio thread -----------------------------------------------------------

void StateMachineLauncher::set_transport(const TransportSnapshot& t) {
    const bool moved = t.sample_rate != transport_.sample_rate ||
                       t.tempo_bpm != transport_.tempo_bpm ||
                       t.beats_per_bar != transport_.beats_per_bar ||
                       t.anchor_frame != transport_.anchor_frame;
    transport_ = t;
    if (!moved || clock_ == kUnknown) return;
    // A pending frame was the first boundary after its arrival on the old
    // grid; on the new grid that is the first boundary from now on.
    for (Column& col : columns_) {
        if (col.pending == Pending::None || !col.pending_resnaps) continue;
        if (col.pending_frame < clock_) continue;
        col.pending_frame = next_boundary(clock_, transport_, col.pending_quantize);
    }
}

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
    clock_ = block_start + static_cast<std::int64_t>(frames);
    published_.store(clock_, std::memory_order_release);
    return TransitionList{transitions_.data(), transition_count_, dropped_};
}

void StateMachineLauncher::apply(const Command& c, std::int64_t now) {
    // Before the first block there is no arrival stamp; a block_start behind
    // the stamp means the caller's clock went back, and now is all we know.
    const std::int64_t from = (c.arrival == kUnknown || c.arrival > now) ? now : c.arrival;
    const std::int64_t at = next_boundary(from, transport_, quantize_);
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
    case Command::Kind::ArmRecord:
        queue_record(c, at);
        break;
    }
}

void StateMachineLauncher::set_pending(std::uint32_t track, Pending p, std::int32_t scene,
                                       std::int64_t at) {
    Column& col = columns_[track];
    col.pending = p;
    col.pending_scene = scene;
    col.pending_frame = at;
    col.pending_quantize = quantize_;
    col.pending_resnaps = quantize_ != Quantize::Immediate;
}

void StateMachineLauncher::queue_start(std::uint32_t track, std::uint32_t scene, std::int64_t at) {
    // Triggering the cell being recorded ends the capture and plays it,
    // although the cell gets its clip only when the capture ends.
    const bool finishes_capture = columns_[track].recording == static_cast<std::int32_t>(scene);
    // An empty slot stops its column, as a scene launch over a gap should.
    if (grid_.cell(track, scene).clip == kNoClip && !finishes_capture) {
        queue_stop(track, at);
        return;
    }
    clear_marks(track);
    set_pending(track, Pending::Start, static_cast<std::int32_t>(scene), at);
    mark(track);
}

void StateMachineLauncher::queue_stop(std::uint32_t track, std::int64_t at) {
    clear_marks(track);
    const Column& col = columns_[track];
    // Nothing sounding or capturing: cancelling a queued start needs no
    // transition.
    const bool idle = col.playing == kNone && col.recording == kNone;
    set_pending(track, idle ? Pending::None : Pending::Stop, kNone, at);
    mark(track);
}

void StateMachineLauncher::queue_record(const Command& c, std::int64_t at) {
    clear_marks(c.track);
    set_pending(c.track, Pending::Record, static_cast<std::int32_t>(c.scene), at);
    Column& col = columns_[c.track];
    col.pending_clip = c.clip;
    col.pending_bars = c.bars;
    mark(c.track);
}

void StateMachineLauncher::fire_due(std::uint32_t track, std::int64_t block_start,
                                    std::uint32_t frames) {
    Column& col = columns_[track];
    if (col.pending == Pending::None) return;
    if (col.pending_frame >= block_start + static_cast<std::int64_t>(frames)) return;
    // A boundary already behind the block (a late command) lands at once.
    const std::uint32_t offset = col.pending_frame <= block_start
        ? 0u : static_cast<std::uint32_t>(col.pending_frame - block_start);

    clear_marks(track);
    const Pending action = col.pending;
    const std::int32_t scene = col.pending_scene;
    col.pending = Pending::None;
    col.pending_scene = kNone;
    if (col.playing != kNone) {
        emit(Transition::Kind::Stop, track, col.playing, col.playing_clip, offset);
        col.playing = kNone;
        col.playing_clip = kNoClip;
    }
    if (col.recording != kNone) end_capture(track, offset);
    if (action == Pending::Start) {
        // The cell may have been emptied after the start was queued.
        const ClipId next = grid_.cell(track, static_cast<std::uint32_t>(scene)).clip;
        if (next != kNoClip) {
            emit(Transition::Kind::Start, track, scene, next, offset);
            col.playing = scene;
            col.playing_clip = next;
        }
    } else if (action == Pending::Record) {
        col.recording = scene;
        begin_capture(track, block_start + offset, offset);
    }
    mark(track);
}

void StateMachineLauncher::end_capture(std::uint32_t track, std::uint32_t offset) {
    Column& col = columns_[track];
    emit(Transition::Kind::RecordStop, track, col.recording, col.recording_clip, offset);
    grid_.cell(track, static_cast<std::uint32_t>(col.recording)).clip = col.recording_clip;
    col.recording = kNone;
    col.recording_clip = kNoClip;
}

void StateMachineLauncher::begin_capture(std::uint32_t track, std::int64_t frame,
                                         std::uint32_t offset) {
    Column& col = columns_[track];
    col.recording_clip = col.pending_clip;
    emit(Transition::Kind::RecordStart, track, col.recording, col.recording_clip, offset);
    const double bar = step_frames(transport_, Quantize::Bar);
    if (col.pending_bars == 0 || !(bar > 0.0)) return;
    // A fixed-length capture ends by starting its own cell: the capture
    // closes and the new clip plays, on the exact frame bars x bar later.
    col.pending = Pending::Start;
    col.pending_scene = col.recording;
    col.pending_frame =
        frame + static_cast<std::int64_t>(std::floor(col.pending_bars * bar + 0.5));
    col.pending_quantize = Quantize::Bar;
    col.pending_resnaps = false;
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
    const std::int32_t scenes[] = {col.playing, col.recording, col.pending_scene};
    for (const std::int32_t s : scenes) {
        if (s != kNone) grid_.cell(track, static_cast<std::uint32_t>(s)).state = CellState::Stopped;
    }
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
    if (col.pending == Pending::Record)
        grid_.cell(track, static_cast<std::uint32_t>(col.pending_scene)).state = CellState::Armed;
    // Last: a capture whose end is queued still reads Recording until it ends.
    if (col.recording != kNone)
        grid_.cell(track, static_cast<std::uint32_t>(col.recording)).state = CellState::Recording;
}

}  // namespace sonora::launch
