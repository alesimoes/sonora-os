// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/backend_conformance.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

namespace sonora::conformance {
namespace {

using backend::AudioBackend;
using backend::AudioBlock;
using backend::PortInfo;

// Polls are bounded in passes, not in time: a pass is about this many
// periods, and 200 of them are over 8 s at 1024 frames and 48 kHz, well past
// the one second a stall takes to be noticed.
constexpr uint32_t kPassPeriods = 32;
constexpr int kMaxPasses = 200;

// What the backend handed over, judged inside process() itself. Only the
// audio thread writes the plain counters' atomics, so they need no RMW.
class Recorder final : public backend::ProcessCallback {
public:
    Recorder(uint32_t inputs, uint32_t outputs) : inputs_(inputs), outputs_(outputs) {}

    void process(const AudioBlock& b) noexcept SONORA_RT override {
        // A call that starts after the suite saw fatal() is a violation; one
        // that started before may still be running, which is allowed.
        if (fatal_seen_.load(std::memory_order_acquire)) bump(after_fatal_);
        const uint64_t calls = calls_.load(std::memory_order_relaxed) + 1;
        if (b.input_count != inputs_ || b.output_count != outputs_) bump(shape_errors_);
        for (uint32_t o = 0; o < b.output_count; ++o) {
            if (b.outputs[o] == nullptr) {
                bump(null_outputs_);
            } else {
                std::memset(b.outputs[o], 0, b.nframes * sizeof(float));
            }
        }
        const uint32_t first = first_nframes_.load(std::memory_order_relaxed);
        if (first == 0) {
            first_nframes_.store(b.nframes, std::memory_order_relaxed);
        } else if (b.nframes != first) {
            bump(size_changes_);
        }
        if (calls > 1) {
            if (b.frame_time < last_time_) {
                bump(decreases_);
            } else if (b.frame_time != last_time_ + last_nframes_) {
                bump(gaps_);
            }
        }
        last_time_ = b.frame_time;
        last_nframes_ = b.nframes;
        calls_.store(calls, std::memory_order_release);
    }

    void mark_fatal() { fatal_seen_.store(true, std::memory_order_release); }

    uint64_t calls() const { return calls_.load(std::memory_order_acquire); }
    uint32_t first_nframes() const { return first_nframes_.load(std::memory_order_relaxed); }
    uint32_t after_fatal() const { return get(after_fatal_); }
    uint32_t shape_errors() const { return get(shape_errors_); }
    uint32_t null_outputs() const { return get(null_outputs_); }
    uint32_t size_changes() const { return get(size_changes_); }
    uint32_t decreases() const { return get(decreases_); }
    uint32_t gaps() const { return get(gaps_); }

private:
    static void bump(std::atomic<uint32_t>& a) noexcept SONORA_RT {
        a.store(a.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }
    static uint32_t get(const std::atomic<uint32_t>& a) {
        return a.load(std::memory_order_relaxed);
    }

    const uint32_t inputs_;
    const uint32_t outputs_;
    std::atomic<bool> fatal_seen_{false};
    std::atomic<uint64_t> calls_{0};
    std::atomic<uint32_t> first_nframes_{0};
    std::atomic<uint32_t> after_fatal_{0};
    std::atomic<uint32_t> shape_errors_{0};
    std::atomic<uint32_t> null_outputs_{0};
    std::atomic<uint32_t> size_changes_{0};
    std::atomic<uint32_t> decreases_{0};
    std::atomic<uint32_t> gaps_{0};
    // Audio thread only.
    uint64_t last_time_ = 0;
    uint32_t last_nframes_ = 0;
};

// The recorder is declared first so it outlives the backend, whose
// destructor deactivates: the callback must outlive activation.
struct Fixture {
    Recorder rec;
    std::unique_ptr<AudioBackend> b;

    Fixture(Rig& rig, uint32_t inputs, uint32_t outputs)
        : rec(inputs, outputs), b(rig.make()), inputs_(inputs), outputs_(outputs) {}

