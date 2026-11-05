// SPDX-License-Identifier: GPL-3.0-or-later
//
// The seam between AlsaBackend's logic and libasound. Nothing here includes
// an ALSA header: src/alsa_system.cpp is the only file that does, so the
// backend (threading, xrun recovery, conversion, device choice) is tested
// against testing/fake_alsa.h on any machine, and the real card is exercised
// only by what needs one (tools/test-alsa-loopback.sh, the UMC1820).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sonora::alsa {

// The interleaved sample layouts a class-compliant interface offers. Which
// one the UMC1820 negotiates is to be read off the hardware (ADR-0053).
enum class SampleFormat { S16, S24_3, S24_4, S32 };

inline uint32_t bytes_per_sample(SampleFormat f) {
    switch (f) {
        case SampleFormat::S16: return 2;
        case SampleFormat::S24_3: return 3;
        case SampleFormat::S24_4:
        case SampleFormat::S32: return 4;
    }
    return 4;
}

struct CardInfo {
    int index = -1;
    std::string id;    // "UMC1820": what hw:CARD=<id> and --device name
    std::string name;  // the long human name
    bool usb = false;
    uint32_t capture_channels = 0;  // 0: no capture stream, or not known
    uint32_t playback_channels = 0;
    // Rates the card offers exactly. Empty means any or not known, which
    // open() then does not refuse on: the stream's own negotiation will.
    std::vector<uint32_t> rates;
    // The PCMs to open. Almost always hw:CARD=<id>,DEV=0 for both; they
    // differ for snd-aloop, whose two ends are different devices.
    std::string capture_pcm;
    std::string playback_pcm;
};

struct DuplexRequest {
    std::string capture_pcm;
    std::string playback_pcm;
    uint32_t rate = 48000;
    uint32_t in_channels = 0;
    uint32_t out_channels = 0;
    uint32_t period_frames = 128;
    uint32_t periods = 2;
};

struct DuplexInfo {
    uint32_t rate = 0;
    uint32_t in_channels = 0;
    uint32_t out_channels = 0;
    uint32_t period_frames = 0;  // as granted, which can differ from asked
    uint32_t buffer_frames = 0;
    SampleFormat in_format = SampleFormat::S32;
    SampleFormat out_format = SampleFormat::S32;
    bool linked = false;  // both directions start on one trigger
};

enum class Status {
    Ready,    // a whole period is captured and there is room for one to play
    Timeout,  // nothing yet; the caller checks its stop flag and asks again
    Xrun,     // an over- or underrun: recover() before the next begin()
    Gone,     // the device is not coming back
};

// One period, both directions, valid from a Ready begin() until commit().
// Interleaved, `frames` long: the region the kernel's ring exposes, so
// reading and writing it is the whole data path (mmap).
struct Cycle {
    const uint8_t* capture = nullptr;
    uint8_t* playback = nullptr;
    uint32_t frames = 0;
    // CLOCK_MONOTONIC ns at which the first frame of `capture` was taken,
    // or 0 if the driver gives no timestamp.
    uint64_t stamp_ns = 0;
};

// One card opened for capture and playback. Used from one thread at a time:
// the control thread to open, the backend's thread afterwards. begin() and
// commit() allocate nothing and wait no longer than their timeout.
class Duplex {
public:
    virtual ~Duplex() = default;
    virtual bool open(const DuplexRequest& request, DuplexInfo& info, std::string& error) = 0;
    // Prefills playback with silence and starts the stream.
    virtual bool start(std::string& error) = 0;
    virtual Status begin(int timeout_ms, Cycle& cycle) noexcept = 0;
    // Hands the period back. May itself report Xrun or Gone.
    virtual Status commit() noexcept = 0;
    // After Xrun: stop, prepare, prefill and restart. False means the device
    // is gone.
    virtual bool recover() noexcept = 0;
};

class System {
public:
    virtual ~System() = default;
    // Cards with PCM device 0, in index order. Cheap enough to ask every
    // second; never opens a stream another program holds.
    virtual std::vector<CardInfo> cards() = 0;
    virtual std::unique_ptr<Duplex> make_duplex() = 0;
};

// libasound's System; nullptr in a build without libasound.
std::unique_ptr<System> make_system();

}  // namespace sonora::alsa
