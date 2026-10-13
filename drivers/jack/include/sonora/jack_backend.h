// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "sonora/audio_backend.h"

namespace sonora {

// AudioBackend over the JACK API. On the target system this binds to
// PipeWire's JACK implementation, so no jackd is involved. <jack/jack.h> is
// included by the implementation only, so nothing that links this target
// compiles against JACK (ADR-0007 §1).
class JackBackend final : public backend::AudioBackend {
public:
    // Once, before any thread that reads the environment exists and before
    // the first JackBackend: turns on pw-jack's port aliases through
    // PIPEWIRE_PROPS, which is how the backend tells a USB interface from
    // onboard audio. Keeps whatever PIPEWIRE_PROPS already holds.
    static void prepare_process();

    JackBackend();
    ~JackBackend() override;
    JackBackend(const JackBackend&) = delete;
    JackBackend& operator=(const JackBackend&) = delete;

    // Before open() these ask the server through a throwaway client: the
    // session has to be sized before this backend can register its ports.
    std::vector<backend::PortInfo> physical_capture_ports() override;
    std::vector<backend::PortInfo> physical_playback_ports() override;

    bool open(const backend::BackendConfig& config,
              backend::ProcessCallback& callback, std::string& error) override;
    bool activate(std::string& error) override;
    int connect_inputs_in_order() override;
    int connected_input_count() const override;
    void deactivate() noexcept override;

    uint32_t sample_rate() const override;
    uint32_t buffer_frames() const override;
    uint32_t xruns() const override;
    bool fatal() const override;
    std::string fatal_reason() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sonora
