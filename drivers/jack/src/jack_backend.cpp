// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/jack_backend.h"

#include <jack/jack.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "sonora/alsa_card.h"
#include "sonora/fatal_reason.h"
#include "sonora/frame_clock.h"
#include "sonora/period_guard.h"
#include "sonora/port_devices.h"
#include "sonora/source_watch.h"

namespace sonora {
namespace {

using backend::PortInfo;
using drivers::Fatal;

// physical_*_ports() before open() connects under this name and disconnects
// again, so the probe never shows up as the engine in a patchbay.
constexpr const char* kProbeClient = "sonora-probe";

// The scratch buffers stand in for a port buffer JACK does not hand over, so
// the largest period admitted is bounded by them. PipeWire's own ceiling on
// the quantum is 8192.
constexpr uint32_t kMinScratchFrames = 8192;

uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// Under pw-jack 1.4.2 with jack.fill-aliases on (prepare_process()), a
// port's first alias is its PipeWire object.path, which names the ALSA
// card; the kernel says whether that card is USB. Nothing in the JACK API
// names the bus, and the client name is the node's description ("UMC1820
// Pro"), which does not either. Under jackd2, or with aliases off, no port
// reads as USB, and a device is then used only when named.
bool port_on_usb(jack_client_t* client, const char* name) {
    jack_port_t* port = jack_port_by_name(client, name);
    if (!port) return false;
    const std::size_t size = static_cast<std::size_t>(jack_port_name_size());
    std::vector<char> first(size, '\0'), second(size, '\0');
    char* aliases[2] = {first.data(), second.data()};
    const int n = jack_port_get_aliases(port, aliases);
    for (int i = 0; i < n && i < 2; ++i) {
        const std::string card = drivers::alsa_card_of(aliases[i]);
        if (!card.empty()) return drivers::alsa_card_is_usb(card);
    }
    return false;
}

std::vector<PortInfo> list_physical(jack_client_t* client, unsigned long flags) {
    std::vector<std::string> names;
    std::vector<std::string> usb;
    if (const char** hw = jack_get_ports(client, nullptr, JACK_DEFAULT_AUDIO_TYPE,
                                         flags | JackPortIsPhysical)) {
        for (int i = 0; hw[i] != nullptr; ++i) {
            names.emplace_back(hw[i]);
            if (port_on_usb(client, hw[i])) usb.emplace_back(hw[i]);
        }
        jack_free(hw);
    }
    return drivers::describe_ports(std::move(names), [&usb](const std::string& n) {
        return std::find(usb.begin(), usb.end(), n) != usb.end();
    });
}

}  // namespace

struct JackBackend::Impl {
    jack_client_t* client = nullptr;
    bool active = false;
    // Empty until a device is chosen: by name in open(), by the default
    // policy at open() or at the first connection after it, or by the
    // caller's fallback (choose_capture_device()). Never changed once set.
    std::string capture_device;
    // What connect_inputs_in_order() last said, so a caller that polls it
    // while waiting for an interface does not repeat it every time.
    std::string last_complaint;
    std::vector<jack_port_t*> in_ports;
    std::vector<jack_port_t*> out_ports;
    // Sized in open() and only overwritten afterwards, so the callback never
    // touches the allocator.
    std::vector<const float*> in_bufs;
    std::vector<float*> out_bufs;
    // jack_port_get_buffer can return NULL (a period larger than the port's
    // buffer, a port being invalidated). An input then reads silence and an
    // output writes into scratch, so every pointer in the block is usable
    // and every track keeps the same length.
    std::vector<float> silence;
    std::vector<float> scratch;
    backend::AudioBlock block{};
    backend::ProcessCallback* callback = nullptr;
    uint32_t rate = 0;
    // Bumped by JACK's xrun callback: on the audio thread under PipeWire's
    // JACK, on a notification thread under jackd2. Read by the control
    // thread.
    std::atomic<uint32_t> xruns{0};
    // jack_get_time()'s base is the server's, not necessarily the engine's
    // CLOCK_MONOTONIC: jackd2's lagged steady_clock by a constant 0.8 s in
    // the container (backend conformance, callback_ns_is_the_monotonic_clock),
    // which would phase every timer-paced plugin host wrongly. The offset
    // from it to steady_clock is sampled off the audio thread, every 100 ms
    // while active, so the callback converts the cycle time with one add
    // and reads no clock (contracts/backend, callback_ns).
    std::atomic<int64_t> steady_minus_jack_ns{0};
    std::thread time_sampler;
    std::mutex sampler_mutex;
    std::condition_variable sampler_wake;
    bool sampler_stop = false;

