// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/fake_backend.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

#include "sonora/fatal_reason.h"
#include "sonora/frame_clock.h"
#include "sonora/period_guard.h"
#include "sonora/port_devices.h"
#include "sonora/ringbuffer.h"
#include "sonora/source_watch.h"

namespace sonora {
namespace {

using backend::PortInfo;
using drivers::Fatal;

// As in drivers/jack: buffers this large hold any period PipeWire runs, and
// a refused period is never written into them.
constexpr uint32_t kMinBufferFrames = 8192;
constexpr int32_t kNoSource = -1;
constexpr double kTwoPi = 6.283185307179586476925286766559;

struct Port {
    uint32_t device;   // index into Config::devices
    uint32_t channel;  // hardware channel within the device
    bool capture;
    std::string name;
    uint32_t id;
};

std::string port_name(const FakeBackend::Device& d, FakeBackend::Naming naming,
                      bool capture, uint32_t channel) {
    const std::string kind = capture ? "capture_" : "playback_";
    return d.name + ":" + kind +
           (naming == FakeBackend::Naming::Aux ? "AUX" + std::to_string(channel)
                                               : std::to_string(channel + 1));
}

}  // namespace

struct FakeBackend::Impl {
    const Config cfg;
    const std::chrono::steady_clock::time_point origin = std::chrono::steady_clock::now();
    std::atomic<uint64_t> virtual_ns{0};

    // Every port any device will ever have, fixed at construction, so the
    // audio thread reads it without a lock.
    std::vector<Port> ports;

    // --- control side, under `control` ---
    mutable std::mutex control;
    std::vector<bool> unplugged;  // per device
    std::string capture_device;
    std::string last_warning;
    drivers::SourceWatch sources;
    std::string gone_port;
    bool open = false;

    // --- set by open(), then read by the audio thread ---
    backend::ProcessCallback* callback = nullptr;
    uint32_t rate = 0;
    uint32_t capacity = 0;
    std::vector<std::vector<float>> in_store;
    std::vector<std::vector<float>> out_store;
    std::vector<const float*> in_bufs;
    std::vector<float*> out_bufs;
    std::vector<float> silence;
    std::vector<std::unique_ptr<RingBuffer<float>>> out_capture;
    // Which port feeds each input: an index into `ports`, or kNoSource.
    // Written under `control`, read by the audio thread.
    std::unique_ptr<std::atomic<int32_t>[]> input_source;
    uint32_t inputs = 0;
    backend::AudioBlock block{};

    // --- the server ---
    std::atomic<bool> server_gone{false};
    std::atomic<bool> stalled{false};
    std::atomic<uint32_t> server_rate;
    std::atomic<uint32_t> server_quantum;
    std::atomic<uint32_t> pending_xrun_frames{0};
    std::atomic<uint32_t> pending_step_back{0};
    std::atomic<uint32_t> pending_skip_frames{0};
    std::atomic<uint32_t> pending_late_xrun_frames{0};
    uint32_t counter;  // the server's 32-bit frame counter; audio thread only

    drivers::PeriodGuard guard;
    drivers::FrameClock clock;  // audio thread only
    std::atomic<uint32_t> xruns{0};
    std::atomic<uint64_t> process_calls{0};
    std::atomic<uint64_t> unwritten{0};
    std::atomic<uint64_t> dropped{0};

    // --- the drive ---
    std::atomic<bool> active{false};
    std::atomic<bool> in_period{false};
    std::atomic<bool> thread_run{false};
    std::thread thread;

    explicit Impl(Config c)
        : cfg(std::move(c)),
          unplugged(cfg.devices.size(), false),
          server_rate(cfg.rate),
          server_quantum(cfg.quantum),
          counter(cfg.first_frame) {
        for (uint32_t d = 0; d < cfg.devices.size(); ++d) {
            const Device& dev = cfg.devices[d];
            for (uint32_t c = 0; c < dev.capture; ++c) {
                ports.push_back({d, c, true, port_name(dev, cfg.naming, true, c), 0});
            }
            for (uint32_t c = 0; c < dev.playback; ++c) {
                ports.push_back({d, c, false, port_name(dev, cfg.naming, false, c), 0});
            }
        }
        assign_ids();
    }

    // Natural ids ascend with the channels. Out of order, they are a fixed
    // scramble of the same set, so a test run twice sees the same ids.
    void assign_ids() {
        std::vector<uint32_t> order(ports.size());
        for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
        if (cfg.ids_out_of_order) {
            std::sort(order.begin(), order.end(), [](uint32_t a, uint32_t b) {
                return (a * 2654435761u) < (b * 2654435761u);
            });
        }
        // Not starting at 0: an id is not an index, and 0 would hide it.
        for (uint32_t i = 0; i < order.size(); ++i) ports[order[i]].id = 40 + i;
    }

