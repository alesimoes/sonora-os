// SPDX-License-Identifier: GPL-3.0-or-later
//
// Misbehaving LV2 plugins for testing the host (ADR-0007 §7). Each one does
// on purpose what real plugins do by accident. Never installed; the test
// driver loads the bundle from the build tree.
//
//   urn:sonora:test#crash  mono pass-through; segfaults when `crash` > 0.5
//   urn:sonora:test#sleep  mono pass-through; sleeps `sleep_ms` per block
//   urn:sonora:test#liar   declares stereo, writes 8 channels into output 0
//   urn:sonora:test#nan    mono pass-through; writes NaN and Inf when `nan` > 0.5
//   urn:sonora:test#lookahead  mono, 100 frames of reported latency and a
//                          `gain` in dB, with no bypass port: what the host
//                          bypasses with its own delay line (ADR-0012 §5.1)
//   urn:sonora:test#key    mono, with a key input: outputs its key, then
//                          scribbles over the key's `n` samples
//   urn:sonora:test#keyliar  the same, and writes 64 samples past its key

#include <lv2/core/lv2.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>

namespace {

enum Kind { kCrash, kSleep, kLiar, kNan, kLookahead, kKey, kKeyLiar };

constexpr uint32_t kLookaheadFrames = 100;
constexpr uint32_t kLookaheadRing = 256;  // a power of two above the delay

struct Plugin {
    Kind kind;
    const float* control = nullptr;
    const float* in[2] = {nullptr, nullptr};
    float* out[2] = {nullptr, nullptr};
    float* latency = nullptr;
    float ring[kLookaheadRing] = {};
    uint32_t write = 0;
    // Written through on purpose: what a plugin that uses its inputs as
    // scratch space does.
    float* key = nullptr;
};

// Read through a volatile so the compiler cannot prove the address is null
// and replace the store with a trap: the test wants a genuine SIGSEGV.
volatile std::uintptr_t g_nowhere = 0;

template <Kind K>
LV2_Handle instantiate(const LV2_Descriptor*, double, const char*, const LV2_Feature* const*) {
    auto* p = new Plugin;
    p->kind = K;
    return p;
}

void connect(LV2_Handle h, uint32_t port, void* data) {
    auto* p = static_cast<Plugin*>(h);
    if (p->kind == kLiar) {
        // 0, 1: in L/R. 2, 3: out L/R.
        if (port < 2) p->in[port] = static_cast<const float*>(data);
        else if (port < 4) p->out[port - 2] = static_cast<float*>(data);
        return;
    }
    if (p->kind == kKey || p->kind == kKeyLiar) {
        // 0: in. 1: key (lv2:isSideChain). 2: out.
        if (port == 0) p->in[0] = static_cast<const float*>(data);
        else if (port == 1) p->key = static_cast<float*>(data);
        else if (port == 2) p->out[0] = static_cast<float*>(data);
        return;
    }
    // 0: control. 1: in. 2: out. 3: latency (lookahead only).
    if (port == 0) p->control = static_cast<const float*>(data);
    else if (port == 1) p->in[0] = static_cast<const float*>(data);
    else if (port == 2) p->out[0] = static_cast<float*>(data);
    else if (port == 3) p->latency = static_cast<float*>(data);
}

void run(LV2_Handle h, uint32_t n) {
    auto* p = static_cast<Plugin*>(h);
    switch (p->kind) {
    case kCrash:
        if (p->control && *p->control > 0.5f)
            *reinterpret_cast<volatile int*>(g_nowhere) = 1;
        std::memcpy(p->out[0], p->in[0], n * sizeof(float));
        break;
    case kSleep:
        if (p->control && *p->control > 0.f) {
            const long ns = static_cast<long>(*p->control * 1e6f);
            timespec ts{ns / 1000000000L, ns % 1000000000L};
            nanosleep(&ts, nullptr);
        }
        std::memcpy(p->out[0], p->in[0], n * sizeof(float));
        break;
    case kNan:
        std::memcpy(p->out[0], p->in[0], n * sizeof(float));
        // What a filter does when its state blows up: NaN, then Inf, on every
        // fourth sample, so the rest of the block is still the input.
        if (p->control && *p->control > 0.5f)
            for (uint32_t i = 0; i < n; i += 4)
                p->out[0][i] = (i & 4) ? std::numeric_limits<float>::infinity()
                                       : std::numeric_limits<float>::quiet_NaN();
        break;
    case kLookahead: {
        const float g = p->control ? std::pow(10.f, *p->control / 20.f) : 1.f;
        for (uint32_t i = 0; i < n; ++i) {
            p->ring[p->write & (kLookaheadRing - 1)] = p->in[0][i];
            p->out[0][i] = g * p->ring[(p->write - kLookaheadFrames) & (kLookaheadRing - 1)];
            ++p->write;
        }
        if (p->latency) *p->latency = float(kLookaheadFrames);
        break;
    }
    case kKey:
    case kKeyLiar:
        std::memcpy(p->out[0], p->key, n * sizeof(float));
        for (uint32_t i = 0; i < n; ++i) p->key[i] = 7.0f;
        if (p->kind == kKeyLiar)
            for (uint32_t i = n; i < n + 64; ++i) p->key[i] = 7.0f;
        break;
    case kLiar:
        // Treats output 0 as eight interleaved-by-block channels. Real
        // plugins do this when their channel count is a compile-time
        // constant and the manifest says something else.
        for (int c = 0; c < 8; ++c)
            std::memcpy(p->out[0] + std::size_t(c) * n, p->in[c & 1], n * sizeof(float));
        break;
    }
}

void cleanup(LV2_Handle h) { delete static_cast<Plugin*>(h); }

const LV2_Descriptor kDescriptors[] = {
    {"urn:sonora:test#crash", instantiate<kCrash>, connect, nullptr, run, nullptr, cleanup, nullptr},
    {"urn:sonora:test#sleep", instantiate<kSleep>, connect, nullptr, run, nullptr, cleanup, nullptr},
    {"urn:sonora:test#liar", instantiate<kLiar>, connect, nullptr, run, nullptr, cleanup, nullptr},
    {"urn:sonora:test#nan", instantiate<kNan>, connect, nullptr, run, nullptr, cleanup, nullptr},
    {"urn:sonora:test#lookahead", instantiate<kLookahead>, connect, nullptr, run, nullptr, cleanup, nullptr},
    {"urn:sonora:test#key", instantiate<kKey>, connect, nullptr, run, nullptr, cleanup, nullptr},
    {"urn:sonora:test#keyliar", instantiate<kKeyLiar>, connect, nullptr, run, nullptr, cleanup, nullptr},
};

}  // namespace

extern "C" LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) {
    return index < sizeof kDescriptors / sizeof kDescriptors[0] ? &kDescriptors[index] : nullptr;
}
