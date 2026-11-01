// SPDX-License-Identifier: GPL-3.0-or-later
//
// The take being recorded, as the Edit window draws it while it grows: Pro
// Tools draws the new region under the playhead on every record-enabled
// track, so the operator sees that audio is arriving before the take is
// stopped, not after.
//
// Built from what the engine already publishes and nothing else: the state
// block's per-track input peak (EngineAdapter::take_peak, the same reading
// as the track's meter, pre-fader, measured on every input), its armed flag
// and the transport. No file is opened while the take runs: the growing BWF
// is the disk thread's, and a reader on the GUI thread would stall the
// window whenever the disk is slow, which is exactly when it is busiest.
//
// Each tick's peak is the maximum since the previous tick, so it belongs to
// the stretch of timeline the playhead covered between the two readings. It
// is kept in fixed buckets of timeline frames; the overview drawn from them
// is as coarse as the tick (about 800 frames at 48 kHz and 60 Hz), which is
// the live overview's resolution and no more. When the take closes, the
// engine's region replaces it.
//
// Where the take lands is the engine's rule, read from the session as
// engine/control's document_punch() and document_loop() read it: a punch
// range keeps the region inside [in, out), a loop take inside [A, B) with
// each pass starting again at A, and a plain take starts at the playhead.
// No Qt, so tests hold every frame of it.
#pragma once

#include <cstdint>
#include <vector>

#include "model_access.h"
#include "peak_source.h"

namespace sonora::ui {

class LiveTake {
public:
    // Timeline frames per stored peak. Finer than a 60 Hz tick at 48 kHz,
    // so a bucket never holds two ticks' worth of time; an hour of one
    // track is about 170 k floats.
    static constexpr model::Frames kBucket = 512;
    // Ticks a stopped take stays drawn waiting for the engine's region.
    // Past it the drawing goes: a take that failed or held nothing must not
    // leave a region on screen that the session does not have.
    static constexpr int kHoldTicks = 120;

    struct Lane {
        int row = -1;                // display row, which is the engine track
        model::Frames start = 0;     // timeline frame of the first bucket
        model::Frames end = 0;       // where the region reaches now
        std::vector<float> peaks;    // one per kBucket from `start`, 0..1
    };

    // One reading. `recording`: the transport records now (not counting
    // off). `armed[i]` and `peaks[i]` are engine track i's, `peaks` read
    // this tick for every armed track. `audio[i]`: row i is an audio track,
    // the only kind a take writes. `settings` is where the region lands.
    void tick(bool recording, model::Frames playhead, const std::vector<bool>& armed,
              const std::vector<bool>& audio, const std::vector<float>& peaks,
              const model::Transport& settings);

    // The session's region arrived, a session opened or the engine went:
    // nothing is drawn.
    void clear();

    bool empty() const { return lanes_.empty(); }
    bool recording() const { return recording_; }
    const std::vector<Lane>& lanes() const { return lanes_; }

    // As PeakSource::peaks, for lane `lane` in timeline frames: `count`
    // columns of `frames_per_bucket` from `first`, symmetric about zero
    // since the meter's peak is unsigned.
    void peaks(const Lane& lane, model::Frames first, double frames_per_bucket, int count,
               PeakPair* out) const;

private:
    void start_lane(int row, model::Frames at);

    std::vector<Lane> lanes_;
    bool recording_ = false;
    bool seen_ = false;            // `last_` is this take's
    model::Frames last_ = 0;       // the playhead at the previous reading
    int stopped_ticks_ = 0;
};

}  // namespace sonora::ui
