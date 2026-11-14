// SPDX-License-Identifier: GPL-3.0-or-later
//
// The classical beat pass (ADR-0114) on synthesised material: click and
// drum-like trains at 70, 120 and 174 bpm (tempo within 0.5 %, every beat
// within 20 ms of the truth), a swung pattern, a drifting one (stability
// flag), silence and noise, cancel, and the command's exit codes.
//
// Synthetic trains prove the arithmetic, not the accuracy on recorded drums
// and vocals; that figure is measured separately (see the PR).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "beat/classical_beat_detector.h"
#include "commands.h"
#include "sonora/analyze/beat_detector.h"
#include "sonora/wav_writer.h"

using namespace sonora;
using namespace sonora::analyze;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 44100.0;

struct Noise {
    std::uint32_t s = 12345u;
    float next() {
        s = s * 1664525u + 1013904223u;
        return static_cast<float>(static_cast<std::int32_t>(s)) / 2147483648.0f;
    }
};

enum class Hit { click, kick, hat };

void add_hit(std::vector<float>& x, double at, Hit kind, float amp, Noise& n) {
    const std::size_t i0 = static_cast<std::size_t>(at);
    if (kind == Hit::click) {
        if (i0 < x.size()) x[i0] += amp;
        return;
    }
    const std::size_t len = static_cast<std::size_t>((kind == Hit::kick ? 0.25 : 0.06) * kRate);
    for (std::size_t k = 0; k < len && i0 + k < x.size(); ++k) {
        const double t = static_cast<double>(k) / kRate;
        double v = 0.0;
        if (kind == Hit::kick) v = std::sin(2.0 * kPi * 60.0 * t) * std::exp(-t / 0.06) + 0.5 * n.next() * std::exp(-t / 0.01);
        else v = n.next() * std::exp(-t / 0.015);
        x[i0 + k] += amp * static_cast<float>(v);
    }
}

std::vector<double> grid(double bpm, double first, double seconds) {
    std::vector<double> t;
    for (double s = first; s < seconds - 0.3; s += 60.0 / bpm) t.push_back(s);
    return t;
}

std::vector<float> train(const std::vector<double>& times, Hit kind, double seconds) {
    std::vector<float> x(static_cast<std::size_t>(seconds * kRate), 0.0f);
    Noise n;
    for (double t : times) add_hit(x, t * kRate, kind, 0.8f, n);
    return x;
}

bool detect(const std::vector<float>& x, BeatResult& r, std::string& error, const std::atomic<bool>* cancel = nullptr) {
    const std::vector<std::vector<float>> planar{x};
    MemorySource src(planar, kRate);
    RunOptions run;
    run.cancel = cancel;
    ClassicalBeatDetector d;
    return d.detect(src, run, r, error);
}

// Every detected beat within 20 ms of a true beat, and nearly every true beat
// found: the pass may miss the first or last one, not wander.
void check_beats(const BeatResult& r, const std::vector<double>& truth, const char* what) {
    int off = 0, found = 0;
    for (std::int64_t b : r.beats) {
        double best = 1e9;
        for (double t : truth) best = std::min(best, std::fabs(static_cast<double>(b) / kRate - t));
        if (best > 0.020) ++off;
    }
    for (double t : truth) {
        for (std::int64_t b : r.beats)
            if (std::fabs(static_cast<double>(b) / kRate - t) <= 0.020) { ++found; break; }
    }
    if (off != 0 || found + 2 < static_cast<int>(truth.size())) {
        std::fprintf(stderr, "%s: %d beats off the grid, %d of %zu true beats found\n", what, off, found, truth.size());
        ++g_failures;
    }
}

void test_trains() {
    for (double bpm : {70.0, 120.0, 174.0}) {
        for (Hit kind : {Hit::click, Hit::kick}) {
            const double seconds = 24.0;
            const std::vector<double> truth = grid(bpm, 0.5, seconds);
            BeatResult r;
            std::string error;
            char what[64];
            std::snprintf(what, sizeof what, "%s %.0f bpm", kind == Hit::click ? "click" : "drum", bpm);
            if (!detect(train(truth, kind, seconds), r, error)) {
                std::fprintf(stderr, "%s: failed: %s\n", what, error.c_str());
                ++g_failures;
                continue;
            }
            if (std::fabs(r.bpm - bpm) > 0.005 * bpm) {
                std::fprintf(stderr, "%s: tempo %.3f\n", what, r.bpm);
                ++g_failures;
            }
            CHECK(r.confidence >= kBeatMinConfidence && r.stable);
            CHECK(beat_result_valid(r));
            check_beats(r, truth, what);
        }
    }
}

