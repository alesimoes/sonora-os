// SPDX-License-Identifier: GPL-3.0-or-later
//
// The backend conformance suite against drivers/fake, in each shape that
// takes the suite down a different branch. The same suite runs against
// drivers/jack in tools/test-engine-docker.sh (sonora-conformance-jack).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "sonora/backend_conformance.h"
#include "sonora/fake_backend.h"

namespace {

using sonora::FakeBackend;
using sonora::conformance::Fault;

class FakeRig final : public sonora::conformance::Rig {
public:
    explicit FakeRig(FakeBackend::Config config) : cfg_(std::move(config)) {}

    std::unique_ptr<sonora::backend::AudioBackend> make() override {
        return std::make_unique<FakeBackend>(cfg_);
    }
    uint32_t rate() override { return cfg_.rate; }

    void pass(sonora::backend::AudioBackend& b, uint32_t periods) override {
        if (exact()) {
            static_cast<FakeBackend&>(b).run_blocks(periods);
            return;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(
            uint64_t{periods} * cfg_.quantum * 1000000 / cfg_.rate + 1000));
    }
    bool exact() const override { return cfg_.mode == FakeBackend::Mode::Synchronous; }

    Injected inject(sonora::backend::AudioBackend& b, Fault fault) override {
        auto& f = static_cast<FakeBackend&>(b);
        switch (fault) {
            case Fault::Xrun: f.inject_xrun(cfg_.quantum * 3); break;
            case Fault::ClockStep: f.inject_clock_step_back(cfg_.quantum * 2); break;
            case Fault::BufferChange: f.inject_quantum_change(cfg_.quantum * 2); break;
            case Fault::RateChange: f.inject_rate_change(cfg_.rate == 96000 ? 48000 : 96000); break;
            case Fault::Stall: f.set_stalled(true); break;
            case Fault::Unplug:
                return f.unplug(unplug_device()) ? Injected::Caused : Injected::Unsupported;
            case Fault::ServerLoss: f.inject_server_loss(); break;
        }
        return Injected::Caused;
    }
    // The server gets its old clock back, as the JACK rig's does: periods
    // run again, and the backend must still refuse them.
    void restore(sonora::backend::AudioBackend& b, Fault fault) override {
        auto& f = static_cast<FakeBackend&>(b);
        switch (fault) {
            case Fault::BufferChange: f.inject_quantum_change(cfg_.quantum); break;
            case Fault::RateChange: f.inject_rate_change(cfg_.rate); break;
            case Fault::Stall: f.set_stalled(false); break;
            default: break;
        }
    }
    std::string unplug_device() override {
        return cfg_.devices.empty() ? std::string() : cfg_.devices.front().name;
    }

private:
    FakeBackend::Config cfg_;
};

int run(const std::string& name, FakeBackend::Config config) {
    FakeRig rig(std::move(config));
    const sonora::conformance::Result r = sonora::conformance::run(rig, name, stdout);
    return r.failed;
}

// The suite tested by backends that break the contract on purpose: each
// must fail the check that is about it, or the suite proves nothing there.

// Says fatal() as soon as it is stalled, without the inner fake ever
// learning of it, so once the server resumes its periods reach process()
// again. A suite that looked for them while the server was still stopped
// would pass it.
class LeakyBackend final : public sonora::backend::AudioBackend {
public:
    FakeBackend inner;
    std::atomic<bool> stalled{false};

