// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <jack/jack.h>

#include <atomic>
#include <string>
#include <vector>

namespace sonora {

class Session;

// Audio backend speaking the JACK API. On the target system this binds to
// PipeWire's JACK implementation, so no jackd is involved.
// Counts the physical capture channels the server is offering, using a
// throwaway client. The session has to be sized before the real backend can
// register its ports, so this cannot be a member: it answers the question that
// decides how many tracks to create.
//
// Returns 0 if the server is unreachable or offering nothing.
int probe_capture_channels(const std::string& client_name);

class JackBackend {
public:
    JackBackend() = default;
    ~JackBackend();

    // Returns false and fills error() if the server is unreachable.
    bool open(const std::string& client_name, Session* session);

    // Physical capture ports the server is offering, in channel order.
    // Used to size a session to the interface instead of guessing.
    std::vector<std::string> capture_ports() const;

    // Connects each input port to the matching hardware capture port.
    // Returns how many connections were made. Zero means every track will
    // record silence, and fewer than the track count means some tracks will,
    // so callers must not ignore it.
    int auto_connect_inputs();

    // How many of our input ports have something connected to them right now.
    // Asked at record time rather than trusting the connect-time count, since
    // a port can be unpatched after we connected it.
    int connected_input_count() const;

    bool activate();
    void close();

    uint32_t sample_rate() const;
    uint32_t buffer_size() const;
    // Count of JACK xruns since open: the headline stability number.
    uint32_t xruns() const { return xruns_; }

    // True once the server has gone away. The callback stops being
    // called at that point, so a recording in progress would sit there
    // looking healthy while receiving nothing. Callers must poll this.
    bool server_gone() const {
        return server_gone_.load(std::memory_order_acquire);
    }

    const std::string& error() const { return error_; }

private:
    static int process_cb(jack_nframes_t nframes, void* arg);
    static int xrun_cb(void* arg);
    static void shutdown_cb(void* arg);

    jack_client_t* client_ = nullptr;
    std::vector<jack_port_t*> in_ports_;
    std::vector<const float*> in_bufs_;  // reused each block, never allocated
    Session* session_ = nullptr;
    uint32_t xruns_ = 0;
    // Written from the JACK shutdown thread, read from main.
    std::atomic<bool> server_gone_{false};
    std::string error_;
};

}  // namespace sonora
