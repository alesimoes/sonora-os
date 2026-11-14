// SPDX-License-Identifier: GPL-3.0-or-later
//
// sonora-analyze vocal-balance (ADR-0104): a synthetic vocal with a known
// resonance gives a band near it and a plan step; a silent file exits 1 with
// the reason and writes no report; stereo is summed; usage exits 2. The
// generator is copied from plugins/native/vocal_balance/tests/test_analysis.cpp
// (reduced: no fricatives), not included.
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "commands.h"
#include "sonora/json/json.h"
#include "sonora/wav_writer.h"

namespace fs = std::filesystem;
using sonora::json::Value;

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
constexpr double kFs = 48000.0;
constexpr double kResHz = 3712.0;

struct Lcg {
    std::uint32_t s;
    std::uint32_t next() { return s = s * 1664525u + 1013904223u; }
    double uniform() { return (next() >> 8) * (1.0 / 8388608.0) - 1.0; }
};

// 12 s of notes of 0.4-0.7 s, 0.15 s apart, with a stationary resonance
// riding on the voice's envelope and a -70 dBFS noise floor.
std::vector<float> make_voice(double res_hz, double res_dbfs) {
    const std::size_t n = static_cast<std::size_t>(12.0 * kFs);
    std::vector<double> y(n, 0.0), env(n, 0.0);
    Lcg rng{3};
    std::size_t pos = static_cast<std::size_t>(0.1 * kFs);
    while (pos + static_cast<std::size_t>(0.8 * kFs) < n) {
        const std::size_t len = static_cast<std::size_t>((0.4 + 0.3 * (0.5 + 0.5 * rng.uniform())) * kFs);
        const double f0 = 115.0 + 140.0 * (0.5 + 0.5 * rng.uniform());
        const double vib_ph = 2.0 * kPi * (0.5 + 0.5 * rng.uniform());
        std::vector<double> phase(160, 0.0);
        for (std::size_t i = 0; i < len; ++i) {
            const double t = static_cast<double>(i) / kFs;
            const double f = f0 * (1.0 + 0.03 * std::sin(2.0 * kPi * 5.5 * t + vib_ph) + 0.05 * (t / 0.5 - 1.0));
            double a = 1.0;
            if (i < 960) a = static_cast<double>(i) / 960.0;
            if (len - i < 1920) a = static_cast<double>(len - i) / 1920.0;
            env[pos + i] = a;
            double s = 0.0;
            for (int h = 1; h * f < 14000.0 && h < 160; ++h) {
                phase[h] += 2.0 * kPi * h * f / kFs;
                const double hf = h * f / 7000.0;
                s += std::sin(phase[h]) * 0.12 / std::pow(static_cast<double>(h), 1.2) / (1.0 + hf * hf * hf * hf);
            }
            y[pos + i] = s * a;
        }
        pos += len + static_cast<std::size_t>(0.15 * kFs);
    }
    const double amp = std::pow(10.0, res_dbfs / 20.0);
    for (std::size_t i = 0; i < n; ++i) {
        y[i] += amp * env[i] * std::sin(2.0 * kPi * res_hz * static_cast<double>(i) / kFs) + 3e-4 * rng.uniform();
    }
    std::vector<float> out(n);
    for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<float>(y[i]);
    return out;
}

bool write_wav(const fs::path& p, const std::vector<float>& interleaved, std::uint16_t channels) {
    fs::remove(p);
    sonora::WavWriter w;
    if (!w.open(p.string(), static_cast<std::uint32_t>(kFs), channels, sonora::WavWriter::Format::Float32, 0)) return false;
    return w.write(interleaved.data(), interleaved.size() / channels) && w.close();
}

struct Run {
    int rc = -1;
    Value report;
    bool has_report = false;
};

