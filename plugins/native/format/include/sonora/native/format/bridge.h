// SPDX-License-Identifier: GPL-3.0-or-later
//
// The format-neutral half of a plugin adapter (ADR-0011 §1.2): what sits
// between a plugin format's calls and a sonora::native::Processor. The DPF
// glue (src/dpf/) is a thin layer over this; a second adapter (the MIT VST3
// SDK, if DPF's VST3 ever fails the validator) would be another thin layer
// over the same class. Neutral C++17 (ADR-0008), and tested without any
// plugin format.
//
// Three things it settles that the formats leave open:
//
// - Threads. A format may set a parameter from any thread: LV2 from run(),
//   a VST3 host's setState() from its main thread while process() runs.
//   Values go through one atomic slot per parameter and are collected by
//   run(), so the Processor is only ever touched by the thread that runs it.
//
// - The first block. Values a host hands over before any audio has been
//   processed (LV2 control ports are only read in run(); a VST3 host's first
//   parameter queue) are the instance's initial state, not automation. They
//   are applied as jumps, so a freshly loaded session does not ramp every
//   parameter from its default over the smoothing time. After the first
//   block every change is an event at offset 0 and is smoothed (§2.4).
//
// - Reconfiguring parameters (ParamInfo::reconfigures: the EQ's mode, the
//   limiter's look-ahead). They change latency, which ADR-0011 §1.5 allows
//   only on a non-RT reconfiguration. A value set while inactive takes effect
//   at activate(); one set while running is held by the Processor until the
//   host next activates the instance, and latency() reports what the audio
//   actually does until then.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include "sonora/native/processor.h"
#include "sonora/rt.h"

namespace sonora::native::format {

class Bridge {
public:
    // `channels` is the bundle's fixed audio layout (DPF fixes it at compile
    // time). Not RT.
    Bridge(std::unique_ptr<Processor> processor, std::uint32_t channels);
    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;

    const Descriptor& descriptor() const noexcept { return processor_->descriptor(); }

    // Any thread, the audio thread included. The value is in plain units and
    // is sanitised (range, steps, non-finite) on the way in.
    void set_value(std::uint32_t index, float value) noexcept SONORA_RT;
    // The last value set, sanitised. Any thread.
    float value(std::uint32_t index) const noexcept;

    // Not RT. Applies every pending value as a jump, then prepares the
    // Processor for `sample_rate` and blocks of up to `max_frames`. The next
    // run() is a first block again. False when the Processor refuses.
    bool activate(double sample_rate, std::uint32_t max_frames);
    // The prepared configuration's latency, in samples. Changes only in
    // activate().
    std::uint32_t latency() const noexcept { return latency_; }

    // The audio thread. `in` and `out` are channels() buffers of `frames`
    // samples; they may alias. Any number of frames: the Processor splits
    // blocks longer than the prepared maximum.
    void run(const float* const* in, float* const* out, std::uint32_t frames,
             const Transport& transport) noexcept SONORA_RT;

    std::uint32_t channels() const noexcept { return channels_; }
    // For tests and the measurement harness. Not to be touched while run()
    // may be running.
    Processor& processor() noexcept { return *processor_; }

private:
    std::unique_ptr<Processor> processor_;
    const std::uint32_t channels_;
    const std::uint32_t count_;
    std::unique_ptr<std::atomic<float>[]> values_;
    std::unique_ptr<std::atomic<bool>[]> dirty_;
    // Set after any dirty_ flag, so run() scans the table only when
    // something changed.
    std::atomic<bool> any_dirty_{false};
    // One event per parameter at most per block: sized here, never grown.
    std::unique_ptr<ParamEvent[]> events_;
    std::uint32_t latency_ = 0;
    bool prepared_ = false;
    bool first_block_ = true;
};

}  // namespace sonora::native::format
