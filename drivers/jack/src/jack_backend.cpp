// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/jack_backend.h"

#include <jack/jack.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "sonora/period_guard.h"
#include "sonora/port_order.h"

namespace sonora {
namespace {

using backend::PortInfo;
using drivers::Fatal;

// Built once, so fatal_reason() can hand out a reference from any thread
// without formatting anything.
const std::string kNoReason;
const std::string kServerGone = "the audio server went away";
const std::string kRateChanged =
    "the audio server changed its sample rate mid-session";
const std::string kBufferChanged =
    "the audio server changed its buffer size mid-session";

// physical_*_ports() before open() connects under this name and disconnects
// again, so the probe never shows up as the engine in a patchbay.
constexpr const char* kProbeClient = "sonora-probe";

std::vector<PortInfo> list_physical(jack_client_t* client, unsigned long flags) {
    std::vector<std::string> names;
    if (const char** hw = jack_get_ports(client, nullptr, JACK_DEFAULT_AUDIO_TYPE,
                                         flags | JackPortIsPhysical)) {
        for (int i = 0; hw[i] != nullptr; ++i) names.emplace_back(hw[i]);
        jack_free(hw);
    }
    std::sort(names.begin(), names.end(), natural_less);

    std::vector<PortInfo> out;
    out.reserve(names.size());
    for (std::size_t i = 0; i < names.size(); ++i) {
        out.push_back({std::move(names[i]), static_cast<uint32_t>(i)});
    }
    return out;
}

}  // namespace

struct JackBackend::Impl {
    jack_client_t* client = nullptr;
    bool active = false;
    std::vector<jack_port_t*> in_ports;
    std::vector<jack_port_t*> out_ports;
    // Sized in open() and only overwritten afterwards, so the callback never
    // touches the allocator.
    std::vector<const float*> in_bufs;
    std::vector<float*> out_bufs;
    backend::AudioBlock block{};
    backend::ProcessCallback* callback = nullptr;
    uint32_t rate = 0;
    // Written on JACK's notification thread, read by the control thread.
    std::atomic<uint32_t> xruns{0};
    drivers::PeriodGuard guard;

    std::vector<PortInfo> physical(unsigned long flags) const {
        if (client) return list_physical(client, flags);
        jack_status_t status{};
        jack_client_t* probe =
            jack_client_open(kProbeClient, JackNoStartServer, &status);
        if (!probe) return {};
        std::vector<PortInfo> out = list_physical(probe, flags);
        jack_client_close(probe);
        return out;
    }

    bool register_ports(uint32_t count, const char* prefix, unsigned long flags,
                        std::vector<jack_port_t*>& ports, std::string& error) {
        ports.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            char name[32];
            std::snprintf(name, sizeof name, "%s_%u", prefix, i + 1);
            jack_port_t* p = jack_port_register(client, name,
                                                JACK_DEFAULT_AUDIO_TYPE, flags, 0);
            if (!p) {
                error = std::string("cannot register port ") + name;
                return false;
            }
            ports.push_back(p);
        }
        return true;
    }

    // The trampoline: fetch the port buffers, hand them over, nothing more
    // (ADR-0007 §2).
    static int process_cb(jack_nframes_t nframes, void* arg) noexcept SONORA_RT {
        auto* self = static_cast<Impl*>(arg);
        for (std::size_t o = 0; o < self->out_ports.size(); ++o) {
            self->out_bufs[o] = static_cast<float*>(
                jack_port_get_buffer(self->out_ports[o], nframes));
        }
        if (!self->guard.admit(nframes)) {
            // The engine no longer fills the outputs, and a JACK buffer holds
            // whatever the last cycle left in it.
            for (float* out : self->out_bufs) {
                std::memset(out, 0, nframes * sizeof(float));
            }
            return 0;
        }
        for (std::size_t i = 0; i < self->in_ports.size(); ++i) {
            self->in_bufs[i] = static_cast<const float*>(
                jack_port_get_buffer(self->in_ports[i], nframes));
        }
        self->block.nframes = nframes;
        self->block.frame_time = jack_last_frame_time(self->client);
        self->callback->process(self->block);
        return 0;
    }