Run run(const fs::path& wav, const fs::path& out, std::vector<std::string> extra = {}) {
    std::vector<std::string> args{wav.string()};
    if (!out.empty()) {
        fs::remove(out);
        args.push_back("--out");
        args.push_back(out.string());
    }
    for (auto& e : extra) args.push_back(e);
    const std::atomic<bool> cancel{false};
    Run r;
    r.rc = sonora::analyze::app::run_vocal_balance_command(args, cancel);
    if (!out.empty() && fs::exists(out)) {
        std::ifstream f(out, std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        sonora::json::Error err;
        r.has_report = sonora::json::parse(ss.str(), r.report, err);
    }
    return r;
}

double number(const Value* v) { return v ? std::atof(v->text().c_str()) : 0.0; }

bool band_near(const Run& r, double hz) {
    const Value* m = r.report.find("measurements");
    const Value* res = m ? m->find("resonances") : nullptr;
    if (!res) return false;
    for (const Value& b : res->items()) {
        if (std::fabs(number(b.find("freq_hz")) - hz) < 40.0 && number(b.find("excess_db")) >= 8.0) return true;
    }
    return false;
}

bool plan_has_band_near(const Run& r, double hz) {
    const Value* steps = r.report.find("plan") ? r.report.find("plan")->find("steps") : nullptr;
    if (!steps) return false;
    for (const Value& s : steps->items()) {
        const Value* plugin = s.find("plugin");
        const Value* params = s.find("params");
        if (!plugin || plugin->text() != "Sonora Vocal Balance" || !params) continue;
        for (int i = 1; i <= 5; ++i) {
            if (std::fabs(number(params->find("freq" + std::to_string(i))) - hz) < 40.0 &&
                number(params->find("cut" + std::to_string(i))) >= 4.0) return true;
        }
    }
    return false;
}

void test_resonance_and_plan(const fs::path& dir) {
    const std::vector<float> x = make_voice(kResHz, -30.0);
    CHECK(write_wav(dir / "voice.wav", x, 1));
    const Run r = run(dir / "voice.wav", dir / "voice.json");
    CHECK(r.rc == 0 && r.has_report);
    CHECK(r.report.find("pass") && r.report.find("pass")->text() == "vocal-balance");
    CHECK(band_near(r, kResHz));
    CHECK(plan_has_band_near(r, kResHz));
    const Value* steps = r.report.find("plan") ? r.report.find("plan")->find("steps") : nullptr;
    CHECK(steps && !steps->items().empty());
    if (steps) {
        for (const Value& s : steps->items()) CHECK(s.find("title") && s.find("reason") && !s.find("reason")->text().empty());
    }
}

void test_stereo_is_summed(const fs::path& dir) {
    // The same voice in both channels sums to itself (mean), so the report matches the mono one.
    const std::vector<float> x = make_voice(kResHz, -30.0);
    std::vector<float> st(x.size() * 2);
    for (std::size_t i = 0; i < x.size(); ++i) st[2 * i] = st[2 * i + 1] = x[i];
    CHECK(write_wav(dir / "stereo.wav", st, 2));
    const Run r = run(dir / "stereo.wav", dir / "stereo.json");
    CHECK(r.rc == 0 && r.has_report && band_near(r, kResHz));
    // One side silent: the mean halves the level (-6 dB), and the resonance is still found.
    std::vector<float> one(x.size() * 2, 0.0f);
    for (std::size_t i = 0; i < x.size(); ++i) one[2 * i] = x[i];
    CHECK(write_wav(dir / "one.wav", one, 2));
    const Run a = run(dir / "one.wav", dir / "one.json");
    const Run b = run(dir / "stereo.wav", dir / "stereo2.json");
    CHECK(a.rc == 0 && b.rc == 0);
    const double la = number(a.report.find("measurements")->find("level_db"));
    const double lb = number(b.report.find("measurements")->find("level_db"));
    CHECK(std::fabs((lb - la) - 6.0) < 0.5);
}

void test_silent_fails(const fs::path& dir) {
    const std::vector<float> z(static_cast<std::size_t>(3 * kFs), 0.0f);
    CHECK(write_wav(dir / "silent.wav", z, 1));
    const Run r = run(dir / "silent.wav", dir / "silent.json");
    CHECK(r.rc == 1);
    CHECK(!fs::exists(dir / "silent.json"));
    const Run missing = run(dir / "nope.wav", dir / "nope.json");
    CHECK(missing.rc == 1 && !fs::exists(dir / "nope.json"));
}

void test_usage(const fs::path& dir) {
    const std::atomic<bool> cancel{false};
    CHECK(sonora::analyze::app::run_vocal_balance_command({}, cancel) == 2);
    CHECK(sonora::analyze::app::run_vocal_balance_command({(dir / "voice.wav").string(), "--bogus", "1"}, cancel) == 2);
    CHECK(sonora::analyze::app::run_vocal_balance_command({(dir / "voice.wav").string(), "--out"}, cancel) == 2);
    const std::atomic<bool> stop{true};
    CHECK(sonora::analyze::app::run_vocal_balance_command({(dir / "voice.wav").string()}, stop) == 130);
}

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "sonora_vocal_command_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    test_resonance_and_plan(dir);
    test_stereo_is_summed(dir);
    test_silent_fails(dir);
    test_usage(dir);
    fs::remove_all(dir);
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("analyze_vocal_command: ok");
    return 0;
}
