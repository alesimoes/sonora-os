// SPDX-License-Identifier: GPL-3.0-or-later
//
// The backend conformance suite against drivers/fake, in each shape that
// takes the suite down a different branch. The same suite runs against
// drivers/jack in tools/test-engine-docker.sh (sonora-conformance-jack).
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <utility>

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

    bool inject(sonora::backend::AudioBackend& b, Fault fault) override {
        auto& f = static_cast<FakeBackend&>(b);
        switch (fault) {
            case Fault::Xrun: f.inject_xrun(cfg_.quantum * 3); return true;
            case Fault::ClockStep: f.inject_clock_step_back(cfg_.quantum * 2); return true;
            case Fault::BufferChange: f.inject_quantum_change(cfg_.quantum * 2); return true;
            case Fault::RateChange:
                f.inject_rate_change(cfg_.rate == 96000 ? 48000 : 96000);
                return true;
            case Fault::Stall: f.set_stalled(true); return true;
            case Fault::Unplug: return f.unplug(unplug_device());
            case Fault::ServerLoss: f.inject_server_loss(); return true;
        }
        return false;
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

}  // namespace

int main() {
    int failed = 0;

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
