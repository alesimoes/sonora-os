// SPDX-License-Identifier: GPL-3.0-or-later
//
// Where waveform overviews come from. The timeline asks for min/max pairs
// over a run of equal buckets of a source; it never reads audio. The real
// implementation reads the peak files the engine writes beside each take
// (Phase 3, ui.editing.waveform); until then SyntheticPeakSource draws a
// deterministic envelope, and the Edit window says the session is synthetic.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "model_access.h"

namespace sonora::ui {

struct PeakPair {
    float min = 0.0f;  // -1 .. 0
    float max = 0.0f;  //  0 .. 1
};

class PeakSource {
public:
    virtual ~PeakSource() = default;
    // `count` buckets of `frames_per_bucket` source frames each, the first
    // starting at source frame `first`. A bucket outside the source is
    // silence. Runs on the GUI thread when the timeline is rebuilt, so it
    // must answer from memory, never from disk.
    virtual void peaks(model::Id source, uint16_t channel, model::Frames first,
                       double frames_per_bucket, int count, PeakPair* out) const = 0;
    // False while a source's overview is still being read or built: peaks()
    // answers silence for it, which is not what the audio holds, so anything
    // that decides by the answer (Strip Silence) must wait.
    virtual bool ready(model::Id source) const {
        (void)source;
        return true;
    }
};

// The overview files beside the takes (common/audiofile/peak_file.h). A
// source's file is read, or built from its WAV when it has none or it is
// stale, on this class's own worker thread, never on the GUI thread and
// never on the engine's audio thread; peaks() answers from memory only. A
// source not loaded yet draws as silence and `on_ready` (called from the
// worker) says to redraw once it is. A source that cannot be read stays
// silent and not ready.
class FilePeakSource final : public PeakSource {
public:
    struct Located {
        std::string wav;    // absolute
        std::string peak;   // absolute
        model::Frames length = 0;
    };
    // `locate` runs on the GUI thread, inside peaks() and ready(); false for
    // a source it cannot place (no session folder, no such source).
    using Locate = std::function<bool(model::Id, Located&)>;
    FilePeakSource(Locate locate, std::function<void()> on_ready);
    ~FilePeakSource() override;
    void peaks(model::Id source, uint16_t channel, model::Frames first, double frames_per_bucket,
               int count, PeakPair* out) const override;
    bool ready(model::Id source) const override;
    // Blocks until the worker has nothing queued. For the tests.
    void wait_idle() const;

private:
    struct Entry;
    std::shared_ptr<Entry> entry(model::Id source) const;
    void work();

    Locate locate_;
    std::function<void()> on_ready_;
    mutable std::mutex m_;
    mutable std::condition_variable cv_, idle_;
    mutable std::map<model::Id, std::shared_ptr<Entry>> entries_;
    mutable std::vector<std::shared_ptr<Entry>> queue_;
    mutable bool busy_ = false;
    bool stop_ = false;
    std::thread worker_;
};

class SyntheticPeakSource final : public PeakSource {
public:
    SyntheticPeakSource(uint32_t sample_rate, double bpm) : rate_(sample_rate), bpm_(bpm) {}
    void peaks(model::Id source, uint16_t channel, model::Frames first,
               double frames_per_bucket, int count, PeakPair* out) const override;

    // The envelope at one source frame, 0..1. Public for the tests.
    float envelope(model::Id source, model::Frames frame) const;

private:
    uint32_t rate_;
    double bpm_;
};

}  // namespace sonora::ui