    std::vector<sonora::backend::PortInfo> physical_capture_ports() override {
        return inner.physical_capture_ports();
    }
    std::vector<sonora::backend::PortInfo> physical_playback_ports() override {
        return inner.physical_playback_ports();
    }
    bool open(const sonora::backend::BackendConfig& c, sonora::backend::ProcessCallback& cb,
              std::string& e) override {
        return inner.open(c, cb, e);
    }
    bool activate(std::string& e) override { return inner.activate(e); }
    int connect_inputs_in_order() override { return inner.connect_inputs_in_order(); }
    int connected_input_count() const override { return inner.connected_input_count(); }
    bool input_connected(uint32_t i) const override { return inner.input_connected(i); }
    bool choose_capture_device(const std::string& d) override {
        return inner.choose_capture_device(d);
    }
    void deactivate() noexcept override { inner.deactivate(); }
    uint32_t sample_rate() const override { return inner.sample_rate(); }
    uint32_t buffer_frames() const override { return inner.buffer_frames(); }
    uint32_t xruns() const override { return inner.xruns(); }
    uint32_t clock_discontinuities() const override { return inner.clock_discontinuities(); }
    bool fatal() const override { return stalled.load(); }
    std::string fatal_reason() const override {
        return stalled.load() ? "the audio server ran no period for over a second" : "";
    }
};

class LeakyRig final : public sonora::conformance::Rig {
public:
    std::unique_ptr<sonora::backend::AudioBackend> make() override {
        return std::make_unique<LeakyBackend>();
    }
    uint32_t rate() override { return 48000; }
    void pass(sonora::backend::AudioBackend& b, uint32_t periods) override {
        static_cast<LeakyBackend&>(b).inner.run_blocks(periods);
    }
    bool exact() const override { return true; }
    Injected inject(sonora::backend::AudioBackend& b, Fault fault) override {
        if (fault != Fault::Stall) return Injected::Unsupported;
        auto& l = static_cast<LeakyBackend&>(b);
        l.inner.set_stalled(true);
        l.stalled.store(true);
        return Injected::Caused;
    }
    void restore(sonora::backend::AudioBackend& b, Fault) override {
        static_cast<LeakyBackend&>(b).inner.set_stalled(false);
    }
    std::string unplug_device() override { return ""; }
};

// A server that refuses the change: the check is skipped with the rig's
// reason, but only if the backend carried on; one that goes fatal over a
// change the server never made fails.
class HeldRig final : public sonora::conformance::Rig {
public:
    explicit HeldRig(bool backend_breaks) : breaks_(backend_breaks) {}
    std::unique_ptr<sonora::backend::AudioBackend> make() override {
        return std::make_unique<FakeBackend>();
    }
    uint32_t rate() override { return 48000; }
    void pass(sonora::backend::AudioBackend& b, uint32_t periods) override {
        static_cast<FakeBackend&>(b).run_blocks(periods);
    }
    bool exact() const override { return true; }
    Injected inject(sonora::backend::AudioBackend& b, Fault) override {
        if (breaks_) static_cast<FakeBackend&>(b).inject_server_loss();
        return Injected::Held;
    }
    std::string held_reason(Fault) override { return "pinned"; }
    std::string unplug_device() override { return ""; }

private:
    bool breaks_;
};

int test_the_suite_itself() {
    int failed = 0;
    LeakyRig leaky;
    const std::string leak = sonora::conformance::check_fault(leaky, Fault::Stall);
    if (leak.rfind("process() ran", 0) != 0) {
        std::fprintf(stderr,
                     "FAIL: a backend that runs process() after fatal() once the server"
                     " resumes passed the stall check: \"%s\"\n",
                     leak.c_str());
        ++failed;
    }
    HeldRig held(false);
    const std::string skipped = sonora::conformance::check_fault(held, Fault::BufferChange);
    if (skipped != "SKIP: pinned") {
        std::fprintf(stderr, "FAIL: a held change was not skipped with its reason: \"%s\"\n",
                     skipped.c_str());
        ++failed;
    }
    HeldRig broken(true);
    const std::string wrong = sonora::conformance::check_fault(broken, Fault::RateChange);
    if (wrong.rfind("the server held the change, yet the backend is fatal", 0) != 0) {
        std::fprintf(stderr, "FAIL: a backend fatal over a held change passed: \"%s\"\n",
                     wrong.c_str());
        ++failed;
    }
    if (failed == 0) std::printf("ok: the suite fails the backends built to fail it\n");
    return failed;
}

}  // namespace

int main() {
    int failed = test_the_suite_itself();

    failed += run("fake umc1820", FakeBackend::Config{});
    failed += run("fake onboard", FakeBackend::onboard_config());

    FakeBackend::Config laptop;
    laptop.devices = {FakeBackend::onboard(), FakeBackend::umc1820()};
    failed += run("fake onboard+umc1820", laptop);

    FakeBackend::Config two;
    FakeBackend::Device second = FakeBackend::umc1820();
    second.name = "UMC1820 Pro #2";
    two.devices = {FakeBackend::umc1820(), second};
    failed += run("fake two interfaces", two);

    // Everything the ordering has gone wrong on at once, plus a counter
    // that wraps within the first few periods and silence, not nullptr,
    // for unconnected inputs.
    FakeBackend::Config injected;
    injected.naming = FakeBackend::Naming::Numbered;
    injected.list_order = FakeBackend::ListOrder::Lexicographic;
    injected.ids_out_of_order = true;
    injected.first_frame = 0xFFFFFFFFu - 1000;
    injected.silence_for_unconnected = true;
    injected.quantum = 256;
    injected.rate = 44100;
    failed += run("fake injected", injected);

    FakeBackend::Config threaded;
    threaded.mode = FakeBackend::Mode::Thread;
    failed += run("fake umc1820 thread", threaded);

    if (failed == 0) std::printf("backend_conformance_fake: all rigs passed\n");
    return failed == 0 ? 0 : 1;
}
