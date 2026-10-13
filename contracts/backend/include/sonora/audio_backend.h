// SPDX-License-Identifier: GPL-3.0-or-later
//
// Contract file (ADR-0007 §4.2, §6). Owners: rt-engineer and driver-engineer
// jointly. The seam between the engine and whatever moves audio for it. No
// backend's own headers (JACK, ALSA, ...) may appear here or in any header
// that implements it.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sonora/process_callback.h"

namespace sonora::backend {

// A physical port, as the server names it. `channel` is its position once
// every port is sorted with natural_less, so capture_AUX2 is before
// capture_AUX10: registration order and id order are not channel order.
struct PortInfo {
    std::string name;
    uint32_t channel;
};

struct BackendConfig {
    std::string client_name;
    uint32_t inputs;  // ports registered as in_1..in_N, in that order
    uint32_t outputs;  // out_1..out_N
    // ADR-0003: the session's rate. A server at any other rate fails open(),
    // since recording at it would write wrong-pitch files.
    uint32_t required_rate;
};

// Everything here except what the ProcessCallback receives is called from
// one non-real-time control thread, unless a member says otherwise.
class AudioBackend {
public:
    virtual ~AudioBackend() = default;

    // Usable before open(), so the session can be sized to the interface.
    // Empty if the server is unreachable or offers nothing.
    virtual std::vector<PortInfo> physical_capture_ports() = 0;
    virtual std::vector<PortInfo> physical_playback_ports() = 0;

    // Connects to the server and registers the ports. `callback` must outlive
    // the backend's activation. On failure returns false with `error` set,
    // and the backend can be destroyed or opened again.
    virtual bool open(const BackendConfig& config, ProcessCallback& callback,
                      std::string& error) = 0;
    // From here on callback.process() runs on the backend's thread.
    virtual bool activate(std::string& error) = 0;

    // Connects physical capture port k to input k, in PortInfo order.
    // Returns the connections made. Zero means every track records silence
    // and fewer than the input count means some do, so callers must not
    // ignore it.
    virtual int connect_inputs_in_order() = 0;
    // How many inputs have something connected right now. Asked at record
    // time, since a port can be unpatched after it was connected.
    virtual int connected_input_count() const = 0;

    // After return, process() will not be called again; safe to tear down.
    // Idempotent. The destructor calls it.
    virtual void deactivate() noexcept = 0;

    // Fixed from open() for the life of the session; see fatal().
    virtual uint32_t sample_rate() const = 0;
    virtual uint32_t buffer_frames() const = 0;
    // Server-reported xruns since open(). Any thread.
    virtual uint32_t xruns() const = 0;

    // Server loss, or a rate or buffer size change mid-session. From the
    // moment it is true, process() is not called again, so no block at the
    // wrong rate or size reaches the session. The engine closes the take
    // cleanly and says why (fatal_reason()); it never adapts silently. Any
    // thread. fatal_reason() is empty until fatal() is true, then fixed.
    virtual bool fatal() const = 0;
    virtual const std::string& fatal_reason() const = 0;
};

}  // namespace sonora::backend
