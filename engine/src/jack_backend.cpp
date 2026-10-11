// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/jack_backend.h"

#include <cstdio>

#include "sonora/session.h"

namespace sonora {

JackBackend::~JackBackend() { close(); }

bool JackBackend::open(const std::string& client_name, Session* session) {
    jack_status_t status{};
    client_ = jack_client_open(client_name.c_str(), JackNoStartServer, &status);
    if (!client_) {
        error_ = "cannot reach the audio server (is PipeWire running?)";
        return false;
    }
    session_ = session;

    const std::size_t n = session->tracks().size();
    in_ports_.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        char port[32];
        std::snprintf(port, sizeof(port), "in_%zu", i + 1);
        jack_port_t* p = jack_port_register(client_, port,
                                            JACK_DEFAULT_AUDIO_TYPE,
                                            JackPortIsInput, 0);
        if (!p) {
            error_ = std::string("cannot register port ") + port;
            return false;
        }
        in_ports_.push_back(p);
    }
    // Sized once here so process_cb never touches the allocator.
    in_bufs_.assign(n, nullptr);

    jack_set_process_callback(client_, process_cb, this);
    jack_set_xrun_callback(client_, xrun_cb, this);
    jack_on_shutdown(client_, shutdown_cb, this);
    return true;
}

int JackBackend::auto_connect_inputs() {
    if (!client_) return 0;
    const char** hw = jack_get_ports(client_, nullptr, JACK_DEFAULT_AUDIO_TYPE,
                                     JackPortIsOutput | JackPortIsPhysical);
    if (!hw) return 0;

    int connected = 0;
    for (std::size_t i = 0; i < in_ports_.size() && hw[i] != nullptr; ++i) {
        if (jack_connect(client_, hw[i], jack_port_name(in_ports_[i])) == 0) {
            ++connected;
        }
    }
    jack_free(hw);
    return connected;
}

int JackBackend::connected_input_count() const {
    int n = 0;
    for (jack_port_t* p : in_ports_) {
        if (jack_port_connected(p) > 0) ++n;
    }
    return n;
}

bool JackBackend::activate() {
    if (!client_) return false;
    if (jack_activate(client_) != 0) {
        error_ = "jack_activate failed";
        return false;
    }
    return true;
}

void JackBackend::close() {
    if (!client_) return;
    // Deactivate first so the callback cannot run against freed state.
    jack_deactivate(client_);
    jack_client_close(client_);
    client_ = nullptr;
    in_ports_.clear();
}

uint32_t JackBackend::sample_rate() const {
    return client_ ? jack_get_sample_rate(client_) : 0;
}

uint32_t JackBackend::buffer_size() const {
    return client_ ? jack_get_buffer_size(client_) : 0;
}

int JackBackend::process_cb(jack_nframes_t nframes, void* arg) {
    auto* self = static_cast<JackBackend*>(arg);
    for (std::size_t i = 0; i < self->in_ports_.size(); ++i) {
        self->in_bufs_[i] = static_cast<const float*>(
            jack_port_get_buffer(self->in_ports_[i], nframes));
    }
    self->session_->process(self->in_bufs_.data(), nframes);
    return 0;
}

int JackBackend::xrun_cb(void* arg) {
    static_cast<JackBackend*>(arg)->xruns_++;
    return 0;
}

void JackBackend::shutdown_cb(void* arg) {
    // Called from a JACK thread. Only flag it: the client handle is
    // already dead, so calling back into the API here is not allowed.
    static_cast<JackBackend*>(arg)->server_gone_.store(
        true, std::memory_order_release);
}

}  // namespace sonora
