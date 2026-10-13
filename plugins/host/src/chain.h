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

    // Parameters by dense index (contracts/plugin). The processing thread,
    // between blocks; false when the plugin will not take it.
    bool set_param(uint32_t instance, uint32_t dense, float value, uint32_t offset) noexcept;
    bool replace_param(uint32_t instance, uint32_t dense, float value) noexcept;
    // --set, by symbol, before validation.
    bool set_control(uint32_t instance, const std::string& symbol, float value);
    uint32_t param_count(std::size_t i) const;
    std::string param_key(std::size_t i, uint32_t dense) const;
    float param_default(std::size_t i, uint32_t dense) const;

    // kBypassParam (ADR-0012 §5.1), latency-compensated so a toggle never
    // shifts timing: through the plugin's own bypass parameter when it has
    // one, otherwise by playing the instance's input delayed by its reported
    // latency, crossfaded over one block. Host-side bypass applies at the
    // block start. The processing thread.
    bool set_bypass(uint32_t instance, bool bypassed, uint32_t offset) noexcept;
    bool designated_bypass(std::size_t i) const;
    // After validate(), not RT: the delay lines for host-side bypass, sized
    // from the latency each plugin reported then, with headroom. A plugin
    // whose latency later grows past the headroom is bypassed with the
    // delay clamped there (kBypassHeadroomFrames).
    void prepare_bypass();

    std::size_t size() const { return slots_.size(); }
    uint32_t instance_latency(std::size_t i) const;
    uint32_t total_latency() const;
    uint32_t replicas(std::size_t i) const;
    const PluginInfo& info(std::size_t i) const;

    static constexpr uint32_t kBypassHeadroomFrames = 4096;

private:
    float* buffer(std::size_t stage, uint32_t channel);
    void fill_canaries(uint32_t frames);
    bool canaries_intact(uint32_t from_frame, uint32_t words) const noexcept;
    uint64_t sanitise_stage(std::size_t stage, uint32_t frames) noexcept;
    void run_bypass(std::size_t k, uint32_t frames) noexcept;

    struct BypassLine {
        bool on = false, was = false;
        uint32_t mask = 0, write = 0;
        std::vector<std::vector<float>> ring;  // per channel, power-of-two size
    };

    PluginSource& source_;
    ChainConfig config_;
    std::vector<std::unique_ptr<Processor>> slots_;
    std::vector<BypassLine> bypass_;  // one per slot; ring empty when designated
    // Stage k is slot k's input and slot k-1's output. Each channel buffer is
    // max_frames samples followed by guard_frames_ canary words. The inner
    // vectors never resize, so the pointers processors hold stay valid.
    std::vector<std::vector<float>> stages_;
    uint32_t guard_frames_;
    uint32_t stride_;
};

}  // namespace sonora::plugin_host