    uint64_t now_ns() const {
        if (cfg.mode == Mode::Synchronous) return virtual_ns.load(std::memory_order_relaxed);
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - origin)
                                         .count());
    }

    // Under `control`.
    bool present(uint32_t device) const {
        return !server_gone.load(std::memory_order_acquire) && !unplugged[device] &&
               now_ns() >= cfg.devices[device].appears_after_ns;
    }

    // Under `control`. As the server lists them, then as every backend
    // describes them: sorted into channel order, numbered per device.
    std::vector<PortInfo> physical(bool capture) const {
        std::vector<std::string> names;
        for (const Port& p : ports) {
            if (p.capture == capture && present(p.device)) names.push_back(p.name);
        }
        switch (cfg.list_order) {
            case ListOrder::Natural:
                break;
            case ListOrder::Lexicographic:
                std::sort(names.begin(), names.end());
                break;
            case ListOrder::Reversed:
                std::reverse(names.begin(), names.end());
                break;
        }
        return drivers::describe_ports(std::move(names), [this](const std::string& n) {
            const int32_t i = find(n);
            return i >= 0 && cfg.devices[ports[i].device].usb;
        });
    }

    int32_t find(const std::string& name) const {
        for (std::size_t i = 0; i < ports.size(); ++i) {
            if (ports[i].name == name) return static_cast<int32_t>(i);
        }
        return kNoSource;
    }

    // Under `control`. One source per input: a link replaces the last one.
    void link(uint32_t index, int32_t port) {
        const int32_t old = input_source[index].load(std::memory_order_relaxed);
        if (old == port) return;
        if (old != kNoSource) unlink(index);
        sources.linked(ports[port].id, ports[port].name, now_ns());
        input_source[index].store(port, std::memory_order_release);
    }

    void unlink(uint32_t index) {
        const int32_t old = input_source[index].load(std::memory_order_relaxed);
        if (old == kNoSource) return;
        input_source[index].store(kNoSource, std::memory_order_release);
        sources.unlinked(ports[old].id, ports[old].name, now_ns());
    }

    float signal(uint32_t channel, uint64_t frame) const noexcept SONORA_RT {
        switch (cfg.signal) {
            case Signal::Tones:
                return FakeBackend::tone(channel, frame, rate);
            case Signal::Impulses:
                return cfg.impulse_interval != 0 && frame % cfg.impulse_interval == 0 ? 1.0f
                                                                                      : 0.0f;
            case Signal::Silence:
                break;
        }
        return 0.0f;
    }

    // One server period: the part drivers/jack's process_cb does, plus what
    // a server would have put in the capture buffers. Returns whether it
    // reached process().
    bool period() noexcept SONORA_RT {
        if (server_gone.load(std::memory_order_acquire) ||
            stalled.load(std::memory_order_acquire)) {
            return false;
        }
        const uint32_t nframes = server_quantum.load(std::memory_order_acquire);
        if (!guard.admit(nframes, server_rate.load(std::memory_order_acquire))) return false;

        if (const uint32_t lost = pending_xrun_frames.exchange(0, std::memory_order_acq_rel)) {
            counter += lost;
            xruns.fetch_add(1, std::memory_order_relaxed);
        }
        counter -= pending_step_back.exchange(0, std::memory_order_acq_rel);
        counter += pending_skip_frames.exchange(0, std::memory_order_acq_rel);
        const uint32_t late = pending_late_xrun_frames.exchange(0, std::memory_order_acq_rel);
        counter += late;
        const uint64_t frame_time =
            clock.widen(counter, nframes, xruns.load(std::memory_order_relaxed));
        // Reported only once this period's callback has its frame time.
        if (late != 0) xruns.fetch_add(1, std::memory_order_relaxed);

        for (uint32_t i = 0; i < inputs; ++i) {
            const int32_t src = input_source[i].load(std::memory_order_acquire);
            if (src == kNoSource) {
                in_bufs[i] = cfg.silence_for_unconnected ? silence.data() : nullptr;
                continue;
            }
            float* buf = in_store[i].data();
            const uint32_t channel = ports[src].channel;
            for (uint32_t f = 0; f < nframes; ++f) buf[f] = signal(channel, frame_time + f);
            in_bufs[i] = buf;
        }
        // What a callback that skips an output would leave there.
        for (float* out : out_bufs) {
            std::fill(out, out + nframes, std::numeric_limits<float>::quiet_NaN());
        }

        block.nframes = nframes;
        block.frame_time = frame_time;
        callback->process(block);
        process_calls.fetch_add(1, std::memory_order_relaxed);

        bool all_written = true;
        for (std::size_t o = 0; o < out_bufs.size(); ++o) {
            const float* out = out_bufs[o];
            for (uint32_t f = 0; f < nframes; ++f) {
                if (std::isnan(out[f])) {
                    all_written = false;
                    break;
                }
            }
            if (o < out_capture.size() && out_capture[o]->write(out, nframes) < nframes) {
                dropped.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (!all_written) unwritten.fetch_add(1, std::memory_order_relaxed);

        counter += nframes;
        return true;
    }

    uint64_t period_ns() const {
        const uint64_t r = std::max<uint32_t>(1, server_rate.load(std::memory_order_acquire));
        return uint64_t{server_quantum.load(std::memory_order_acquire)} * 1000000000ull / r;
    }

    // One step of either drive. The flag goes up before `active` is read,
    // and deactivate() clears `active` before it reads the flag, so either
    // this sees it inactive or deactivate() waits for it (both seq_cst).
    bool step() noexcept {
        in_period.store(true);
        bool ran = false;
        if (active.load()) ran = period();
        in_period.store(false);
        return ran;
    }

    void run_thread() {
        auto next = std::chrono::steady_clock::now();
        while (thread_run.load(std::memory_order_acquire)) {
            step();
            next += std::chrono::nanoseconds(period_ns());
            std::this_thread::sleep_until(next);
        }
    }
};

FakeBackend::Device FakeBackend::umc1820() { return {"UMC1820 Pro", 18, 20, true, 0}; }

FakeBackend::Device FakeBackend::onboard() { return {"Built-in Audio Pro", 2, 2, false, 0}; }

FakeBackend::Config FakeBackend::onboard_config() {
    Config c;
    c.devices = {onboard()};
    return c;
}

double FakeBackend::tone_hz(uint32_t channel) { return 250.0 + 317.0 * channel; }

float FakeBackend::tone(uint32_t channel, uint64_t frame, uint32_t rate) {
    if (rate == 0) return 0.0f;
    // The frequencies are whole hertz, so the phase is exact in integers
    // however far the counter has run.
    const uint64_t hz = 250 + 317 * uint64_t{channel};
    const uint64_t phase = (hz * frame) % rate;
    return static_cast<float>(0.5 * std::sin(kTwoPi * static_cast<double>(phase) / rate));
}

FakeBackend::FakeBackend() : FakeBackend(Config{}) {}

FakeBackend::FakeBackend(Config config) : impl_(std::make_unique<Impl>(std::move(config))) {}

FakeBackend::~FakeBackend() { deactivate(); }

std::vector<PortInfo> FakeBackend::physical_capture_ports() {
    std::lock_guard<std::mutex> lock(impl_->control);
    return impl_->physical(true);
}

std::vector<PortInfo> FakeBackend::physical_playback_ports() {
    std::lock_guard<std::mutex> lock(impl_->control);
    return impl_->physical(false);
}

bool FakeBackend::open(const backend::BackendConfig& config,
                       backend::ProcessCallback& callback, std::string& error) {
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lock(d.control);
    if (d.open) {
        error = "the audio backend is already open";
        return false;
    }
    if (d.server_gone.load()) {
        error = "cannot reach the audio server (the fake server is gone)";
        return false;
    }
    const uint32_t rate = d.server_rate.load();
    if (rate != config.required_rate) {
        error = "server runs at " + std::to_string(rate) + " Hz, session asked for " +
                std::to_string(config.required_rate) + " Hz";
        return false;
    }
    std::vector<PortInfo> chosen;
    if (!backend::select_device(d.physical(true), config.capture_device, chosen, error)) {
        return false;
    }
    // Nothing above changed any state, so a failed open() leaves the
    // backend as it was, ready to be opened again.
    d.capture_device = config.capture_device.empty() && !chosen.empty()
                           ? chosen.front().device
                           : config.capture_device;

    d.rate = rate;
    d.capacity = std::max(kMinBufferFrames, d.server_quantum.load());
    d.inputs = config.inputs;
    d.in_store.assign(config.inputs, std::vector<float>(d.capacity, 0.0f));
    d.out_store.assign(config.outputs, std::vector<float>(d.capacity, 0.0f));
    d.in_bufs.assign(config.inputs, nullptr);
    d.out_bufs.clear();
    for (std::vector<float>& o : d.out_store) d.out_bufs.push_back(o.data());
    d.silence.assign(d.capacity, 0.0f);
    d.input_source = std::make_unique<std::atomic<int32_t>[]>(config.inputs);
    for (uint32_t i = 0; i < config.inputs; ++i) d.input_source[i].store(kNoSource);
    d.out_capture.clear();
    if (d.cfg.output_capture_frames > 0) {
        for (uint32_t o = 0; o < config.outputs; ++o) {
            d.out_capture.push_back(
                std::make_unique<RingBuffer<float>>(d.cfg.output_capture_frames));
        }
    }
    d.block = backend::AudioBlock{
        d.in_bufs.data(),  config.inputs, d.out_bufs.data(), config.outputs, 0, 0,
    };
    d.callback = &callback;
    d.guard.expect_rate(rate);
    d.guard.set_max_frames(d.capacity);
    d.open = true;
    return true;
}

bool FakeBackend::activate(std::string& error) {
    Impl& d = *impl_;
    {
        std::lock_guard<std::mutex> lock(d.control);
        if (!d.open) {
            error = "the audio backend is not open";
            return false;
        }
    }
    if (d.active.load()) return true;
    d.guard.watch_from(d.now_ns());
    d.active.store(true);
    if (d.cfg.mode == Mode::Thread) {
        d.thread_run.store(true, std::memory_order_release);
        d.thread = std::thread([&d] { d.run_thread(); });
    }
    return true;
}

int FakeBackend::connect_inputs_in_order() {
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lock(d.control);
    if (!d.open) return 0;
    std::vector<PortInfo> hw;
    std::string why;
    if (!backend::select_device(d.physical(true), d.capture_device, hw, why)) {
        d.last_warning = "not connecting inputs: " + why;
        return 0;
    }
    if (hw.empty()) return 0;

    const std::size_t n = std::min<std::size_t>(hw.size(), d.inputs);
    int connected = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const int32_t port = d.find(hw[i].name);
        if (port == kNoSource) continue;
        d.link(static_cast<uint32_t>(i), port);
        ++connected;
    }
    if (connected > 0 && d.capture_device.empty()) d.capture_device = hw.front().device;
    if (hw.size() < d.inputs) {
        d.last_warning = std::to_string(hw.size()) + " capture channels for " +
                         std::to_string(d.inputs) + " tracks";
    } else if (hw.size() > d.inputs) {
        d.last_warning = std::to_string(hw.size()) + " capture channels but only " +
                         std::to_string(d.inputs) + " tracks";
    }
    return connected;
}

