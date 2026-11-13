#include "sonora/launch/launch_clock.h"

namespace sonora::launch {

LaunchClock::LaunchClock(Launcher& launcher)
    // Two segments per block when the loop wraps, each up to 2 x tracks.
    : launcher_(launcher),
      merged_(static_cast<std::size_t>(launcher.grid().tracks()) * 4) {}

TransitionList LaunchClock::process(const BlockTransport& transport, std::uint32_t frames) {
    count_ = 0;
    dropped_ = 0;
    std::int64_t position = transport.position;
    std::uint32_t offset = 0;
    std::uint32_t remaining = frames;
    // At least one segment, so a zero-frame block still drains commands.
    do {
        const std::uint32_t length = segment_length(transport, position, remaining);
        run_segment(transport, position, length, offset);
        offset += length;
        remaining -= length;
        position += length;
        if (length > 0 && position == transport.loop_end && transport.rolling &&
            transport.looping) {
            position = transport.loop_start;
        }
    } while (remaining > 0);
    return TransitionList{merged_.data(), count_, dropped_};
}

std::uint32_t LaunchClock::segment_length(const BlockTransport& t, std::int64_t position,
                                          std::uint32_t remaining) {
    if (!t.rolling || !t.looping || t.loop_end <= t.loop_start) return remaining;
    if (position >= t.loop_end) return remaining;
    const std::int64_t to_wrap = t.loop_end - position;
    return to_wrap < static_cast<std::int64_t>(remaining) ? static_cast<std::uint32_t>(to_wrap)
                                                          : remaining;
}

void LaunchClock::run_segment(const BlockTransport& t, std::int64_t position,
                              std::uint32_t frames, std::uint32_t block_offset) {
    // Stopped: the grid keeps its anchor and free-runs with the clock.
    if (t.rolling) anchor_ = clock_ - (position - t.bar1_position);
    TransportSnapshot snapshot;
    snapshot.sample_rate = t.sample_rate;
    snapshot.tempo_bpm = t.tempo_bpm;
    snapshot.beats_per_bar = t.beats_per_bar;
    snapshot.anchor_frame = anchor_;
    launcher_.set_transport(snapshot);

    const TransitionList list = launcher_.advance(clock_, frames);
    dropped_ += list.dropped;
    for (std::uint32_t i = 0; i < list.count; ++i) {
        if (count_ == merged_.size()) {
            ++dropped_;
            continue;
        }
        Transition tr = list[i];
        tr.offset += block_offset;
        merged_[count_++] = tr;
    }
    clock_ += frames;
}

}  // namespace sonora::launch
