// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A linear chain of plugins, of any format, processing `channels` channels.
// A mono plugin in a multichannel chain is replicated once per channel; any
// other mismatch between what a plugin declares and the chain is refused.
//
// Every audio buffer the plugins see is private to this process and followed
// by a guard region of canary words. A plugin that writes more than it
// declared hits the canaries, which is how "declares 2 channels and writes 8"
// is caught instead of trusted.

#include "processor.h"
#include "sonora/plugin/protocol.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sonora::plugin_host {

class Chain {
public:
    Chain(PluginSource& source, const ChainConfig& config);
    ~Chain();
    Chain(const Chain&) = delete;
    Chain& operator=(const Chain&) = delete;

    bool add(const std::string& id, ChainError& error);
    // Run blocks of silence and check what each plugin did against what it
    // declared. Must pass before the chain is offered to the engine.
    bool validate(ChainError& error);

    // The processing thread. No allocation, no lock, no I/O. Every NaN or
    // Inf a plugin writes is replaced by 0 before the next plugin sees it and
    // counted against that plugin in report->instances[k].nonfinite_samples.
    // Returns false, with `error` set, when a plugin wrote outside its
    // buffers; the process must then be considered corrupt.
    bool process(const float* const* in, float* const* out, uint32_t frames,
                 plugin::ChannelHeader* report, ChainError& error) noexcept;

    // The main thread, concurrently with process(): Processor::control for
    // every plugin.
    void control();

    bool set_control(uint32_t instance, uint32_t id, float value) noexcept;
    bool set_control(uint32_t instance, const std::string& symbol, float value);

    std::size_t size() const { return slots_.size(); }
    uint32_t instance_latency(std::size_t i) const;
    uint32_t total_latency() const;
    uint32_t replicas(std::size_t i) const;
    const PluginInfo& info(std::size_t i) const;

private:
    float* buffer(std::size_t stage, uint32_t channel);
    void fill_canaries(uint32_t frames);
    bool canaries_intact(uint32_t from_frame, uint32_t words) const noexcept;
    uint64_t sanitise_stage(std::size_t stage, uint32_t frames) noexcept;

    PluginSource& source_;
    ChainConfig config_;
    std::vector<std::unique_ptr<Processor>> slots_;
    // Stage k is slot k's input and slot k-1's output. Each channel buffer is
    // max_frames samples followed by guard_frames_ canary words. The inner
    // vectors never resize, so the pointers processors hold stay valid.
    std::vector<std::vector<float>> stages_;
    uint32_t guard_frames_;
    uint32_t stride_;
};

}  // namespace sonora::plugin_host