int FakeBackend::connected_input_count() const {
    int n = 0;
    for (uint32_t i = 0; i < impl_->inputs; ++i) {
        if (input_connected(i)) ++n;
    }
    return n;
}

bool FakeBackend::input_connected(uint32_t index) const {
    return index < impl_->inputs &&
           impl_->input_source[index].load(std::memory_order_acquire) != kNoSource;
}

bool FakeBackend::choose_capture_device(const std::string& device) {
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lock(d.control);
    if (!d.open || !d.capture_device.empty() || device.empty()) return false;
    d.capture_device = device;
    return true;
}

void FakeBackend::deactivate() noexcept {
    Impl& d = *impl_;
    d.active.store(false);
    d.guard.unwatch();
    if (d.thread.joinable()) {
        d.thread_run.store(false, std::memory_order_release);
        try {
            d.thread.join();
        } catch (...) {
            // join() throws only for a thread that is not joinable or is
            // this one; neither can happen here.
        }
    }
    // A synchronous period running on another thread finishes first.
    while (d.in_period.load()) std::this_thread::yield();
    d.clock.finish(d.xruns.load(std::memory_order_relaxed));
}

uint32_t FakeBackend::sample_rate() const { return impl_->rate; }

uint32_t FakeBackend::buffer_frames() const { return impl_->guard.frames(); }

