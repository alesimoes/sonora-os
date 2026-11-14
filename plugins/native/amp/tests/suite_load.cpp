// SPDX-License-Identifier: GPL-3.0-or-later
//
// The amp's load-time bars (ADR-0111 §5): a capture over the class-E budget
// is refused with a held notice that names the figure and the budget, and
// it never reaches the audio thread (the output stays exactly what it was);
// the "model" file slot; a capture whose latency differs waits for the
// next prepare(); and the hand-over under a loader thread.
//
// The refusal is shown two ways. Deterministically, with the budget the
// test injects (1 us p99.9, 2 us max: no network does a 128-sample block in
// that). And on the real budget with the largest LSTM the kernel accepts
// (4 x 64), whose figure on this machine is reported with the outcome.
#include "suite_load.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

#include "suite_models.h"
#include "sonora/native/amp.h"

namespace sonora::native::suites::amp_load {

namespace {

using measure::Chans;
using measure::Suite;
namespace at = amp_test;

constexpr amp::Budget kImpossible = {1.0, 2.0};
// For tests of loading, not of speed: a loaded CI runner (ASan, parallel ctest) must not refuse a tiny capture.
constexpr amp::Budget kRelaxed = {1.0e6, 2.0e6};

double delayed_error(const Chans& x, const Chans& y, std::size_t lag) {
    double e = 0.0;
    for (std::size_t c = 0; c < x.size(); ++c) {
        for (std::size_t n = lag; n < y[c].size(); ++n)
            e = std::max(e, static_cast<double>(std::fabs(y[c][n] - x[c][n - lag])));
    }
    return e;
}

bool names_budget(const std::string& m) {
    return m.find("class E") != std::string::npos && m.find(" us") != std::string::npos;
}

void ok(Suite& s, const std::string& what, bool pass, const std::string& note = {}) {
    s.check(what, pass ? 0.0 : 1.0, 0.0, pass, "", note);
}

std::string data(const char* file) { return std::string(SONORA_AMP_TEST_DATA) + "/" + file; }

}  // namespace

void refusal(Suite& s) {
    const double fs = 48000.0;
    // Equal channels: the amp sums a stereo input to mono, and a capture's
    // output is that sum, so this way the output is comparable to x.
    Chans x = measure::noise(fs, -6.0, 0.25, 9u);
    x[1] = x[0];
    {
        // Before prepare(): refused at once, and prepare() has nothing to play.
        auto p = make_amp();
        p->set_budget(kImpossible);
        std::string msg;
        const auto r = p->load_nam(at::lstm(1, 8), msg);
        ok(s, "over budget, before prepare: refused", r == Amp::LoadResult::Refused, msg);
        ok(s, "over budget, before prepare: the message names the figure and class E", names_budget(msg), msg);
        ok(s, "over budget, before prepare: the notice is held", p->notice() == msg);
        p->prepare(fs, 128, 2);
        ok(s, "over budget, before prepare: not loaded", !p->status().loaded);
        const Chans y = measure::render(*p, x);
        s.check("over budget, before prepare: output is the input (never loaded)", delayed_error(x, y, p->latency()),
                0.0, delayed_error(x, y, p->latency()) == 0.0, "linear");
    }
    {
        // While another capture plays: refused, and that capture keeps playing.
        auto p = make_amp();
        std::string msg;
        p->load_capture(at::identity(fs), msg);
        p->prepare(fs, 128, 2);
        p->set_budget(kImpossible);
        const auto r = p->load_nam(at::lstm(1, 8), msg);
        ok(s, "over budget, while playing: refused", r == Amp::LoadResult::Refused, msg);
        ok(s, "over budget, while playing: the notice is held", !p->notice().empty() && names_budget(p->notice()));
        const Chans y = measure::render(*p, x);
        s.check("over budget, while playing: the playing capture is untouched", delayed_error(x, y, 0), 0.0,
                delayed_error(x, y, 0) == 0.0, "linear");
        p->set_budget(amp::Budget{});
        p->clear_capture();
        ok(s, "a clear removes the notice", p->notice().empty());
    }
    {
        // The real budget, the largest LSTM the kernel takes.
        auto p = make_amp();
        p->prepare(fs, 128, 2);
        std::string msg;
        const auto r = p->load_nam(at::lstm(4, 64), msg);
        const amp::Status st = p->status();
        if (r == Amp::LoadResult::Refused) {
            ok(s, "4 x 64 LSTM, class E: refused here, with the figure", names_budget(msg) && !st.loaded, msg);
        } else {
            ok(s, "4 x 64 LSTM, class E: loaded here, within the budget",
               r == Amp::LoadResult::Ok && st.p999_us <= 250.0 && st.max_us <= 500.0);
            s.info("4 x 64 LSTM: p99.9 per q128 block at load", st.p999_us, "us");
        }
    }
}

void file_slot(Suite& s) {
    auto p = make_amp();
    p->set_budget(kRelaxed);
    FileSlots* f = p->files();
    ok(s, "the amp has one file slot, \"model\", for .nam", f != nullptr && f->slot_count() == 1 &&
       std::string(f->slot(0).id) == "model" && std::string(f->slot(0).extensions) == "nam");
    if (f == nullptr) return;
    std::string err;
    const bool lstm = f->load(0, data("lstm_1x16.nam"), err);
    ok(s, "file slot: an LSTM .nam loads", lstm && f->path(0) == data("lstm_1x16.nam"), err);
    p->prepare(48000.0, 128, 2);
    const amp::Status st = p->status();
    ok(s, "file slot: loaded, normalised from the file's loudness",
       st.loaded && st.has_loudness && std::fabs(st.trim_db) < 1e-9, std::to_string(st.trim_db) + " dB");
    err.clear();
    const bool wavenet = f->load(0, data("tiny.nam"), err);
    ok(s, "file slot: a WaveNet .nam loads while prepared", wavenet && f->path(0) == data("tiny.nam"), err);
    // Let the audio thread take it over.
    measure::render(*p, measure::silence(48000.0, 0.05));
    err.clear();
    const bool missing = f->load(0, data("no-such-capture.nam"), err);
    ok(s, "file slot: a missing file fails with a reason, the slot unchanged",
       !missing && !err.empty() && f->path(0) == data("tiny.nam"), err);
    p->set_budget(kImpossible);
    err.clear();
    const bool over = f->load(0, data("lstm_2x8.nam"), err);
    ok(s, "file slot: a capture over budget fails with the notice, the slot unchanged",
       !over && names_budget(err) && f->path(0) == data("tiny.nam"), err);
    p->set_budget(kRelaxed);
    measure::render(*p, measure::silence(48000.0, 0.05));
    err.clear();
    const bool cleared = f->load(0, "", err);
    ok(s, "file slot: an empty path clears it", cleared && f->path(0).empty() && !p->status().loaded, err);
}

void deferred(Suite& s) {
    const double fs = 44100.0;
    auto p = make_amp();
    p->prepare(fs, 128, 2);
    const std::uint32_t before = p->latency();
    std::string msg;
    const auto r = p->load_capture(at::identity(fs), msg);
    ok(s, "a 44.1 kHz capture in a 44.1 kHz session prepared for 48 kHz: Deferred, and said why",
       r == Amp::LoadResult::Deferred && msg.find("restarts") != std::string::npos, msg);
    Chans x = measure::noise(fs, -6.0, 0.25, 3u);
    x[1] = x[0];  // see refusal()
    const Chans y = measure::render(*p, x);
    s.check("deferred: the latency stays until prepare()", p->latency(), before, p->latency() == before, "samples");
    s.check("deferred: the old state plays on (the input, delayed)", delayed_error(x, y, before), 0.0,
            delayed_error(x, y, before) == 0.0, "linear");
    p->prepare(fs, 128, 2);
    const Chans y2 = measure::render(*p, x);
    s.check("deferred: after prepare() the capture plays at latency 0", p->latency(), 0.0,
            p->latency() == 0 && delayed_error(x, y2, 0) == 0.0, "samples");
}

void concurrent_loads(Suite& s) {
    const double fs = 48000.0;
    auto p = make_amp();
    std::string msg;
    p->load_capture(at::identity(fs), msg);
    p->prepare(fs, 128, 2);
    const int wanted = measure::quick() ? 6 : 30;
    std::atomic<int> loaded{0};
    std::atomic<bool> done{false};
    std::thread loader([&] {
        std::string m;
        const auto t0 = std::chrono::steady_clock::now();
        while (loaded < wanted && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(40)) {
            const auto r = loaded % 2 == 0 ? p->load_nam(at::lstm(1, 8), m) : p->load_capture(at::identity(fs), m);
            if (r == Amp::LoadResult::Ok) ++loaded;
            else std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
        done = true;
    });
    const Chans x = measure::noise(fs, -6.0, 0.5, 11u);
    bool finite = true;
    double peak = 0.0;
    while (!done) {
        const Chans y = measure::render(*p, x);
        for (const auto& ch : y) {
            peak = std::max(peak, measure::peak(ch));
            for (float v : ch) finite = finite && std::isfinite(v);
        }
    }
    loader.join();
    s.check("loads under a running audio thread all land", loaded, wanted, loaded == wanted, "loads");
    ok(s, "output stays finite while captures are swapped", finite);
    s.info("output peak under the swaps", peak, "linear");
}

}  // namespace sonora::native::suites::amp_load
