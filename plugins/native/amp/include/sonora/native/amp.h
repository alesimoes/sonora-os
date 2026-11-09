// SPDX-License-Identifier: GPL-3.0-or-later
//
// Amp (ADR-0011 stage N3 §4.3, ADR-0111 §5-6): a NAM-format capture of an
// amplifier, a WaveNet or an LSTM from common/dsp/neural, run on the input.
// Mono at heart, as an amplifier is: a stereo input is summed to one
// signal, the capture runs once, and its output goes to every channel.
//
// The capture is data, not a parameter. A control thread loads it, never
// the audio thread (amp_loader.cpp):
// - The kernel is built and prepared, and when the session is not at the
//   capture's rate the resampler pair is designed (amp_resampler.h). Then a
//   q128 block is timed (§3.5's p99.9 and max, ADR-0111 §5): a capture over
//   the class-E budget is refused with a held notice naming the figure and
//   the budget, and never reaches the audio thread. It is never run at a
//   lower quality instead.
// - A capture that fits is handed over as the cabinet hands over a
//   response: two slots, a release store, an acquire load at a block
//   boundary, the wet path faded out over kFadeMs, swapped, held at zero
//   while the new network settles, and faded in. Nothing is allocated or
//   freed in process(). A load while the last is still being taken over
//   is Busy.
//
// Latency (changes only through prepare(), processor.h): the resampler
// pair's group delay when the capture's rate is not the session's, else 0,
// exactly. With no capture the amp reports what a 48 kHz capture (the
// format's default rate) would need, so loading the usual capture into a
// running session never moves PDC; with nothing loaded it passes the input
// through, delayed by that latency. A capture whose rate needs another
// latency is kept and Deferred: it plays from the next prepare().
//
// Level: "normalise" (on by default) trims the capture's output from its
// metadata.loudness to kReferenceDb; a capture without one stays at unity,
// and status() says which (the faceplate shows it).
//
// The capture is the amp's one file slot, "model" (`.nam`, files.h): the
// plugin state carries its path. Loading an empty path clears it.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "sonora/native/amp_model.h"
#include "sonora/native/processor_base.h"

namespace sonora::native {

namespace amp {
enum : std::uint32_t {
    kBypass = 0,
    kInput,
    kOutput,
    kMix,
    kNormalise,
    kParamCount,
};
constexpr double kFadeMs = 4.0;
// The level a capture with metadata.loudness is trimmed to: about where a
// DI'd guitar through a clean channel sits, so switching captures keeps
// the level and the input gain is what drives them.
constexpr double kReferenceDb = -18.0;
// A loudness further than this from the reference is not believed: the
// trim stops there.
constexpr double kMaxTrimDb = 24.0;

// ADR-0011 §3.5 class E, per q128 block: p99.9 and max.
struct Budget {
    double p999_us = 250.0;
    double max_us = 500.0;
};
// The load-time timing: blocks per run, runs (the per-block minimum).
constexpr int kTimedBlocks = 400;
constexpr int kTimedRuns = 2;
constexpr std::uint32_t kTimedBlock = 128;

// What a load hands the amp: a way to build the network (called once per
// prepare, so it is rebuilt for each session rate), and the capture's rate
// and loudness.
struct CaptureSource {
    std::function<std::unique_ptr<NeuralModel>(std::string* error)> make;
    double rate = 48000.0;
    bool has_loudness = false;
    double loudness_db = 0.0;
};

// For the faceplate and the tests. Control threads.
struct Status {
    bool loaded = false;         // a capture is playing (or is about to)
    bool has_loudness = false;   // it carries metadata.loudness
    double trim_db = 0.0;        // the normalisation applied, 0 when off or unknown
    double capture_rate = 0.0;
    double p999_us = 0.0, max_us = 0.0;  // its load-time timing
};
}  // namespace amp

class Amp : public ProcessorBase {
public:
    enum class LoadResult {
        Ok,
        Deferred,  // kept; needs another latency, so it plays from the next prepare()
        Busy,      // the previous capture is not taken over yet: retry
        Refused,   // not loaded; notice() says why
    };

    // Not for the audio thread. Safe from one control thread while
    // process() runs; before prepare() the capture is timed at its own rate
    // and kept for prepare(), which times it again at the session's.
    virtual LoadResult load_capture(amp::CaptureSource source, std::string& message) = 0;
    // The same from a parsed `.nam` (ampcab/nam_file.h).
    virtual LoadResult load_nam(const ampcab::NamModel& model, std::string& message) = 0;
    // Back to no capture (the input, delayed by the latency).
    virtual LoadResult clear_capture() = 0;

    // The last refusal, held until a capture loads or is cleared: what the
    // operator must see, not a passing status line. Empty when none.
    virtual std::string notice() const = 0;
    virtual amp::Status status() const = 0;

    // The budget a capture is held to. Before the amp is shared with
    // another thread. Class E unless a test says otherwise.
    virtual void set_budget(amp::Budget budget) = 0;

protected:
    Amp(const Descriptor& d) : ProcessorBase(d) {}
};

std::unique_ptr<Amp> make_amp();

}  // namespace sonora::native
