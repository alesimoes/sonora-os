// SPDX-License-Identifier: GPL-3.0-or-later
//
// AlsaBackend on the fake card: what the engine's callback receives, what the
// card is asked for and given back, xrun recovery, a card that goes, one that
// is held, device choice, and the backend conformance suite. No libasound.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "fake_alsa.h"
#include "sonora/alsa_backend.h"
#include "sonora/backend_conformance.h"

namespace {

using sonora::AlsaBackend;
using sonora::alsa::FakeState;
using sonora::alsa::FakeSystem;
using sonora::backend::AudioBlock;
using sonora::backend::BackendConfig;

int failed = 0;

void expect(bool ok, const std::string& what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failed;
    }
}

template <class Pred>
bool wait_for(Pred pred, int ms = 3000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

// The engine's side: copies input k to output k, and checks what it is given.
class Passthrough final : public sonora::backend::ProcessCallback {
public:
    void process(const AudioBlock& b) noexcept override {
        if (b.nframes != nframes_.load()) sizes_.fetch_add(nframes_.load() ? 1 : 0);
        nframes_ = b.nframes;
        if (last_time_ && b.frame_time < last_time_ + last_n_) decreases_.fetch_add(1);
        if (last_time_ && b.frame_time != last_time_ + last_n_) gaps_.fetch_add(1);
        last_time_ = b.frame_time;
        last_n_ = b.nframes;
        if (b.callback_ns != 0) {
            if (b.callback_ns < last_ns_) ns_back_.fetch_add(1);
            last_ns_ = b.callback_ns;
        }
        inputs_ = b.input_count;
        for (uint32_t c = 0; c < b.input_count && c < kMax; ++c) in0_[c].store(b.inputs[c][0]);
        for (uint32_t c = 0; c < b.output_count; ++c) {
            for (uint32_t i = 0; i < b.nframes; ++i) {
                b.outputs[c][i] = c < b.input_count ? b.inputs[c][i] : 0.5f;
            }
        }
        calls_.fetch_add(1);
    }
    static constexpr uint32_t kMax = 32;
    std::atomic<uint64_t> calls_{0};
    std::atomic<uint32_t> nframes_{0}, sizes_{0}, decreases_{0}, gaps_{0}, ns_back_{0};
    std::atomic<uint32_t> inputs_{0};
    std::atomic<float> in0_[kMax];
    Passthrough() {
        for (auto& v : in0_) v = 0.0f;
    }
    uint64_t last_time_ = 0, last_ns_ = 0;
    uint32_t last_n_ = 0;
};

std::shared_ptr<FakeState> umc_state() {
    auto s = std::make_shared<FakeState>();
    s->cards = {sonora::alsa::fake_umc1820()};
    return s;
}

AlsaBackend::Options options() {
    AlsaBackend::Options o;
    o.rt_priority = 0;  // the test machine may not grant it; checked on its own
    return o;
}

std::unique_ptr<AlsaBackend> make(const std::shared_ptr<FakeState>& s,
                                  AlsaBackend::Options o = options()) {
    return std::make_unique<AlsaBackend>(o, std::make_unique<FakeSystem>(s));
}

BackendConfig config(uint32_t in = 18, uint32_t out = 20, const std::string& device = "") {
    return {"sonora-test", in, out, 48000, device};
}

void test_data_path_and_negotiation() {
    auto s = umc_state();
    Passthrough cb;
    auto b = make(s);
    std::string err;
    expect(b->open(config(), cb, err), "open: " + err);
    expect(b->activate(err), "activate: " + err);
    expect(b->capture_device() == "UMC1820", "the only USB card is chosen by default");
    expect(wait_for([&] { return cb.calls_ >= 20; }), "periods reach process()");
    expect(s->last_request.in_channels == 18 && s->last_request.out_channels == 20,
           "the card is opened with all 18 in and 20 out channels");
    expect(s->last_request.period_frames == 128 && s->last_request.periods == 2,
           "the default period is 128 frames, 2 periods");
    expect(s->last_request.rate == 48000, "the session's rate is asked for");
    expect(b->buffer_frames() == 128, "buffer_frames is the card's period");
    expect(b->ring_frames() == 256, "the ring is two periods");
    expect(b->reported_latency() == 128 + 256, "reported latency is period plus ring");
    expect(cb.inputs_ == 18, "the callback gets 18 inputs");
    bool order = true;
    for (uint32_t c = 0; c < 18; ++c) {
        order = order && std::fabs(cb.in0_[c].load() -sonora::alsa::fake_tone(c)) < 2e-6f;
    }
    expect(order, "input k carries card channel k, in order (channel 10 is not on track 3)");
    expect(b->connect_inputs_in_order() == 18, "18 inputs connected");
    expect(b->input_connected(17) && !b->input_connected(18), "input_connected follows the card");
    expect(cb.decreases_ == 0 && cb.ns_back_ == 0, "frame_time and callback_ns never decrease");
    expect(cb.gaps_ == 0, "frame_time is contiguous with no xrun");
    expect(cb.sizes_ == 0, "every block is the same size");
    b->deactivate();
    // What was played: the engine echoed input k to output k, silence past 18.
    std::lock_guard<std::mutex> lock(s->mutex);
    const auto& pb = s->last_playback;
    expect(pb.size() == 20u * 128u * 3u, "playback is 20 channels of 128 frames");
    bool echo = !pb.empty();
    for (uint32_t c = 0; c < 20 && echo; ++c) {
        float* dst[1];
        float v = 0;
        dst[0] = &v;
        sonora::alsa::deinterleave(sonora::alsa::SampleFormat::S24_3, pb.data() + c * 3, 20, dst,
                                   1, 1);
        const float want = c < 18 ? sonora::alsa::fake_tone(c) : 0.5f;
        if (std::fabs(v - want) > 2e-6f) echo = false;
    }
    expect(echo, "output k reaches card channel k");
}

void test_period_is_configurable() {
    auto s = umc_state();
    Passthrough cb;
    AlsaBackend::Options o = options();
    o.period_frames = 64;
    o.periods = 3;
    auto b = make(s, o);
    std::string err;
    expect(b->open(config(), cb, err) && b->activate(err), "start at 64 x 3: " + err);
    expect(wait_for([&] { return cb.calls_ >= 5; }), "runs");
    expect(s->last_request.period_frames == 64 && s->last_request.periods == 3, "asked for 64 x 3");
    expect(b->buffer_frames() == 64 && b->ring_frames() == 192, "granted 64 x 3");
}

// The card grants a different period than asked: the engine's block is what
// was granted, and it is said.
void test_granted_period_wins() {
    auto s = umc_state();
    s->granted_period = 192;
    Passthrough cb;
    auto b = make(s);
    std::string err;
    expect(b->open(config(), cb, err) && b->activate(err), "start: " + err);
    expect(wait_for([&] { return cb.calls_ >= 5; }), "runs");
    expect(b->buffer_frames() == 192 && cb.nframes_ == 192, "blocks are the granted 192 frames");
}

void test_fewer_and_more_tracks() {
    {
        auto s = umc_state();
        Passthrough cb;
        auto b = make(s);
        std::string err;
        expect(b->open(config(8, 2), cb, err) && b->activate(err), "8 tracks: " + err);
        expect(wait_for([&] { return cb.calls_ >= 5; }), "runs");
        expect(b->connect_inputs_in_order() == 8 && b->connected_input_count() == 8,
               "8 of 18 channels are connected");
        expect(s->last_request.in_channels == 18, "the card is still opened whole");
    }
    {
        auto s = umc_state();
        Passthrough cb;
        auto b = make(s);
        std::string err;
        expect(b->open(config(20, 2), cb, err) && b->activate(err), "20 tracks: " + err);
        expect(wait_for([&] { return cb.calls_ >= 5; }), "runs");
        expect(b->connect_inputs_in_order() == 18, "18 connected of 20 tracks");
        expect(!b->input_connected(18) && !b->input_connected(19), "tracks 19 and 20 are silent");
        expect(cb.in0_[19].load() == 0.0f, "an input past the card reads silence, not stale memory");
    }
}

void test_xrun_is_counted_never_blocks() {
    auto s = umc_state();
    Passthrough cb;
    auto b = make(s);
    std::string err;
    expect(b->open(config(), cb, err) && b->activate(err), "start: " + err);
    expect(wait_for([&] { return cb.calls_ >= 10; }), "runs");
    const uint64_t before = cb.calls_;
    s->xruns_to_throw = 3;
    expect(wait_for([&] { return b->xruns() == 3; }), "three xruns are counted");
    expect(s->recovers == 3, "each was recovered");
    expect(wait_for([&] { return cb.calls_ >= before + 10; }), "periods carry on after recovery");
    expect(!b->fatal(), "an xrun is not fatal");
    expect(cb.decreases_ == 0, "frame_time did not go back");
    expect(cb.gaps_ >= 1, "frame_time shows the lost frames");
}

void test_recovery_failure_is_the_device_going() {
    auto s = umc_state();
    Passthrough cb;
    auto b = make(s);
    std::string err;
    expect(b->open(config(), cb, err) && b->activate(err), "start: " + err);
    expect(wait_for([&] { return cb.calls_ >= 5; }), "runs");
    s->recover_fails = true;
    s->xruns_to_throw = 1;
    expect(wait_for([&] { return b->fatal(); }), "a stream that cannot restart is fatal");
    expect(b->fatal_reason().find("UMC1820") != std::string::npos, "the reason names the card");
}

void test_unplug() {
    auto s = umc_state();
    Passthrough cb;
    auto b = make(s);
    std::string err;
    expect(b->open(config(), cb, err) && b->activate(err), "start: " + err);
    expect(wait_for([&] { return cb.calls_ >= 5; }), "runs");
    s->unplugged = true;
    expect(wait_for([&] { return b->fatal(); }), "unplugging is fatal");
    const std::string why = b->fatal_reason();
    expect(why.find("capture device UMC1820 went away") != std::string::npos, "reason: " + why);
    expect(b->connected_input_count() == 0, "no input is connected once the card is gone");
    const uint64_t at = cb.calls_;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    expect(cb.calls_ == at, "process() does not run after the card went");
    b->deactivate();
}

void test_stall_is_fatal_after_a_second() {
    auto s = umc_state();
    Passthrough cb;
    auto b = make(s);
    std::string err;
    expect(b->open(config(), cb, err) && b->activate(err), "start: " + err);
    expect(wait_for([&] { return cb.calls_ >= 5; }), "runs");
    s->stalled = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    expect(!b->fatal(), "600 ms without a period is not yet fatal");
    expect(wait_for([&] { return b->fatal(); }, 2500), "a second without a period is");
    expect(b->fatal_reason().find("no period for over a second") != std::string::npos ||
               b->fatal_reason().find("ran no period") != std::string::npos,
           "reason: " + b->fatal_reason());
    b->deactivate();
}

// deactivate() has to come back though the card never answers.
void test_deactivate_is_bounded() {
    auto s = umc_state();
    Passthrough cb;
    auto b = make(s);
    std::string err;
    expect(b->open(config(), cb, err) && b->activate(err), "start: " + err);
    expect(wait_for([&] { return cb.calls_ >= 5; }), "runs");
    s->stalled = true;
    const auto t0 = std::chrono::steady_clock::now();
    b->deactivate();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    expect(ms < 400, "deactivate() took " + std::to_string(ms) + " ms with a stalled card");
    const uint64_t at = cb.calls_;
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    expect(cb.calls_ == at, "no process() after deactivate()");
}

void test_held_card_says_so() {
    auto s = umc_state();
    s->open_error = "capture hw:CARD=UMC1820,DEV=0: Device or resource busy";
    Passthrough cb;
    auto b = make(s);
    std::string err;
    expect(b->open(config(), cb, err), "open does not touch the card: " + err);
    expect(!b->activate(err), "activate fails on a held card");
    expect(err.find("cannot open card UMC1820") != std::string::npos &&
               err.find("busy") != std::string::npos,
           "the error names the card and the cause: " + err);
    expect(b->connected_input_count() == 0, "nothing is connected");
}

void test_absent_and_named() {
    auto s = umc_state();
    Passthrough cb;
    auto b = make(s);
    std::string err;
    expect(b->open(config(18, 20, "Nonexistent"), cb, err), "absent device: open is fine");
    expect(b->activate(err), "absent device: activate is fine");
    expect(b->connect_inputs_in_order() == 0, "absent device: nothing connected");
    expect(!b->choose_capture_device("UMC1820"), "a named device is never replaced");
    expect(b->capture_device() == "Nonexistent", "the name is kept");

    for (const char* name : {"umc1820", "hw:UMC1820", "hw:1,0", "UMC 1820"}) {
        Passthrough cb2;
        auto b2 = make(umc_state());
        std::string e;
        expect(b2->open(config(18, 20, name), cb2, e) && b2->activate(e), std::string(name) + ": " + e);
        expect(b2->capture_device() == "UMC1820", std::string(name) + " resolves to the card id");
    }
}

void test_default_policy() {
    {
        auto s = std::make_shared<FakeState>();
        s->cards = {sonora::alsa::fake_onboard(), sonora::alsa::fake_umc1820()};
        Passthrough cb;
        auto b = make(s);
        std::string err;
        expect(b->open(config(), cb, err) && b->activate(err), "laptop + UMC: " + err);
        expect(b->capture_device() == "UMC1820", "USB wins over onboard");
        expect(!b->choose_capture_device("PCH"), "the choice does not move");
    }
    {
        auto s = std::make_shared<FakeState>();
        s->cards = {sonora::alsa::fake_umc1820(), sonora::alsa::fake_umc1820()};
        s->cards[1].id = "UMC1820_1";
        s->cards[1].index = 2;
        Passthrough cb;
        auto b = make(s);
        std::string err;
        expect(!b->open(config(), cb, err), "two interfaces and no name is refused");
        expect(err.find("UMC1820") != std::string::npos && err.find("UMC1820_1") != std::string::npos,
               "the refusal names both: " + err);
    }
    {
        auto s = std::make_shared<FakeState>();
        s->cards = {sonora::alsa::fake_onboard()};
        Passthrough cb;
        auto b = make(s);
        std::string err;
        expect(b->open(config(2, 2), cb, err) && b->activate(err), "onboard only: " + err);
        expect(b->connect_inputs_in_order() == 0, "no card is chosen for the caller");
        expect(s->opens == 0, "and none was opened");
        expect(b->choose_capture_device("PCH") && !b->choose_capture_device("PCH"),
               "the fallback chooses once");
        expect(b->connect_inputs_in_order() == 2, "then connecting opens it");
        expect(wait_for([&] { return cb.calls_ >= 5; }), "and periods run");
    }
}

// A card that appears after activate(): the periods start at the first
// connect, which is the caller's polling loop.
void test_card_appears_late() {
    auto s = std::make_shared<FakeState>();
    Passthrough cb;
    auto b = make(s);
    std::string err;
    expect(b->open(config(), cb, err) && b->activate(err), "start with no card: " + err);
    expect(b->connect_inputs_in_order() == 0 && cb.calls_ == 0, "nothing runs without a card");
    expect(!b->fatal(), "waiting for a card is not a stall");
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        s->cards = {sonora::alsa::fake_umc1820()};
    }
    expect(b->connect_inputs_in_order() == 18, "the card is found and connected");
    expect(wait_for([&] { return cb.calls_ >= 5; }), "periods run");
    expect(b->capture_device() == "UMC1820", "and the card is remembered");
}

