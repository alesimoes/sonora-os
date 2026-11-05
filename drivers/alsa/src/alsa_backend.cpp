// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/alsa_backend.h"

#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <system_error>
#include <thread>
#include <utility>

#include "sonora/alsa_convert.h"
#include "sonora/alsa_proc.h"
#include "sonora/clock_offset.h"
#include "sonora/fatal_reason.h"
#include "sonora/period_guard.h"

namespace sonora {
namespace {

using alsa::Status;
using backend::PortInfo;
using drivers::Fatal;

uint64_t now_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

// How long begin() waits before the thread looks at its stop flag. This is
// the bound on deactivate(), and a tenth of the stall limit.
constexpr int kWaitMs = 100;

std::vector<PortInfo> ports_of(const std::vector<alsa::CardInfo>& cards, bool capture) {
    std::vector<PortInfo> out;
    for (const alsa::CardInfo& c : cards) {
        const uint32_t n = capture ? c.capture_channels : c.playback_channels;
        for (uint32_t i = 0; i < n; ++i) {
            out.push_back({c.id, c.id + (capture ? ":capture_" : ":playback_") + std::to_string(i + 1), i,
                           c.usb});
        }
    }
    return out;
}

}  // namespace

struct AlsaBackend::Impl {
    Options options;
    std::unique_ptr<alsa::System> system;

    backend::BackendConfig config;
    backend::ProcessCallback* callback = nullptr;
    bool opened = false;
    bool active = false;
    // Empty until a card is chosen; never changed once set (the inputs must
    // not move to another card under the tracks).
    std::string capture_device;
    std::string last_complaint;

    std::unique_ptr<alsa::Duplex> duplex;
    alsa::DuplexInfo info;
    bool streaming = false;
    std::thread thread;
    std::atomic<bool> stop{false};

    // Sized in start_stream() before the thread exists; the thread only
    // overwrites them.
    std::vector<float> in_planar, out_planar, silence;
    std::vector<const float*> in_bufs;
    std::vector<float*> out_bufs;
    std::vector<float*> in_write;
    uint32_t used_in = 0;   // card channels that feed an input
    uint32_t used_out = 0;  // outputs that reach a card channel
    backend::AudioBlock block{};

    drivers::PeriodGuard guard;
    drivers::MonotonicStamp stamper;
    uint64_t frame_time = 0;  // the thread's
    std::atomic<uint32_t> xruns{0};
    std::atomic<uint32_t> connected{0};
    std::atomic<int> rt_granted{0};
    std::atomic<uint32_t> ring{0};
    std::string gone_port;  // written before guard.raise(), read after it

    void set_up_thread() noexcept {
        pthread_setname_np(pthread_self(), "sonora-alsa");
        if (options.rt_priority <= 0) return;
        sched_param sp{};
        sp.sched_priority = options.rt_priority;
        const int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
        if (rc == 0) {
            rt_granted.store(options.rt_priority, std::memory_order_relaxed);
        } else {
            // Not fatal: the stream runs, with whatever latency the
            // scheduler gives it. Said once, with the way to fix it.
            std::fprintf(stderr,
                         "sonora: alsa: no SCHED_FIFO %d for the audio thread (%s); is this "
                         "user in the audio group with rtprio in limits-audio.conf?\n",
                         options.rt_priority, std::strerror(rc));
        }
    }

    void device_gone() noexcept {
        connected.store(0, std::memory_order_relaxed);
        try {
            gone_port = capture_device + ":capture_1";
        } catch (...) {
        }
        guard.raise(Fatal::DeviceGone);
    }