    bool open(Rig& rig, const std::string& device, std::string& error) {
        return b->open({"sonora-conformance", inputs_, outputs_, rig.rate(), device}, rec,
                       error);
    }
    bool start(Rig& rig, const std::string& device, std::string& error) {
        return open(rig, device, error) && b->activate(error);
    }

private:
    uint32_t inputs_, outputs_;
};

bool wait_calls(Rig& rig, Fixture& f, uint64_t target) {
    for (int i = 0; i < kMaxPasses; ++i) {
        if (f.rec.calls() >= target) return true;
        rig.pass(*f.b, kPassPeriods);
    }
    return f.rec.calls() >= target;
}

bool wait_connected(Rig& rig, Fixture& f, int target) {
    for (int i = 0; i < kMaxPasses; ++i) {
        if (f.b->connected_input_count() == target) return true;
        rig.pass(*f.b, kPassPeriods);
    }
    return f.b->connected_input_count() == target;
}

bool wait_fatal(Rig& rig, Fixture& f) {
    for (int i = 0; i < kMaxPasses; ++i) {
        if (f.b->fatal()) {
            f.rec.mark_fatal();
            return true;
        }
        rig.pass(*f.b, kPassPeriods);
    }
    return false;
}

std::string n(uint64_t v) { return std::to_string(v); }

// The devices present, in order, and which of them are USB.
struct Devices {
    std::vector<std::string> usb, other;
    uint32_t count(const std::vector<PortInfo>& ports, const std::string& device) const {
        return static_cast<uint32_t>(std::count_if(
            ports.begin(), ports.end(), [&](const PortInfo& p) { return p.device == device; }));
    }
};

Devices devices_of(const std::vector<PortInfo>& ports) {
    Devices d;
    for (const PortInfo& p : ports) {
        std::vector<std::string>& bus = p.usb ? d.usb : d.other;
        if (std::find(bus.begin(), bus.end(), p.device) == bus.end()) bus.push_back(p.device);
    }
    return d;
}

// The device a check that is not about the policy opens with: none named,
// unless the default policy would refuse to guess between interfaces.
std::string usable_device(Rig& rig) {
    const std::unique_ptr<AudioBackend> probe = rig.make();
    const Devices d = devices_of(probe->physical_capture_ports());
    return d.usb.size() > 1 ? d.usb.front() : std::string();
}

// Each check returns "" when it passes, "SKIP: why" when it cannot run on
// this rig, and the failure otherwise.
using Check = std::string (*)(Rig&);

std::string open_refuses_another_rate(Rig& rig) {
    Recorder rec(2, 2);
    const std::unique_ptr<AudioBackend> b = rig.make();
    std::string error;
    if (b->open({"sonora-conformance", 2, 2, rig.rate() + 1, usable_device(rig)}, rec, error)) {
        return "open() accepted " + n(rig.rate() + 1) + " Hz on a server at " + n(rig.rate());
    }
    if (error.empty()) return "open() failed without saying why";
    // A failed open() leaves the backend usable.
    error.clear();
    if (!b->open({"sonora-conformance", 2, 2, rig.rate(), usable_device(rig)}, rec, error)) {
        return "open() after a refused one failed: " + error;
    }
    return "";
}

std::string periods_reach_process(Rig& rig) {
    Fixture f(rig, 4, 3);
    std::string error;
    if (!f.open(rig, usable_device(rig), error)) return "open: " + error;
    if (f.b->buffer_frames() != 0) return "buffer_frames() is not 0 before the first period";
    if (!f.b->activate(error)) return "activate: " + error;
    if (!wait_calls(rig, f, 8)) return "no periods reached process()";
    if (f.b->sample_rate() != rig.rate()) {
        return "sample_rate() is " + n(f.b->sample_rate()) + ", the server's is " + n(rig.rate());
    }
    if (f.b->buffer_frames() == 0 || f.b->buffer_frames() != f.rec.first_nframes()) {
        return "buffer_frames() is " + n(f.b->buffer_frames()) + ", the first period was " +
               n(f.rec.first_nframes());
    }
    if (f.rec.shape_errors() != 0) return "a block had the wrong input or output count";
    if (f.rec.null_outputs() != 0) return "a block had a null output";
    if (f.rec.size_changes() != 0) return "a block of another size reached process()";
    return "";
}

std::string frame_time_never_decreases(Rig& rig) {
    Fixture f(rig, 2, 2);
    std::string error;
    if (!f.start(rig, usable_device(rig), error)) return "start: " + error;
    if (!wait_calls(rig, f, 64)) return "no periods reached process()";
    f.b->deactivate();
    if (f.rec.decreases() != 0) return n(f.rec.decreases()) + " decreases of frame_time";
    // Contiguous unless frames were lost or the clock changed.
    const uint32_t excused = f.b->xruns() + f.b->clock_discontinuities();
    if (f.rec.gaps() > excused || (rig.exact() && f.rec.gaps() != 0)) {
        return n(f.rec.gaps()) + " gaps in frame_time with " + n(f.b->xruns()) + " xruns and " +
               n(f.b->clock_discontinuities()) + " clock steps";
    }
    return "";
}

std::string absent_device_is_not_an_error(Rig& rig) {
    Fixture f(rig, 2, 2);
    std::string error;
    if (!f.start(rig, "sonora-conformance-absent", error)) {
        return "a device not present failed open(): " + error;
    }
    if (f.b->connect_inputs_in_order() != 0) return "connected inputs from an absent device";
    if (f.b->connected_input_count() != 0) return "inputs connected from an absent device";
    // Named, the device is chosen: nothing may replace it.
    if (f.b->choose_capture_device("anything")) {
        return "choose_capture_device() replaced a named device";
    }
    return "";
}

std::string default_device_policy(Rig& rig) {
    const std::unique_ptr<AudioBackend> probe = rig.make();
    // Usable before open(), so the session can be sized to the interface.
    const std::vector<PortInfo> ports = probe->physical_capture_ports();
    const Devices d = devices_of(ports);
    if (d.usb.empty() && d.other.empty()) return "SKIP: no capture device present";

    if (d.usb.size() > 1) {
        Fixture f(rig, 2, 2);
        std::string error;
        if (f.open(rig, "", error)) return "open() guessed between USB interfaces";
        for (const std::string& name : d.usb) {
            if (error.find(name) == std::string::npos) {
                return "the refusal does not name \"" + name + "\": " + error;
            }
        }
        return "";
    }

    const std::string device = d.usb.empty() ? d.other.front() : d.usb.front();
    const int expected = static_cast<int>(d.count(ports, device));
    Fixture f(rig, static_cast<uint32_t>(expected), 2);
    std::string error;
    if (!f.start(rig, "", error)) return "start: " + error;
    if (d.usb.empty()) {
        // No interface yet: nothing is chosen while the caller waits, and
        // the caller's fallback chooses once.
        if (f.b->connect_inputs_in_order() != 0) {
            return "connected a non-USB device before the caller chose it";
        }
        if (!f.b->choose_capture_device(device)) return "choose_capture_device() refused";
        if (f.b->choose_capture_device(device)) {
            return "choose_capture_device() chose twice";
        }
    } else if (!d.other.empty() && f.b->choose_capture_device(d.other.front())) {
        return "choose_capture_device() moved the inputs off the USB interface";
    }
    const int connected = f.b->connect_inputs_in_order();
    if (connected != expected) {
        return "connected " + n(static_cast<uint64_t>(connected)) + " of " + device + "'s " +
               n(static_cast<uint64_t>(expected)) + " channels";
    }
    if (!wait_connected(rig, f, expected)) {
        return "connected_input_count() never reached " + n(static_cast<uint64_t>(expected));
    }
    return "";
}

std::string connect_is_idempotent(Rig& rig) {
    const std::unique_ptr<AudioBackend> probe = rig.make();
    const std::vector<PortInfo> ports = probe->physical_capture_ports();
    if (ports.empty()) return "SKIP: no capture device present";
    const std::string device = ports.front().device;
    const int channels = static_cast<int>(devices_of(ports).count(ports, device));
    // One input more than the device has channels, so one stays unconnected.
    const uint32_t inputs = static_cast<uint32_t>(channels) + 1;
    Fixture f(rig, inputs, 2);
    std::string error;
    if (!f.start(rig, device, error)) return "start: " + error;
    const int first = f.b->connect_inputs_in_order();
    const int second = f.b->connect_inputs_in_order();
    if (first != channels || second != channels) {
        return "connect_inputs_in_order() returned " + n(static_cast<uint64_t>(first)) +
               " then " + n(static_cast<uint64_t>(second)) + " for " +
               n(static_cast<uint64_t>(channels)) + " channels";
    }
    if (!wait_connected(rig, f, channels)) {
        return "connected_input_count() is " +
               n(static_cast<uint64_t>(f.b->connected_input_count())) + ", not " +
               n(static_cast<uint64_t>(channels));
    }
    for (uint32_t i = 0; i < static_cast<uint32_t>(channels); ++i) {
        if (!f.b->input_connected(i)) return "input " + n(i) + " is not connected";
    }
    if (f.b->input_connected(inputs - 1)) return "the input past the channels is connected";
    if (f.b->input_connected(inputs + 5)) return "an input that does not exist is connected";
    return "";
}

std::string deactivate_is_a_barrier(Rig& rig) {
    Fixture f(rig, 2, 2);
    std::string error;
    if (!f.start(rig, usable_device(rig), error)) return "start: " + error;
    if (!wait_calls(rig, f, 4)) return "no periods reached process()";
    f.b->deactivate();
    const uint64_t calls = f.rec.calls();
    rig.pass(*f.b, kPassPeriods);
    if (f.rec.calls() != calls) return "process() ran after deactivate() returned";
    f.b->deactivate();
    return "";
}

std::string check_fault(Rig& rig, Fault fault) {
    std::string device = usable_device(rig);
    uint32_t inputs = 2;
    if (fault == Fault::Unplug) {
        device = rig.unplug_device();
        if (device.empty()) return "SKIP: this rig has no device to unplug";
        const std::unique_ptr<AudioBackend> probe = rig.make();
        const std::vector<PortInfo> ports = probe->physical_capture_ports();
        inputs = Devices{}.count(ports, device);
        if (inputs == 0) return "SKIP: " + device + " is not present";
    }
    Fixture f(rig, inputs, 2);
    std::string error;
    if (!f.start(rig, device, error)) return "start: " + error;
    if (fault == Fault::Unplug) {
        f.b->connect_inputs_in_order();
        if (!wait_connected(rig, f, static_cast<int>(inputs))) return "could not connect " + device;
    }
    if (!wait_calls(rig, f, 8)) return "no periods reached process()";

    const uint64_t before = f.rec.calls();
    const uint32_t xruns = f.b->xruns();
    if (!rig.inject(*f.b, fault)) return "SKIP: this rig cannot cause it";
    struct Restore {
        Rig& rig;
        Fault fault;
        ~Restore() { rig.restore(fault); }
    } const restore{rig, fault};

    if (fault == Fault::Xrun || fault == Fault::ClockStep) {
        if (!wait_calls(rig, f, before + 8)) return "periods stopped";
        if (f.b->fatal()) return "fatal: " + f.b->fatal_reason();
        if (f.rec.decreases() != 0) return "frame_time decreased";
        if (fault == Fault::Xrun) {
            if (f.b->xruns() <= xruns) return "xruns() did not count it";
            if (f.rec.gaps() == 0) return "frame_time shows no gap";
        } else if (f.b->clock_discontinuities() == 0) {
            return "clock_discontinuities() did not count it";
        }
        return "";
    }

    if (!wait_fatal(rig, f)) return "fatal() never became true";
    const std::string reason = f.b->fatal_reason();
    std::string expected;
    switch (fault) {
        case Fault::BufferChange: expected = "buffer size"; break;
        case Fault::RateChange: expected = "sample rate"; break;
        case Fault::Stall: expected = "ran no period"; break;
        case Fault::Unplug: expected = "capture device " + device + " went away"; break;
        case Fault::ServerLoss: expected = "server went away"; break;
        default: break;
    }
    if (reason.find(expected) == std::string::npos) {
        return "fatal_reason() is \"" + reason + "\", expected it to say \"" + expected + "\"";
    }
    const uint64_t at_fatal = f.rec.calls();
    rig.pass(*f.b, kPassPeriods);
    // A call already running when fatal() turned true may finish, and is
    // counted when it does; only an exact rig can rule that out.
    const uint64_t finished = rig.exact() ? 0 : 1;
    if (f.rec.after_fatal() != 0 || f.rec.calls() > at_fatal + finished) {
        return "process() ran " + n(f.rec.calls() - at_fatal) + " times after fatal()";
    }
    if (f.rec.size_changes() != 0) return "a block of the new size reached process()";
    // The first period at a new rate or size is the one refused.
    if (rig.exact() && (fault == Fault::BufferChange || fault == Fault::RateChange) &&
        at_fatal != before) {
        return n(at_fatal - before) + " periods reached process() after the change";
    }
    return "";
}

}  // namespace

const char* fault_name(Fault f) {
    switch (f) {
        case Fault::Xrun: return "xrun";
        case Fault::ClockStep: return "clock_step";
        case Fault::BufferChange: return "buffer_change";
        case Fault::RateChange: return "rate_change";
        case Fault::Stall: return "stall";
        case Fault::Unplug: return "unplug";
        case Fault::ServerLoss: return "server_loss";
    }
    return "?";
}

Result run(Rig& rig, const std::string& rig_name, std::FILE* out) {
    Result result;
    const auto report = [&](const std::string& name, const std::string& outcome) {
        if (outcome.empty()) {
            ++result.passed;
            std::fprintf(out, "ok %s: %s\n", rig_name.c_str(), name.c_str());
        } else if (outcome.rfind("SKIP: ", 0) == 0) {
            ++result.skipped;
            std::fprintf(out, "SKIP %s: %s: %s\n", rig_name.c_str(), name.c_str(),
                         outcome.c_str() + 6);
        } else {
            ++result.failed;
            std::fprintf(out, "FAIL %s: %s: %s\n", rig_name.c_str(), name.c_str(),
                         outcome.c_str());
        }
        std::fflush(out);
    };

    const struct {
        const char* name;
        Check check;
    } checks[] = {
        {"open_refuses_another_rate", open_refuses_another_rate},
        {"periods_reach_process", periods_reach_process},
        {"frame_time_never_decreases", frame_time_never_decreases},
        {"absent_device_is_not_an_error", absent_device_is_not_an_error},
        {"default_device_policy", default_device_policy},
        {"connect_is_idempotent", connect_is_idempotent},
        {"deactivate_is_a_barrier", deactivate_is_a_barrier},
    };
    for (const auto& c : checks) report(c.name, c.check(rig));

    for (Fault fault : {Fault::Xrun, Fault::ClockStep, Fault::BufferChange, Fault::RateChange,
                        Fault::Stall, Fault::Unplug, Fault::ServerLoss}) {
        report(std::string("fault_") + fault_name(fault), check_fault(rig, fault));
    }
    std::fprintf(out, "RESULT %s: %d ok, %d FAIL, %d SKIP\n", rig_name.c_str(), result.passed,
                 result.failed, result.skipped);
    return result;
}

}  // namespace sonora::conformance