void test_rate_refused_at_open() {
    auto s = umc_state();
    Passthrough cb;
    auto b = make(s);
    std::string err;
    BackendConfig c = config();
    c.required_rate = 192000;
    expect(!b->open(c, cb, err), "192 kHz is refused");
    expect(err.find("192000") != std::string::npos && err.find("96000") != std::string::npos,
           "and the card's rates are listed: " + err);
    expect(b->open(config(), cb, err), "a refused open leaves the backend usable: " + err);
}

void test_rt_priority_is_reported() {
    auto s = umc_state();
    Passthrough cb;
    AlsaBackend::Options o;
    o.rt_priority = 20;
    auto b = make(s, o);
    std::string err;
    expect(b->open(config(), cb, err) && b->activate(err), "start: " + err);
    expect(wait_for([&] { return cb.calls_ >= 5; }), "runs");
    const int got = b->rt_priority_granted();
    expect(got == 0 || got == 20, "granted priority is what was asked or 0");
    std::printf("note: SCHED_FIFO 20 %s on this machine\n", got ? "granted" : "refused (ok in a container)");
    expect(b->fatal() == false, "a refused priority does not stop the stream");
}

void test_reactivation() {
    auto s = umc_state();
    Passthrough cb;
    auto b = make(s);
    std::string err;
    expect(b->open(config(), cb, err) && b->activate(err), "start: " + err);
    expect(wait_for([&] { return cb.calls_ >= 5; }), "runs");
    b->deactivate();
    const uint64_t at = cb.calls_;
    expect(b->activate(err), "activate again: " + err);
    expect(wait_for([&] { return cb.calls_ >= at + 5; }), "runs again");
    expect(s->opens == 2, "the card was reopened");
}