    // One period through the engine; returns the status of the commit.
    Status cycle(const alsa::Cycle& cy) noexcept {
        const uint32_t n = cy.frames;
        if (!guard.admit(n, config.required_rate)) {
            std::memset(cy.playback, 0,
                        static_cast<std::size_t>(n) * info.out_channels *
                            alsa::bytes_per_sample(info.out_format));
            return duplex->commit();
        }
        alsa::deinterleave(info.in_format, cy.capture, info.in_channels, in_write.data(),
                           used_in, n);
        block.nframes = n;
        block.frame_time = frame_time;
        block.callback_ns = stamper.stamp(static_cast<int64_t>(cy.stamp_ns), 0);
        callback->process(block);
        alsa::interleave(info.out_format, out_bufs.data(), used_out, cy.playback,
                         info.out_channels, n);
        frame_time += n;
        return duplex->commit();
    }

    void run() noexcept {
        set_up_thread();
        alsa::Cycle cy;
        while (!stop.load(std::memory_order_relaxed)) {
            Status st = duplex->begin(kWaitMs, cy);
            if (st == Status::Timeout) continue;
            if (st == Status::Ready) {
                st = cycle(cy);
                if (guard.fatal() != Fatal::None) break;
            }
            if (st == Status::Xrun) {
                // Counted, never waited on: recover() restarts the stream
                // with silence queued, and the capture frames between the
                // last read and the restart are gone. Their number is not
                // known; one period is the least that is certain.
                xruns.fetch_add(1, std::memory_order_relaxed);
                frame_time += info.period_frames;
                if (!duplex->recover()) st = Status::Gone;
            }
            if (st == Status::Gone) {
                device_gone();
                break;
            }
        }
    }

    enum class Start { Started, Absent, Failed };

    Start start_stream(std::string& error) {
        if (streaming) return Start::Started;
        if (guard.fatal() != Fatal::None) {
            error = "the audio device failed earlier in this session";
            return Start::Failed;
        }
        if (!system) {
            error = "this build has no ALSA support (libasound was not found at build time)";
            return Start::Failed;
        }
        alsa::CardInfo card;
        bool found = false;
        for (const alsa::CardInfo& c : system->cards()) {
            if (c.id == capture_device) {
                card = c;
                found = true;
            }
        }
        if (!found) return Start::Absent;
        if (card.capture_channels == 0 || card.playback_channels == 0) {
            error = "card " + card.id + " does not have both capture and playback";
            return Start::Failed;
        }
        alsa::DuplexRequest req;
        req.capture_pcm = card.capture_pcm;
        req.playback_pcm = card.playback_pcm;
        req.rate = config.required_rate;
        req.in_channels = card.capture_channels;
        req.out_channels = card.playback_channels;
        req.period_frames = options.period_frames;
        req.periods = options.periods;
        auto dev = system->make_duplex();
        alsa::DuplexInfo got;
        if (!dev->open(req, got, error)) {
            error = "cannot open card " + card.id + ": " + error;
            return Start::Failed;
        }
        if (got.rate != config.required_rate) {
            error = "card " + card.id + " runs at " + std::to_string(got.rate) +
                    " Hz, session asked for " + std::to_string(config.required_rate) + " Hz";
            return Start::Failed;
        }
        info = got;
        duplex = std::move(dev);

        const uint32_t period = info.period_frames;
        used_in = std::min(info.in_channels, config.inputs);
        used_out = std::min(info.out_channels, config.outputs);
        in_planar.assign(static_cast<std::size_t>(used_in) * period, 0.0f);
        // One plane for every output, including those past the card's
        // channels: the callback writes them all, the card hears the first
        // used_out.
        out_planar.assign(static_cast<std::size_t>(config.outputs) * period, 0.0f);
        silence.assign(period, 0.0f);
        in_bufs.assign(config.inputs, nullptr);
        in_write.assign(used_in, nullptr);
        for (uint32_t i = 0; i < config.inputs; ++i) {
            if (i < used_in) {
                in_write[i] = in_planar.data() + static_cast<std::size_t>(i) * period;
                in_bufs[i] = in_write[i];
            } else {
                in_bufs[i] = silence.data();
            }
        }
        out_bufs.assign(config.outputs, nullptr);
        for (uint32_t i = 0; i < config.outputs; ++i) {
            out_bufs[i] = out_planar.data() + static_cast<std::size_t>(i) * period;
        }
        block = backend::AudioBlock{in_bufs.data(),  config.inputs, out_bufs.data(),
                                    config.outputs, 0,             0, 0};

        guard.expect_rate(config.required_rate);
        guard.set_max_frames(period);
        stamper.reset();
        frame_time = 0;
        if (!duplex->start(error)) {
            error = "cannot start card " + card.id + ": " + error;
            duplex.reset();
            return Start::Failed;
        }
        stop.store(false, std::memory_order_relaxed);
        ring.store(info.buffer_frames, std::memory_order_relaxed);
        connected.store(used_in, std::memory_order_relaxed);
        try {
            thread = std::thread([this] { run(); });
        } catch (const std::system_error& e) {
            error = std::string("cannot start the audio thread: ") + e.what();
            connected.store(0, std::memory_order_relaxed);
            duplex.reset();
            return Start::Failed;
        }
        streaming = true;
        guard.watch_from(now_ns());
        return Start::Started;
    }

