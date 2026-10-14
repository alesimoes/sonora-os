// SPDX-License-Identifier: GPL-3.0-or-later
//
// sonora-rtsan-jack-canary: the control for tools/rtsan-jack.sh. Built only
// with -fsanitize=realtime, and run against a live server, which ctest
// does not have.
//
// A clean RTSan run of JackBackend means something only if process_cb itself
// holds a realtime context on the server's thread, still open after its own
// libjack calls. The callback below proves that, and only that: it makes
// one malloc with its own context closed, so what reports it is process_cb's.
// Run with halt_on_error=true it must be reported, with process_cb in the
// stack; exiting 0 means process_cb is not checked, and the clean runs
// beside it prove nothing. With SONORA_RT taken off process_cb it exits 0
// (checked by hand on ci/rtsan-jack, PR #56).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "sonora/jack_backend.h"
#include "sonora/rt.h"

// The runtime's own entry points, noexcept for the same reason as in
// common/platform/src/posix/wake.cpp.
extern "C" void __rtsan_realtime_enter() noexcept;
extern "C" void __rtsan_realtime_exit() noexcept;

namespace {

void* volatile sink = nullptr;

class Allocates final : public sonora::backend::ProcessCallback {
public:
    // Clang gives an override the base's nonblocking whether it is written
    // or not (measured with clang-22: an unmarked override's malloc is
    // reported from a plain caller), so this function opens a context of
    // its own on entry. It closes that one around the malloc and reopens
    // it before the instrumented exit, which closes it again.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfunction-effects"
    void process(const sonora::backend::AudioBlock&) noexcept SONORA_RT override {
        __rtsan_realtime_exit();
        // volatile, so the compiler cannot drop the allocation.
        sink = std::malloc(64);
        __rtsan_realtime_enter();
    }
#pragma clang diagnostic pop
};

}  // namespace

int main() {
    sonora::JackBackend::prepare_process();
    Allocates callback;
    sonora::JackBackend backend;
    sonora::backend::BackendConfig config;
    config.client_name = "sonora-rtsan-canary";
    config.inputs = 2;
    config.outputs = 2;
    config.required_rate = 48000;
    std::string error;
    if (!backend.open(config, callback, error) || !backend.activate(error)) {
        std::fprintf(stderr, "sonora-rtsan-jack-canary: %s\n", error.c_str());
        return 2;
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));
    backend.deactivate();
    std::printf("sonora-rtsan-jack-canary: no report in 2 s\n");
    return 0;
}
