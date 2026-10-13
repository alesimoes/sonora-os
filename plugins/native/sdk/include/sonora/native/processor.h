// SPDX-License-Identifier: GPL-3.0-or-later
//
// sonora::native::Processor, the one interface every native processor
// implements (ADR-0011 §1.2). The format adapters (LV2, VST3; N1) translate
// it; the measurement harness renders through it; a promotion into the
// engine (§1.4) would call it directly. No processor sees a plugin format.
#pragma once

#include <cstdint>

#include "sonora/native/param.h"
#include "sonora/rt.h"

namespace sonora::native {

struct Descriptor {
    const char* id;        // reverse-domain, stable: "org.sonora.eq"
    const char* name;      // ours (ADR-0011 §5)
    const char* category;  // "EQ", "Dynamics", "Delay", "Modulation"
    std::uint32_t state_version;
    const ParamInfo* params;
    std::uint32_t param_count;
    std::uint32_t min_channels, max_channels;
    bool sidechain;  // accepts a key input
};

// A parameter change at a sample offset inside the block. Events are
// sorted by offset; the VST3 adapter passes VST3's offsets through, the LV2
// adapter delivers at offset 0 (ADR-0011 §2.4, "Automation").
struct ParamEvent {
    std::uint32_t offset;
    std::uint32_t index;
    float value;  // plain units, not normalised
};

struct Transport {
    double tempo_bpm = 120.0;
    bool tempo_valid = false;  // false: processors use their own tempo parameter
};

struct AudioBuffers {
    const float* const* in;
    float* const* out;  // may alias `in`
    const float* const* sidechain;  // null when the host has none
    std::uint32_t channels;
    std::uint32_t sidechain_channels;
    std::uint32_t frames;
};

class Processor {
public:
    virtual ~Processor() = default;

    virtual const Descriptor& descriptor() const noexcept = 0;

    // Not real-time: allocates every buffer, FFT plan and table. Applies
    // `reconfigures` parameters. Returns false for an unsupported layout.
    virtual bool prepare(double sample_rate, std::uint32_t max_frames,
                         std::uint32_t channels) = 0;
    // Clears all signal state (filters, delay lines, detectors), keeping
    // parameters. Real-time safe.
    virtual void reset() noexcept = 0;
    // Exact latency in samples for the prepared configuration. Changes only
    // through prepare().
    virtual std::uint32_t latency() const noexcept = 0;

    virtual void process(const AudioBuffers& io, const ParamEvent* events,
                         std::uint32_t event_count, const Transport& transport) noexcept SONORA_RT = 0;

    virtual float param(std::uint32_t index) const noexcept = 0;
    // Not real-time: sets a value at once, without smoothing. For state
    // restore and offline setup; a playing host sends events instead.
    virtual bool set_param(std::uint32_t index, float value) noexcept = 0;
};

}  // namespace sonora::native