    void stop_stream() noexcept {
        guard.unwatch();
        stop.store(true, std::memory_order_relaxed);
        if (thread.joinable()) thread.join();
        duplex.reset();
        streaming = false;
        connected.store(0, std::memory_order_relaxed);
        ring.store(0, std::memory_order_relaxed);
        rt_granted.store(0, std::memory_order_relaxed);
    }

    void complain(const std::string& what) {
        if (what == last_complaint) return;
        last_complaint = what;
        std::fprintf(stderr, "sonora: %s\n", what.c_str());
    }
};

AlsaBackend::AlsaBackend() : AlsaBackend(Options{}) {}

AlsaBackend::AlsaBackend(const Options& options) : AlsaBackend(options, alsa::make_system()) {}

AlsaBackend::AlsaBackend(const Options& options, std::unique_ptr<alsa::System> system)
    : impl_(std::make_unique<Impl>()) {
    impl_->options = options;
    impl_->system = std::move(system);
}

AlsaBackend::~AlsaBackend() { deactivate(); }

std::vector<PortInfo> AlsaBackend::physical_capture_ports() {
    return impl_->system ? ports_of(impl_->system->cards(), true) : std::vector<PortInfo>{};
}

std::vector<PortInfo> AlsaBackend::physical_playback_ports() {
    return impl_->system ? ports_of(impl_->system->cards(), false) : std::vector<PortInfo>{};
}

bool AlsaBackend::open(const backend::BackendConfig& config, backend::ProcessCallback& callback,
                       std::string& error) {
    Impl& d = *impl_;
    if (d.opened) {
        error = "the audio backend is already open";
        return false;
    }
    if (!d.system) {
        error = "this build has no ALSA support (libasound was not found at build time)";
        return false;
    }
    const std::vector<alsa::CardInfo> cards = d.system->cards();
    std::string chosen;
    if (!config.capture_device.empty()) {
        // A name that matches no card is kept as given: the card may be
        // plugged in later, and nothing else may take its place.
        chosen = alsa::resolve_card(cards, config.capture_device);
        if (chosen.empty()) chosen = config.capture_device;
    } else {
        std::vector<PortInfo> picked;
        if (!backend::select_device(ports_of(cards, true), "", picked, error)) return false;
        if (!picked.empty()) chosen = picked.front().device;
    }
    for (const alsa::CardInfo& c : cards) {
        if (c.id != chosen || c.rates.empty()) continue;
        if (std::find(c.rates.begin(), c.rates.end(), config.required_rate) == c.rates.end()) {
            error = "card " + c.id + " does not offer " + std::to_string(config.required_rate) +
                    " Hz, session asked for it; it offers:";
            for (uint32_t r : c.rates) error += " " + std::to_string(r);
            return false;
        }
    }
    d.config = config;
    d.callback = &callback;
    d.capture_device = chosen;
    d.opened = true;
    return true;
}

bool AlsaBackend::activate(std::string& error) {
    Impl& d = *impl_;
    if (!d.opened) {
        error = "the audio backend is not open";
        return false;
    }
    if (d.active) return true;
    d.active = true;
    if (d.capture_device.empty()) return true;
    if (d.start_stream(error) == Impl::Start::Failed) {
        d.active = false;
        return false;
    }
    return true;
}

int AlsaBackend::connect_inputs_in_order() {
    Impl& d = *impl_;
    if (!d.opened || !d.system) return 0;

    std::vector<PortInfo> hw;
    if (d.capture_device.empty()) {
        std::string why;
        if (!backend::select_device(physical_capture_ports(), "", hw, why)) {
            d.complain("not connecting inputs: " + why);
            return 0;
        }
        if (hw.empty()) return 0;
        d.capture_device = hw.front().device;
    }
    if (!d.streaming) {
        if (!d.active) return 0;
        std::string error;
        if (d.start_stream(error) == Impl::Start::Failed) {
            d.complain(error);
            return 0;
        }
        if (!d.streaming) return 0;
    }
    const uint32_t channels = d.info.in_channels;
    if (channels < d.config.inputs) {
        d.complain(std::to_string(channels) + " capture channels for " +
                   std::to_string(d.config.inputs) + " tracks; tracks " +
                   std::to_string(channels + 1) + " and above will be silent.");
    } else if (channels > d.config.inputs) {
        d.complain(std::to_string(channels) + " capture channels but only " +
                   std::to_string(d.config.inputs) + " tracks; channels " +
                   std::to_string(d.config.inputs + 1) + " and above are not being recorded.");
    }
    return static_cast<int>(d.connected.load(std::memory_order_relaxed));
}

int AlsaBackend::connected_input_count() const {
    return static_cast<int>(impl_->connected.load(std::memory_order_relaxed));
}

bool AlsaBackend::input_connected(uint32_t index) const {
    return index < impl_->connected.load(std::memory_order_relaxed);
}

bool AlsaBackend::choose_capture_device(const std::string& device) {
    Impl& d = *impl_;
    if (!d.opened || !d.capture_device.empty() || device.empty()) return false;
    d.capture_device = device;
    return true;
}

void AlsaBackend::deactivate() noexcept {
    Impl& d = *impl_;
    if (d.streaming) d.stop_stream();
    d.active = false;
}

uint32_t AlsaBackend::sample_rate() const { return impl_->config.required_rate; }

uint32_t AlsaBackend::buffer_frames() const { return impl_->guard.frames(); }

uint32_t AlsaBackend::xruns() const { return impl_->xruns.load(std::memory_order_relaxed); }

// A card's clock cannot step: there is no server between it and us.
uint32_t AlsaBackend::clock_discontinuities() const { return 0; }

bool AlsaBackend::fatal() const { return impl_->guard.check_stall(now_ns()); }

std::string AlsaBackend::fatal_reason() const {
    const Impl& d = *impl_;
    drivers::FatalContext context;
    context.rate = d.config.required_rate;
    context.admitted_frames = d.guard.frames();
    context.max_frames = d.options.period_frames;
    const drivers::PeriodGuard::State state = d.guard.state();
    if (state.why == Fatal::DeviceGone) context.gone_port = d.gone_port;
    return drivers::describe_fatal(state, context);
}

std::string AlsaBackend::capture_device() const { return impl_->capture_device; }

// The capture period has to be complete before process() sees it, and what
// process() writes waits behind the ring's other queued periods: the figure
// is the least this backend adds, without the converters or the USB
// transport. It is the fallback for a measured L (ADR-0019), nothing more.
uint32_t AlsaBackend::reported_latency() const {
    const Impl& d = *impl_;
    if (!d.streaming) return 0;
    return d.info.period_frames + d.info.buffer_frames;
}

int AlsaBackend::rt_priority_granted() const {
    return impl_->rt_granted.load(std::memory_order_relaxed);
}

uint32_t AlsaBackend::ring_frames() const { return impl_->ring.load(std::memory_order_relaxed); }

}  // namespace sonora
