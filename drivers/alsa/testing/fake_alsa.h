// SPDX-License-Identifier: GPL-3.0-or-later
//
// alsa::System with no libasound: cards from a list, a duplex that paces
// itself in real time, hands out a recognisable tone per channel and records
// what was played. Faults are switches the test throws. Test code; not
// installed and not linked by the engine.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sonora/alsa_convert.h"
#include "sonora/alsa_device.h"

namespace sonora::alsa {

struct FakeState {
    std::mutex mutex;
    std::vector<CardInfo> cards;
    std::string open_error;  // when set, Duplex::open() fails with it
    uint32_t granted_period = 0;  // 0: grant what was asked
    SampleFormat format = SampleFormat::S24_3;
    DuplexRequest last_request;

    std::atomic<bool> unplugged{false};
    std::atomic<bool> stalled{false};
    std::atomic<int> xruns_to_throw{0};
    std::atomic<bool> recover_fails{false};
    std::atomic<uint32_t> cycles{0};
    std::atomic<uint32_t> recovers{0};
    std::atomic<uint32_t> opens{0};
    std::atomic<bool> started{false};
    std::vector<uint8_t> last_playback;  // under mutex
    uint32_t last_playback_channels = 0;
    // Real time by default, so the thread loop is the one that ships; a
    // test of data alone could set 0.
    double pace = 1.0;
};

// Channel c of a fake capture carries this constant, so a swap or an offset
// shows up as a wrong value on a known channel.
inline float fake_tone(uint32_t channel) { return 0.01f * static_cast<float>(channel + 1); }

inline CardInfo fake_umc1820() {
    CardInfo c;
    c.index = 1;
    c.id = "UMC1820";
    c.name = "UMC 1820";
    c.usb = true;
    c.capture_channels = 18;
    c.playback_channels = 20;
    c.rates = {44100, 48000, 88200, 96000};
    c.capture_pcm = c.playback_pcm = "hw:CARD=UMC1820,DEV=0";
    return c;
}

inline CardInfo fake_onboard() {
    CardInfo c;
    c.index = 0;
    c.id = "PCH";
    c.name = "HDA Intel PCH";
    c.usb = false;
    c.capture_channels = 2;
    c.playback_channels = 2;
    c.capture_pcm = c.playback_pcm = "hw:CARD=PCH,DEV=0";
    return c;
}

class FakeDuplex final : public Duplex {
public:
    explicit FakeDuplex(std::shared_ptr<FakeState> s) : s_(std::move(s)) {}

    bool open(const DuplexRequest& req, DuplexInfo& info, std::string& error) override {
        if (!s_->open_error.empty()) {
            error = s_->open_error;
            return false;
        }
        s_->opens.fetch_add(1);
        s_->last_request = req;
        req_ = req;
        period_ = s_->granted_period ? s_->granted_period : req.period_frames;
        info.rate = req.rate;
        info.in_channels = req.in_channels;
        info.out_channels = req.out_channels;
        info.period_frames = period_;
        info.buffer_frames = period_ * req.periods;
        info.in_format = info.out_format = s_->format;
        info.linked = true;
        in_.assign(static_cast<std::size_t>(req.in_channels) * period_ * 4, 0);
        out_.assign(static_cast<std::size_t>(req.out_channels) * period_ * 4, 0);
        // The capture is the same every period: constant per channel.
        std::vector<float*> planes(req.in_channels);
        std::vector<std::vector<float>> store(req.in_channels, std::vector<float>(period_));
        for (uint32_t c = 0; c < req.in_channels; ++c) {
            std::fill(store[c].begin(), store[c].end(), fake_tone(c));
            planes[c] = store[c].data();
        }
        interleave(s_->format, planes.data(), req.in_channels, in_.data(), req.in_channels,
                   period_);
        return true;
    }

    bool start(std::string&) override {
        s_->started = true;
        next_ = std::chrono::steady_clock::now();
        return true;
    }

    Status begin(int timeout_ms, Cycle& cy) noexcept override {
        if (s_->unplugged) return Status::Gone;
        if (s_->stalled) {
            std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
            return s_->unplugged ? Status::Gone : Status::Timeout;
        }
        if (s_->xruns_to_throw.load() > 0) {
            s_->xruns_to_throw.fetch_sub(1);
            return Status::Xrun;
        }
        const auto period_ns = std::chrono::nanoseconds(static_cast<int64_t>(
            s_->pace * 1e9 * period_ / req_.rate));
        next_ += period_ns;
        std::this_thread::sleep_until(next_);
        cy.capture = in_.data();
        cy.playback = out_.data();
        cy.frames = period_;
        cy.stamp_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                std::chrono::steady_clock::now().time_since_epoch())
                                                .count());
        return Status::Ready;
    }

    Status commit() noexcept override {
        s_->cycles.fetch_add(1);
        std::lock_guard<std::mutex> lock(s_->mutex);
        s_->last_playback.assign(out_.begin(), out_.begin() + static_cast<std::ptrdiff_t>(
                                     static_cast<std::size_t>(req_.out_channels) * period_ *
                                     bytes_per_sample(s_->format)));
        s_->last_playback_channels = req_.out_channels;
        return s_->unplugged ? Status::Gone : Status::Ready;
    }

    bool recover() noexcept override {
        s_->recovers.fetch_add(1);
        next_ = std::chrono::steady_clock::now();
        return !s_->recover_fails && !s_->unplugged;
    }

private:
    std::shared_ptr<FakeState> s_;
    DuplexRequest req_;
    uint32_t period_ = 0;
    std::vector<uint8_t> in_, out_;
    std::chrono::steady_clock::time_point next_;
};

class FakeSystem final : public System {
public:
    explicit FakeSystem(std::shared_ptr<FakeState> s) : s_(std::move(s)) {}
    std::vector<CardInfo> cards() override {
        std::lock_guard<std::mutex> lock(s_->mutex);
        if (s_->unplugged) {
            std::vector<CardInfo> rest;
            for (const CardInfo& c : s_->cards) {
                if (!c.usb) rest.push_back(c);
            }
            return rest;
        }
        return s_->cards;
    }
    std::unique_ptr<Duplex> make_duplex() override { return std::make_unique<FakeDuplex>(s_); }

private:
    std::shared_ptr<FakeState> s_;
};

}  // namespace sonora::alsa
