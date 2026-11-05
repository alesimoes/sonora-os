// SPDX-License-Identifier: GPL-3.0-or-later
//
// AlsaBackend on a real ALSA device, outputs = inputs, and a report. Run by
// tools/test-alsa-loopback.sh against snd-aloop, and by hand on the UMC1820:
//
//   sonora-alsa-passthrough --device UMC1820 --seconds 30 [--period 128] [--periods 2]
//
// Prints one machine-readable line, "alsa-passthrough key=value ...", which
// is what goes into brain/research with the model, kernel and host. Exit 0
// when the stream ran with no fatal condition, 1 when it did not, 2 on usage.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "sonora/alsa_backend.h"

namespace {

class Passthrough final : public sonora::backend::ProcessCallback {
public:
    void process(const sonora::backend::AudioBlock& b) noexcept override {
        const uint32_t n = b.input_count < b.output_count ? b.input_count : b.output_count;
        for (uint32_t c = 0; c < b.output_count; ++c) {
            for (uint32_t i = 0; i < b.nframes; ++i) {
                b.outputs[c][i] = c < n && b.inputs[c] ? b.inputs[c][i] : 0.0f;
            }
        }
        if (last_ns_ != 0 && b.callback_ns > last_ns_) {
            const uint64_t d = b.callback_ns - last_ns_;
            if (d > max_.load(std::memory_order_relaxed)) max_.store(d, std::memory_order_relaxed);
            if (d < min_.load(std::memory_order_relaxed)) min_.store(d, std::memory_order_relaxed);
        }
        last_ns_ = b.callback_ns;
        periods_.fetch_add(1, std::memory_order_relaxed);
    }
    std::atomic<uint64_t> periods_{0};
    std::atomic<uint64_t> max_{0};
    std::atomic<uint64_t> min_{UINT64_MAX};
    uint64_t last_ns_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    std::string device;
    unsigned seconds = 5, rate = 48000;
    sonora::AlsaBackend::Options options;
    for (int i = 1; i < argc; ++i) {
        const bool more = i + 1 < argc;
        if (!std::strcmp(argv[i], "--device") && more) device = argv[++i];
        else if (!std::strcmp(argv[i], "--seconds") && more) seconds = static_cast<unsigned>(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--rate") && more) rate = static_cast<unsigned>(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--period") && more) options.period_frames = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--periods") && more) options.periods = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--rtprio") && more) options.rt_priority = std::atoi(argv[++i]);
        else {
            std::fprintf(stderr, "usage: %s --device ID [--seconds N] [--rate HZ] [--period N] [--periods N] [--rtprio N]\n", argv[0]);
            return 2;
        }
    }
    sonora::AlsaBackend backend(options);
    const auto in = backend.physical_capture_ports();
    const auto out = backend.physical_playback_ports();
    if (in.empty()) {
        std::fprintf(stderr, "no ALSA capture device found\n");
        return 1;
    }
    std::string ids;
    for (const auto& p : in) {
        if (p.channel == 0) ids += (ids.empty() ? "" : ",") + p.device;
    }
    std::fprintf(stderr, "cards with capture: %s\n", ids.c_str());

    Passthrough cb;
    sonora::backend::BackendConfig cfg{"sonora-alsa-passthrough", 0, 0, rate, device};
    // As many tracks as the card has, so the whole card is exercised.
    uint32_t channels = 0, playback = 0;
    for (const auto& p : in) {
        if (device.empty() || p.device == device) channels = p.channel + 1;
    }
    for (const auto& p : out) {
        if (device.empty() || p.device == device) playback = p.channel + 1;
    }
    cfg.inputs = channels;
    cfg.outputs = playback;
    std::string error;
    if (!backend.open(cfg, cb, error) || !backend.activate(error)) {
        std::fprintf(stderr, "alsa: %s\n", error.c_str());
        return 1;
    }
    const int connected = backend.connect_inputs_in_order();
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    const bool fatal = backend.fatal();
    const std::string why = backend.fatal_reason();
    std::printf(
        "alsa-passthrough device=%s rate=%u period=%u ring=%u inputs=%u outputs=%u connected=%d "
        "periods=%llu xruns=%u rtprio=%d callback_min_ns=%llu callback_max_ns=%llu "
        "reported_latency_frames=%u fatal=%d\n",
        backend.capture_device().c_str(), rate, backend.buffer_frames(), backend.ring_frames(),
        cfg.inputs, cfg.outputs, connected, static_cast<unsigned long long>(cb.periods_.load()),
        backend.xruns(), backend.rt_priority_granted(),
        static_cast<unsigned long long>(cb.min_.load() == UINT64_MAX ? 0 : cb.min_.load()),
        static_cast<unsigned long long>(cb.max_.load()), backend.reported_latency(), fatal ? 1 : 0);
    if (fatal) std::fprintf(stderr, "fatal: %s\n", why.c_str());
    backend.deactivate();
    return fatal || cb.periods_.load() == 0 ? 1 : 0;
}