    static int xrun_cb(void* arg) noexcept {
        static_cast<Impl*>(arg)->xruns.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    static int sample_rate_cb(jack_nframes_t rate, void* arg) noexcept {
        static_cast<Impl*>(arg)->guard.rate_reported(rate);
        return 0;
    }

    static void shutdown_cb(void* arg) noexcept {
        // Called from a JACK thread. Only flag it: the client handle is
        // already dead, so calling back into the API here is not allowed.
        static_cast<Impl*>(arg)->guard.raise(Fatal::ServerGone);
    }
};

JackBackend::JackBackend() : impl_(std::make_unique<Impl>()) {}

JackBackend::~JackBackend() {
    deactivate();
    if (impl_->client) jack_client_close(impl_->client);
}

std::vector<PortInfo> JackBackend::physical_capture_ports() {
    return impl_->physical(JackPortIsOutput);
}

std::vector<PortInfo> JackBackend::physical_playback_ports() {
    return impl_->physical(JackPortIsInput);
}

bool JackBackend::open(const backend::BackendConfig& config,
                       backend::ProcessCallback& callback, std::string& error) {
    if (impl_->client) {
        error = "the audio backend is already open";
        return false;
    }
    Impl& d = *impl_;
    const auto fail = [this, &d]() {
        jack_client_close(d.client);
        // A fresh Impl, so a later open() starts with no ports and no
        // fatal state left over from this attempt.
        impl_ = std::make_unique<Impl>();
        return false;
    };

    jack_status_t status{};
    d.client = jack_client_open(config.client_name.c_str(), JackNoStartServer,
                                &status);
    if (!d.client) {
        error = "cannot reach the audio server (is PipeWire running?)";
        return false;
    }

    // The server decides the rate; a mismatch would write wrong-pitch files.
    d.rate = jack_get_sample_rate(d.client);
    if (d.rate != config.required_rate) {
        error = "server runs at " + std::to_string(d.rate) +
                " Hz, session asked for " +
                std::to_string(config.required_rate) + " Hz";
        return fail();
    }

    if (!d.register_ports(config.inputs, "in", JackPortIsInput, d.in_ports,
                          error) ||
        !d.register_ports(config.outputs, "out", JackPortIsOutput, d.out_ports,
                          error)) {
        return fail();
    }
    d.in_bufs.assign(d.in_ports.size(), nullptr);
    d.out_bufs.assign(d.out_ports.size(), nullptr);
    d.block = backend::AudioBlock{
        d.in_bufs.data(),  static_cast<uint32_t>(d.in_bufs.size()),
        d.out_bufs.data(), static_cast<uint32_t>(d.out_bufs.size()),
        0, 0,
    };
    d.callback = &callback;

    // Before the callback is set: JACK calls it at once with the current
    // rate, which must not read as a change.
    d.guard.expect_rate(d.rate);
    jack_set_process_callback(d.client, Impl::process_cb, &d);
    jack_set_xrun_callback(d.client, Impl::xrun_cb, &d);
    jack_set_sample_rate_callback(d.client, Impl::sample_rate_cb, &d);
    jack_on_shutdown(d.client, Impl::shutdown_cb, &d);
    return true;
}

bool JackBackend::activate(std::string& error) {
    Impl& d = *impl_;
    if (!d.client) {
        error = "the audio backend is not open";
        return false;
    }
    if (d.active) return true;
    if (jack_activate(d.client) != 0) {
        error = "jack_activate failed";
        return false;
    }
    d.active = true;
    return true;
}

int JackBackend::connect_inputs_in_order() {
    Impl& d = *impl_;
    if (!d.client) return 0;

    const std::vector<PortInfo> hw = d.physical(JackPortIsOutput);
    if (hw.empty()) return 0;

    int connected = 0;
    const std::size_t n = std::min(hw.size(), d.in_ports.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (jack_connect(d.client, hw[i].name.c_str(),
                         jack_port_name(d.in_ports[i])) == 0) {
            ++connected;
        }
    }

    // A count mismatch either way is worth saying out loud. More tracks than
    // channels means the extra tracks record silence; more channels than
    // tracks means inputs the operator plugged in are not being captured.
    if (hw.size() < d.in_ports.size()) {
        std::fprintf(stderr,
                     "sonora: %zu capture channels for %zu tracks;"
                     " tracks %zu and above will be silent.\n",
                     hw.size(), d.in_ports.size(), hw.size() + 1);
    } else if (hw.size() > d.in_ports.size()) {
        std::fprintf(stderr,
                     "sonora: %zu capture channels but only %zu tracks;"
                     " channels %zu and above are not being recorded.\n",
                     hw.size(), d.in_ports.size(), d.in_ports.size() + 1);
    }

    return connected;
}

int JackBackend::connected_input_count() const {
    int n = 0;
    for (jack_port_t* p : impl_->in_ports) {
        if (jack_port_connected(p) > 0) ++n;
    }
    return n;
}

void JackBackend::deactivate() noexcept {
    Impl& d = *impl_;
    if (!d.client || !d.active) return;
    // Before the client is closed and before the caller frees the session,
    // so the callback cannot run against freed state.
    jack_deactivate(d.client);
    d.active = false;
}

uint32_t JackBackend::sample_rate() const { return impl_->rate; }

uint32_t JackBackend::buffer_frames() const {
    return impl_->client ? jack_get_buffer_size(impl_->client) : 0;
}

uint32_t JackBackend::xruns() const {
    return impl_->xruns.load(std::memory_order_relaxed);
}

bool JackBackend::fatal() const { return impl_->guard.fatal() != Fatal::None; }

const std::string& JackBackend::fatal_reason() const {
    switch (impl_->guard.fatal()) {
        case Fatal::ServerGone:    return kServerGone;
        case Fatal::RateChanged:   return kRateChanged;
        case Fatal::BufferChanged: return kBufferChanged;
        case Fatal::None:          break;
    }
    return kNoReason;
}

}  // namespace sonora