// ---- the backend conformance suite -------------------------------------

class AlsaRig final : public sonora::conformance::Rig {
public:
    explicit AlsaRig(std::vector<sonora::alsa::CardInfo> cards) : cards_(std::move(cards)) {}

    std::unique_ptr<sonora::backend::AudioBackend> make() override {
        auto s = std::make_shared<FakeState>();
        s->cards = cards_;
        auto b = ::make(s);
        states_[b.get()] = s;
        return b;
    }
    uint32_t rate() override { return 48000; }
    void pass(sonora::backend::AudioBackend&, uint32_t periods) override {
        std::this_thread::sleep_for(std::chrono::microseconds(uint64_t{periods} * 128 * 1000000 / 48000 + 1000));
    }
    bool exact() const override { return false; }
    Injected inject(sonora::backend::AudioBackend& b, sonora::conformance::Fault f) override {
        FakeState& s = *states_.at(&b);
        switch (f) {
            case sonora::conformance::Fault::Xrun: s.xruns_to_throw = 1; return Injected::Caused;
            case sonora::conformance::Fault::Stall: s.stalled = true; return Injected::Caused;
            case sonora::conformance::Fault::Unplug: s.unplugged = true; return Injected::Caused;
            // A card's clock is its own: it has no server to step, change
            // its quantum or rate under the stream, or vanish.
            default: return Injected::Unsupported;
        }
    }
    void restore(sonora::backend::AudioBackend& b, sonora::conformance::Fault f) override {
        if (f == sonora::conformance::Fault::Stall) states_.at(&b)->stalled = false;
    }
    std::string unplug_device() override { return "UMC1820"; }

private:
    std::vector<sonora::alsa::CardInfo> cards_;
    std::map<sonora::backend::AudioBackend*, std::shared_ptr<FakeState>> states_;
};

