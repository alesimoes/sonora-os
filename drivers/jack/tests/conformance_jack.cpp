// SPDX-License-Identifier: GPL-3.0-or-later
//
// sonora-conformance-jack: the backend conformance suite against JackBackend
// and a running server. Not in ctest, which has no server; run by
// tools/test-engine-entrypoint.sh against PipeWire in the runtime container.
//
// The faults are caused from outside, by shell commands the caller passes
// in the environment, since breaking a server is not something a client can
// do to itself: SONORA_INJECT_<FAULT> causes it and SONORA_RESTORE_<FAULT>,
// if set, undoes it before the next check. <FAULT> is the suite's name in
// capitals (XRUN, CLOCK_STEP, BUFFER_CHANGE, RATE_CHANGE, STALL, UNPLUG,
// SERVER_LOSS). A fault with no command is skipped, and says so.
// SONORA_UNPLUG_DEVICE names the device SONORA_INJECT_UNPLUG removes;
// SONORA_CONFORMANCE_RATE is the server's rate (default 48000).
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include "sonora/backend_conformance.h"
#include "sonora/jack_backend.h"

namespace {

using sonora::conformance::Fault;

std::string env(const std::string& name) {
    const char* v = std::getenv(name.c_str());
    return v ? v : "";
}

std::string variable(const char* prefix, Fault f) {
    std::string name = prefix;
    for (const char* c = sonora::conformance::fault_name(f); *c; ++c) {
        name += static_cast<char>(std::toupper(static_cast<unsigned char>(*c)));
    }
    return name;
}

class JackRig final : public sonora::conformance::Rig {
public:
    std::unique_ptr<sonora::backend::AudioBackend> make() override {
        return std::make_unique<sonora::JackBackend>();
    }
    uint32_t rate() override {
        const std::string r = env("SONORA_CONFORMANCE_RATE");
        return r.empty() ? 48000 : static_cast<uint32_t>(std::strtoul(r.c_str(), nullptr, 10));
    }
    void pass(sonora::backend::AudioBackend& b, uint32_t periods) override {
        const uint64_t frames = b.buffer_frames() ? b.buffer_frames() : 1024;
        std::this_thread::sleep_for(
            std::chrono::microseconds(periods * frames * 1000000 / rate() + 1000));
    }
    bool exact() const override { return false; }

    bool inject(sonora::backend::AudioBackend&, Fault f) override {
        const std::string command = env(variable("SONORA_INJECT_", f));
        if (command.empty()) return false;
        std::printf("  inject %s: %s\n", sonora::conformance::fault_name(f), command.c_str());
        std::fflush(stdout);
        return std::system(command.c_str()) == 0;
    }
    void restore(Fault f) override {
        const std::string command = env(variable("SONORA_RESTORE_", f));
        if (command.empty()) return;
        std::printf("  restore %s: %s\n", sonora::conformance::fault_name(f), command.c_str());
        std::fflush(stdout);
        if (std::system(command.c_str()) != 0) {
            std::printf("  restore %s failed\n", sonora::conformance::fault_name(f));
        }
    }
    std::string unplug_device() override { return env("SONORA_UNPLUG_DEVICE"); }
};

}  // namespace

int main() {
    // As sonora-engine does, before any thread exists.
    sonora::JackBackend::prepare_process();
    JackRig rig;
    const sonora::conformance::Result r = sonora::conformance::run(rig, "jack", stdout);
    return r.failed == 0 && r.passed > 0 ? 0 : 1;
}