uint32_t FakeBackend::xruns() const { return impl_->xruns.load(std::memory_order_relaxed); }

uint32_t FakeBackend::clock_discontinuities() const { return impl_->clock.discontinuities(); }

bool FakeBackend::fatal() const { return impl_->guard.check_stall(impl_->now_ns()); }

std::string FakeBackend::fatal_reason() const {
    const Impl& d = *impl_;
    drivers::FatalContext context;
    context.rate = d.rate;
    context.admitted_frames = d.guard.frames();
    context.max_frames = d.capacity;
    const drivers::PeriodGuard::State state = d.guard.state();
    if (state.why == Fatal::DeviceGone) {
        std::lock_guard<std::mutex> lock(d.control);
        context.gone_port = d.gone_port;
    }
    return drivers::describe_fatal(state, context);
}

uint32_t FakeBackend::run_blocks(uint32_t n) {
    Impl& d = *impl_;
    uint32_t delivered = 0;
    if (d.cfg.mode != Mode::Synchronous) return 0;
    for (uint32_t i = 0; i < n && d.active.load(); ++i) {
        const uint64_t length = d.period_ns();
        if (d.step()) ++delivered;
        d.virtual_ns.fetch_add(length, std::memory_order_relaxed);
    }
    return delivered;
}

void FakeBackend::advance_time(uint64_t ns) {
    impl_->virtual_ns.fetch_add(ns, std::memory_order_relaxed);
}