    void sample_time_offset() {
        // The tightest of three brackets around jack_get_time().
        int64_t best_width = INT64_MAX, best = 0;
        for (int i = 0; i < 3; ++i) {
            const auto t0 = static_cast<int64_t>(now_ns());
            const auto j = static_cast<int64_t>(jack_get_time()) * 1000;
            const auto t1 = static_cast<int64_t>(now_ns());
            if (t1 - t0 < best_width) {
                best_width = t1 - t0;
                best = t0 + (t1 - t0) / 2 - j;
            }
        }
        // A bracket the scheduler stretched (a loaded machine) would move
        // the offset by half its width: keep the last good one instead.
        const bool have = sampled.load(std::memory_order_relaxed);
        if (have && best_width > 100'000) return;
        steady_minus_jack_ns.store(best, std::memory_order_relaxed);
        sampled.store(true, std::memory_order_relaxed);
    }
    std::atomic<bool> sampled{false};
    void start_time_sampler() {
        sample_time_offset();
        {
            std::lock_guard<std::mutex> lock(sampler_mutex);
            sampler_stop = false;
        }
        time_sampler = std::thread([this] {
            std::unique_lock<std::mutex> lock(sampler_mutex);
            while (!sampler_wake.wait_for(lock, std::chrono::milliseconds(100), [this] { return sampler_stop; })) {
                lock.unlock();
                sample_time_offset();
                lock.lock();
            }
        });
    }
    void stop_time_sampler() noexcept {
        {
            std::lock_guard<std::mutex> lock(sampler_mutex);
            sampler_stop = true;
        }
        sampler_wake.notify_all();
        if (time_sampler.joinable()) time_sampler.join();
    }
    drivers::PeriodGuard guard;
    drivers::FrameClock clock;  // widened on the audio thread only

    // The physical ports feeding our inputs, so their going away can be
    // recognised. Touched by JACK's notification callbacks and by
    // fatal_reason(); never by the audio thread.
    mutable std::mutex sources_mutex;
    drivers::SourceWatch sources;
    std::string gone_port;

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

    bool is_input(jack_port_t* p) const {
        return p && std::find(in_ports.begin(), in_ports.end(), p) != in_ports.end();
    }

    // The trampoline: fetch the port buffers, hand them over, nothing more
    // (ADR-0007 §2).
    static int process_cb(jack_nframes_t nframes, void* arg) noexcept SONORA_RT {
        auto* self = static_cast<Impl*>(arg);
        for (std::size_t o = 0; o < self->out_ports.size(); ++o) {
            self->out_bufs[o] = static_cast<float*>(
                jack_port_get_buffer(self->out_ports[o], nframes));
        }
        // The rate is read every period: the server's notification of a
        // change can arrive after periods at the new rate have run.
        if (!self->guard.admit(nframes, jack_get_sample_rate(self->client))) {
            // The engine no longer fills the outputs, and a JACK buffer holds
            // whatever the last cycle left in it.
            for (float* out : self->out_bufs) {
                if (out) std::memset(out, 0, nframes * sizeof(float));
            }
            return 0;
        }
        // admit() bounds nframes by the scratch size, so either buffer will do.
        for (float*& out : self->out_bufs) {
            if (!out) out = self->scratch.data();
        }
        for (std::size_t i = 0; i < self->in_ports.size(); ++i) {
            const auto* in = static_cast<const float*>(
                jack_port_get_buffer(self->in_ports[i], nframes));
            self->in_bufs[i] = in ? in : self->silence.data();
        }
        self->block.nframes = nframes;
        self->block.frame_time = self->clock.widen(jack_last_frame_time(self->client), nframes,
                                                   self->xruns.load(std::memory_order_relaxed));
        // The server's own stamp of this cycle, from its shared memory, in
        // jack_get_time()'s base, moved onto steady_clock by the offset the
        // sampler thread keeps: no clock read here (contracts/backend,
        // callback_ns). The conformance suite checks the result against
        // steady_clock read in the period.
        jack_nframes_t cycle_frames = 0;
        jack_time_t cycle_usecs = 0, next_usecs = 0;
        float period_usecs = 0.0f;
        int64_t stamp = 0;
        if (jack_get_cycle_times(self->client, &cycle_frames, &cycle_usecs, &next_usecs, &period_usecs) == 0) {
            stamp = static_cast<int64_t>(cycle_usecs) * 1000 +
                    self->steady_minus_jack_ns.load(std::memory_order_relaxed);
        }
        self->block.callback_ns = stamp > 0 ? static_cast<uint64_t>(stamp) : 0;
        self->callback->process(self->block);
        return 0;
    }

    static int xrun_cb(void* arg) noexcept SONORA_RT {
        static_cast<Impl*>(arg)->xruns.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    static int sample_rate_cb(jack_nframes_t rate, void* arg) noexcept {
        static_cast<Impl*>(arg)->guard.rate_reported(rate);
        return 0;
    }

    // Follows every physical port that feeds one of our inputs, however it
    // was patched: by connect_inputs_in_order() or by hand. A non-physical
    // source (pw-play, a soft synth) going away is not a device going away,
    // so it is never followed.
    static void connect_cb(jack_port_id_t a, jack_port_id_t b, int connect,
                           void* arg) noexcept {
        auto* self = static_cast<Impl*>(arg);
        jack_port_t* pa = jack_port_by_id(self->client, a);
        jack_port_t* pb = jack_port_by_id(self->client, b);
        jack_port_id_t source_id;
        jack_port_t* source;
        if (self->is_input(pb)) {
            source_id = a;
            source = pa;
        } else if (self->is_input(pa)) {
            source_id = b;
            source = pb;
        } else {
            return;
        }
        if (!source || !(jack_port_flags(source) & JackPortIsPhysical)) return;
        const char* name = jack_port_name(source);
        if (!name) return;
        try {
            std::lock_guard<std::mutex> lock(self->sources_mutex);
            if (connect) {
                self->sources.linked(source_id, name, now_ns());
            } else {
                self->sources.unlinked(source_id, name, now_ns());
            }
        } catch (...) {
            // Out of memory in a notification: this port's loss will still
            // show as a stall if the device takes the clock with it.
        }
    }

    static void registration_cb(jack_port_id_t id, int registered,
                                void* arg) noexcept {
        auto* self = static_cast<Impl*>(arg);
        if (registered) return;
        // By name as well as id: jackd2 reuses ids. pw-jack still knows
        // the port while it reports it gone.
        const jack_port_t* port = jack_port_by_id(self->client, id);
        const char* name = port ? jack_port_name(port) : nullptr;
        try {
            std::lock_guard<std::mutex> lock(self->sources_mutex);
            std::string gone;
            if (!self->sources.removed(id, name, now_ns(), gone)) return;
            if (self->guard.fatal() == Fatal::None) self->gone_port = std::move(gone);
            self->guard.raise(Fatal::DeviceGone);
        } catch (...) {
            self->guard.raise(Fatal::DeviceGone);
        }
    }

    static void shutdown_cb(void* arg) noexcept {
        // Called from a JACK thread. Only flag it: the client handle is
        // already dead, so calling back into the API here is not allowed.
        static_cast<Impl*>(arg)->guard.raise(Fatal::ServerGone);
    }
};

void JackBackend::prepare_process() {
    const std::string props = drivers::with_fill_aliases(std::getenv("PIPEWIRE_PROPS"));
    ::setenv("PIPEWIRE_PROPS", props.c_str(), 1);
}

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

    // Decided now when it can be, so a second card appearing later cannot
    // change which device the inputs come from. With no USB device yet it
    // is decided at the first connection instead.
    std::vector<PortInfo> chosen;
    if (!backend::select_device(d.physical(JackPortIsOutput),
                                config.capture_device, chosen, error)) {
        return fail();
    }
    d.capture_device = config.capture_device.empty() && !chosen.empty()
                           ? chosen.front().device
                           : config.capture_device;

    if (!d.register_ports(config.inputs, "in", JackPortIsInput, d.in_ports,
                          error) ||
        !d.register_ports(config.outputs, "out", JackPortIsOutput, d.out_ports,
                          error)) {
        return fail();
    }
    d.in_bufs.assign(d.in_ports.size(), nullptr);
    d.out_bufs.assign(d.out_ports.size(), nullptr);
    const uint32_t scratch_frames =
        std::max(kMinScratchFrames, jack_get_buffer_size(d.client));
    d.silence.assign(scratch_frames, 0.0f);
    d.scratch.assign(scratch_frames, 0.0f);
    d.guard.set_max_frames(scratch_frames);
    d.block = backend::AudioBlock{
        d.in_bufs.data(),  static_cast<uint32_t>(d.in_bufs.size()),
        d.out_bufs.data(), static_cast<uint32_t>(d.out_bufs.size()),
        0, 0, 0,
    };
    d.callback = &callback;

    // Before the callback is set: JACK calls it at once with the current
    // rate, which must not read as a change.
    d.guard.expect_rate(d.rate);
    jack_set_process_callback(d.client, Impl::process_cb, &d);
    jack_set_xrun_callback(d.client, Impl::xrun_cb, &d);
    jack_set_sample_rate_callback(d.client, Impl::sample_rate_cb, &d);
    jack_set_port_connect_callback(d.client, Impl::connect_cb, &d);
    jack_set_port_registration_callback(d.client, Impl::registration_cb, &d);
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
    d.clock.resume();
    // Before the first callback, so it has an offset.
    d.start_time_sampler();
    if (jack_activate(d.client) != 0) {
        d.stop_time_sampler();
        error = "jack_activate failed";
        return false;
    }
    d.active = true;
    d.guard.watch_from(now_ns());
    return true;
}

int JackBackend::connect_inputs_in_order() {
    Impl& d = *impl_;
    if (!d.client) return 0;

    const auto complain = [&d](const std::string& what) {
        if (what == d.last_complaint) return;
        d.last_complaint = what;
        std::fprintf(stderr, "sonora: %s\n", what.c_str());
    };

    std::vector<PortInfo> hw;
    std::string why;
    if (!backend::select_device(d.physical(JackPortIsOutput), d.capture_device,
                                hw, why)) {
        complain("not connecting inputs: " + why);
        return 0;
    }
    if (hw.empty()) return 0;

    int connected = 0;
    const std::size_t n = std::min(hw.size(), d.in_ports.size());
    for (std::size_t i = 0; i < n; ++i) {
        const int rc = jack_connect(d.client, hw[i].name.c_str(),
                                    jack_port_name(d.in_ports[i]));
        // An existing link is the outcome asked for, not a failure.
        if (rc == 0 || rc == EEXIST) ++connected;
    }
    if (connected > 0 && d.capture_device.empty()) d.capture_device = hw.front().device;

    // A count mismatch either way is worth saying out loud. More tracks than
    // channels means the extra tracks record silence; more channels than
    // tracks means inputs the operator plugged in are not being captured.
    if (hw.size() < d.in_ports.size()) {
        complain(std::to_string(hw.size()) + " capture channels for " +
                 std::to_string(d.in_ports.size()) + " tracks; tracks " +
                 std::to_string(hw.size() + 1) + " and above will be silent.");
    } else if (hw.size() > d.in_ports.size()) {
        complain(std::to_string(hw.size()) + " capture channels but only " +
                 std::to_string(d.in_ports.size()) + " tracks; channels " +
                 std::to_string(d.in_ports.size() + 1) +
                 " and above are not being recorded.");
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

bool JackBackend::input_connected(uint32_t index) const {
    return index < impl_->in_ports.size() &&
           jack_port_connected(impl_->in_ports[index]) > 0;
}

bool JackBackend::choose_capture_device(const std::string& device) {
    Impl& d = *impl_;
    if (!d.client || !d.capture_device.empty() || device.empty()) return false;
    d.capture_device = device;
    return true;
}

void JackBackend::deactivate() noexcept {
    Impl& d = *impl_;
    if (!d.client || !d.active) return;
    d.guard.unwatch();
    // Before the client is closed and before the caller frees the session,
    // so the callback cannot run against freed state.
    jack_deactivate(d.client);
    d.stop_time_sampler();
    d.clock.finish(d.xruns.load(std::memory_order_relaxed));
    d.active = false;
}

uint32_t JackBackend::sample_rate() const { return impl_->rate; }

uint32_t JackBackend::buffer_frames() const { return impl_->guard.frames(); }

uint32_t JackBackend::xruns() const {
    return impl_->xruns.load(std::memory_order_relaxed);
}

uint32_t JackBackend::clock_discontinuities() const {
    return impl_->clock.discontinuities();
}

bool JackBackend::fatal() const { return impl_->guard.check_stall(now_ns()); }

std::string JackBackend::fatal_reason() const {
    const Impl& d = *impl_;
    drivers::FatalContext context;
    context.rate = d.rate;
    context.admitted_frames = d.guard.frames();
    context.max_frames = d.scratch.size();
    const drivers::PeriodGuard::State state = d.guard.state();
    if (state.why == Fatal::DeviceGone) {
        std::lock_guard<std::mutex> lock(d.sources_mutex);
        context.gone_port = d.gone_port;
    }
    return drivers::describe_fatal(state, context);
}

}  // namespace sonora