// Quarter-note kicks with a softer hat on the swung off-beat (2/3 of a beat).
void test_swing() {
    const double seconds = 28.0, bpm = 100.0;
    const std::vector<double> beats = grid(bpm, 0.5, seconds);
    std::vector<float> x(static_cast<std::size_t>(seconds * kRate), 0.0f);
    Noise n;
    for (double t : beats) {
        add_hit(x, t * kRate, Hit::kick, 0.8f, n);
        add_hit(x, (t + 0.6667 * 60.0 / bpm) * kRate, Hit::hat, 0.15f, n);
    }
    BeatResult r;
    std::string error;
    CHECK(detect(x, r, error));
    CHECK(std::fabs(r.bpm - bpm) < 0.01 * bpm);
    CHECK(r.stable);
    check_beats(r, beats, "swing");
}

// 90 to 120 bpm over the file: the local period moves by a third.
void test_drift() {
    const double seconds = 30.0;
    std::vector<double> times;
    double t = 0.5;
    while (t < seconds - 0.3) {
        times.push_back(t);
        const double bpm = 90.0 + 30.0 * t / seconds;
        t += 60.0 / bpm;
    }
    BeatResult r;
    std::string error;
    CHECK(detect(train(times, Hit::kick, seconds), r, error));
    CHECK(!r.stable);
    CHECK(r.beats.size() > 20);
}

void test_silence_and_noise() {
    BeatResult r;
    std::string error;
    CHECK(!detect(std::vector<float>(static_cast<std::size_t>(10 * kRate), 0.0f), r, error));
    CHECK(error.find("silent") != std::string::npos);
    CHECK(!detect(std::vector<float>(static_cast<std::size_t>(2 * kRate), 0.5f), r, error));
    CHECK(error.find("4 s") != std::string::npos);

    std::vector<float> noise(static_cast<std::size_t>(20 * kRate));
    Noise n;
    for (float& v : noise) v = 0.3f * n.next();
    if (detect(noise, r, error)) {
        CHECK(r.confidence < kBeatMinConfidence);
        CHECK(r.beats.empty());
        CHECK(!r.stable);
    }
}

void test_cancel() {
    const std::vector<double> truth = grid(120.0, 0.5, 10.0);
    const std::vector<float> x = train(truth, Hit::click, 10.0);
    BeatResult r;
    std::string error;
    const std::atomic<bool> stop{true};
    CHECK(!detect(x, r, error, &stop));
    CHECK(error == "cancelled");
}

bool write_wav(const fs::path& p, const std::vector<float>& x) {
    fs::remove(p);
    WavWriter w;
    if (!w.open(p.string(), static_cast<std::uint32_t>(kRate), 1, WavWriter::Format::Float32, 0)) return false;
    const bool ok = w.write(x.data(), x.size());
    w.close();
    return ok;
}

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

void test_command() {
    const fs::path dir = fs::temp_directory_path() / "sonora_test_analyze_beat";
    fs::create_directories(dir);
    const fs::path wav = dir / "drums.wav", silent = dir / "silent.wav", out = dir / "report.json";
    const std::vector<double> truth = grid(120.0, 0.5, 12.0);
    CHECK(write_wav(wav, train(truth, Hit::kick, 12.0)));
    CHECK(write_wav(silent, std::vector<float>(static_cast<std::size_t>(6 * kRate), 0.0f)));

    const std::atomic<bool> go{false}, stop{true};
    fs::remove(out);
    CHECK(app::run_beat_command({wav.string(), "--out", out.string()}, stop) == 130);
    CHECK(!fs::exists(out));

    CHECK(app::run_beat_command({wav.string(), "--out", out.string()}, go) == 0);
    BeatResult back;
    std::string error;
    CHECK(parse_beat_report(slurp(out), back, error));
    CHECK(std::fabs(back.bpm - 120.0) < 0.6 && back.beats.size() > 15);

    fs::remove(out);
    CHECK(app::run_beat_command({silent.string(), "--out", out.string()}, go) == 1);
    CHECK(!fs::exists(out));
    CHECK(app::run_beat_command({(dir / "missing.wav").string()}, go) == 1);
    CHECK(app::run_beat_command({}, go) == 2);
    CHECK(app::run_beat_command({wav.string(), "--bogus"}, go) == 2);
    fs::remove_all(dir);
}

}  // namespace

int main() {
    test_trains();
    test_swing();
    test_drift();
    test_silence_and_noise();
    test_cancel();
    test_command();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("ok");
    return 0;
}
