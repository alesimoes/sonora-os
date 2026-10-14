// SPDX-License-Identifier: GPL-3.0-or-later
//
// sonora-rtsan-jack-canary: the control for tools/rtsan-jack.sh. Built only
// with -fsanitize=realtime, and run against a live server, which ctest
// does not have.
//
// A clean RTSan run of JackBackend means something only if RTSan is checking
// process_cb on the server's thread, after process_cb's own libjack calls.
// This callback makes one malloc, after those calls, inside the realtime
// context process_cb opens. Run with halt_on_error=true it must be reported,
// with process_cb in the stack; exiting 0 means RTSan saw nothing, and the
// clean runs beside it prove nothing.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "sonora/jack_backend.h"
#include "sonora/rt.h"

namespace {

void* volatile sink = nullptr;

class Allocates final : public sonora::backend::ProcessCallback {
public:
    void process(const sonora::backend::AudioBlock&) noexcept SONORA_RT override {
        // volatile, so the compiler cannot drop the allocation.
        sink = std::malloc(64);
    }
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