uint64_t FakeBackend::now_ns() const { return impl_->now_ns(); }

bool FakeBackend::patch(uint32_t index, const std::string& port) {
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lock(d.control);
    if (!d.open || index >= d.inputs) return false;
    const int32_t p = d.find(port);
    if (p == kNoSource || !d.ports[p].capture || !d.present(d.ports[p].device)) return false;
    d.link(index, p);
    return true;
}

void FakeBackend::unpatch(uint32_t index) {
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lock(d.control);
    if (d.open && index < d.inputs) d.unlink(index);
}

void FakeBackend::inject_xrun(uint32_t frames) {
    impl_->pending_xrun_frames.fetch_add(frames, std::memory_order_acq_rel);
}

void FakeBackend::inject_late_xrun(uint32_t frames) {
    impl_->pending_late_xrun_frames.fetch_add(frames, std::memory_order_acq_rel);
}

void FakeBackend::inject_server_loss() {
    impl_->server_gone.store(true, std::memory_order_release);
    impl_->guard.raise(Fatal::ServerGone);
}

void FakeBackend::inject_rate_change(uint32_t rate, bool notify) {
    impl_->server_rate.store(rate, std::memory_order_release);
    if (notify) impl_->guard.rate_reported(rate);
}

void FakeBackend::inject_quantum_change(uint32_t frames) {
    impl_->server_quantum.store(frames, std::memory_order_release);
}

void FakeBackend::inject_clock_step_back(uint32_t frames) {
    impl_->pending_step_back.fetch_add(frames, std::memory_order_acq_rel);
}

void FakeBackend::inject_unreported_skip(uint32_t frames) {
    impl_->pending_skip_frames.fetch_add(frames, std::memory_order_acq_rel);
}

void FakeBackend::set_stalled(bool stalled) {
    impl_->stalled.store(stalled, std::memory_order_release);
}

bool FakeBackend::unplug(const std::string& device) {
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lock(d.control);
    uint32_t dev = 0;
    while (dev < d.cfg.devices.size() && d.cfg.devices[dev].name != device) ++dev;
    if (dev == d.cfg.devices.size() || !d.present(dev)) return false;
    d.unplugged[dev] = true;
    if (!d.open) return true;
    for (uint32_t i = 0; i < d.inputs; ++i) {
        const int32_t src = d.input_source[i].load(std::memory_order_relaxed);
        if (src != kNoSource && d.ports[src].device == dev) d.unlink(i);
    }
    const uint64_t now = d.now_ns();
    for (const Port& p : d.ports) {
        if (p.device != dev || !p.capture) continue;
        std::string gone;
        if (!d.sources.removed(p.id, p.name.c_str(), now, gone)) continue;
        if (d.guard.fatal() == Fatal::None) d.gone_port = std::move(gone);
        d.guard.raise(Fatal::DeviceGone);
    }
    return true;
}

uint64_t FakeBackend::periods() const { return impl_->guard.periods(); }

uint64_t FakeBackend::process_calls() const {
    return impl_->process_calls.load(std::memory_order_relaxed);
}

uint64_t FakeBackend::unwritten_output_periods() const {
    return impl_->unwritten.load(std::memory_order_relaxed);
}

std::size_t FakeBackend::read_output(uint32_t index, float* dst, std::size_t n) {
    if (index >= impl_->out_capture.size()) return 0;
    return impl_->out_capture[index]->read(dst, n);
}

uint64_t FakeBackend::output_dropped() const {
    return impl_->dropped.load(std::memory_order_relaxed);
}

uint32_t FakeBackend::port_id(const std::string& name) const {
    const int32_t i = impl_->find(name);
    return i == kNoSource ? UINT32_MAX : impl_->ports[i].id;
}

std::string FakeBackend::capture_device() const {
    std::lock_guard<std::mutex> lock(impl_->control);
    return impl_->capture_device;
}

std::string FakeBackend::last_warning() const {
    std::lock_guard<std::mutex> lock(impl_->control);
    return impl_->last_warning;
}

}  // namespace sonora
