// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "sonora/alsa_device.h"
#include "sonora/audio_backend.h"

namespace sonora {

// AudioBackend straight on ALSA (ADR-0053): mmap interleaved, the card's own
// period as the engine's block, one SCHED_FIFO thread. There is no server, so
// nothing else plays through the card while this runs and no other program
// can hold it: starting on a card PipeWire owns fails and says so. Use it on
// the installed system when the last millisecond matters; ADR-0003's
// PipeWire path stays the default.
//
// Differences from a server backend, all inside the AudioBackend contract:
//  - there is no clock until a card is chosen and present, so periods start
//    at activate() if the card was chosen at open(), else at the first
//    connect_inputs_in_order() that finds it;
//  - "connecting" opens the card, so connect_inputs_in_order() is where a
//    card held by another program reports its error (to stderr, once);
//  - the card is the whole graph: its capture channel k is input k.
class AlsaBackend final : public backend::AudioBackend {
public:
    struct Options {
        uint32_t period_frames = 128;
        uint32_t periods = 2;  // the ring, in periods; 2 is the least that streams
        int rt_priority = 70;  // SCHED_FIFO; below the 95 limits-audio.conf allows
    };

    AlsaBackend();
    explicit AlsaBackend(const Options& options);
    // `system` is libasound's in production (alsa::make_system()); tests pass a fake.
    AlsaBackend(const Options& options, std::unique_ptr<alsa::System> system);
    ~AlsaBackend() override;
    AlsaBackend(const AlsaBackend&) = delete;
    AlsaBackend& operator=(const AlsaBackend&) = delete;

    std::vector<backend::PortInfo> physical_capture_ports() override;
    std::vector<backend::PortInfo> physical_playback_ports() override;

    bool open(const backend::BackendConfig& config, backend::ProcessCallback& callback,
              std::string& error) override;
    bool activate(std::string& error) override;
    int connect_inputs_in_order() override;
    int connected_input_count() const override;
    bool input_connected(uint32_t index) const override;
    bool choose_capture_device(const std::string& device) override;
    void deactivate() noexcept override;

    uint32_t sample_rate() const override;
    uint32_t buffer_frames() const override;
    uint32_t xruns() const override;
    uint32_t clock_discontinuities() const override;
    bool fatal() const override;
    std::string fatal_reason() const override;
    std::string capture_device() const override;
    uint32_t reported_latency() const override;

    // SCHED_FIFO priority the thread got; 0 while none runs or the kernel
    // refused (no rtprio limit for this user). Any thread.
    int rt_priority_granted() const;
    // Frames the card's ring holds, as granted; 0 before a stream runs.
    uint32_t ring_frames() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sonora