int run_conformance(const std::string& name, std::vector<sonora::alsa::CardInfo> cards) {
    AlsaRig rig(std::move(cards));
    return sonora::conformance::run(rig, name, stdout).failed;
}

}  // namespace

int main() {
    test_data_path_and_negotiation();
    test_period_is_configurable();
    test_granted_period_wins();
    test_fewer_and_more_tracks();
    test_xrun_is_counted_never_blocks();
    test_recovery_failure_is_the_device_going();
    test_unplug();
    test_stall_is_fatal_after_a_second();
    test_deactivate_is_bounded();
    test_held_card_says_so();
    test_absent_and_named();
    test_default_policy();
    test_card_appears_late();
    test_rate_refused_at_open();
    test_rt_priority_is_reported();
    test_reactivation();

    failed += run_conformance("alsa fake umc1820", {sonora::alsa::fake_umc1820()});
    failed += run_conformance("alsa fake onboard+umc1820",
                              {sonora::alsa::fake_onboard(), sonora::alsa::fake_umc1820()});
    auto second = sonora::alsa::fake_umc1820();
    second.id = "UMC1820_1";
    second.index = 2;
    failed += run_conformance("alsa fake two interfaces", {sonora::alsa::fake_umc1820(), second});

    if (failed == 0) std::printf("alsa_backend: all passed\n");
    return failed == 0 ? 0 : 1;
}
