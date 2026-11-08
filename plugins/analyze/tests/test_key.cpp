// SPDX-License-Identifier: GPL-3.0-or-later
//
// The classical key pass (ADR-0066): synthesised triad progressions in all 24
// keys, at A = 440 and detuned by +-30 cents; noise and silence; the sample
// rate; and the command's exit codes (0, 1, 130) with no report on cancel.
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

#include "commands.h"
#include "key/classical_key_detector.h"
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

struct Chord {
    int degree;  // semitones above the tonic
    bool minor;
};

// A tone with four harmonics at 1/h: enough partials that the third harmonic
// lands on the fifth of the scale, as a real instrument's does.
void add_note(std::vector<float>& x, double rate, double hz, double amp, std::size_t from, std::size_t n) {
    const double fade = 0.02 * rate;
    for (int h = 1; h <= 4; ++h) {
        const double w = 2.0 * kPi * hz * h / rate;
        for (std::size_t i = 0; i < n; ++i) {
            const double env = std::min({1.0, static_cast<double>(i) / fade, static_cast<double>(n - i) / fade});
            x[from + i] += static_cast<float>(amp / h * env * std::sin(w * static_cast<double>(i)));
        }
    }
}

// I-IV-I-V-I (i-iv-i-V-i in minor, with the major dominant), 2 s a chord,
// each with its root an octave down.
std::vector<float> progression(int tonic, bool minor, double cents, double rate) {
    const std::vector<Chord> chords = {{0, minor}, {5, minor}, {0, minor}, {7, false}, {0, minor}};
    const std::size_t per = static_cast<std::size_t>(2.0 * rate);
    std::vector<float> x(per * chords.size(), 0.0f);
    auto hz = [&](int midi) { return 440.0 * std::pow(2.0, (midi - 69) / 12.0 + cents / 1200.0); };
    for (std::size_t c = 0; c < chords.size(); ++c) {
        const int root = 60 + tonic + chords[c].degree;
        add_note(x, rate, hz(root - 12), 0.1, c * per, per);
        add_note(x, rate, hz(root), 0.08, c * per, per);
        add_note(x, rate, hz(root + (chords[c].minor ? 3 : 4)), 0.08, c * per, per);
        add_note(x, rate, hz(root + 7), 0.08, c * per, per);
    }
    return x;
}

bool detect(const std::vector<float>& x, double rate, KeyResult& r, std::string& err, double* tuning = nullptr,
            const std::atomic<bool>* cancel = nullptr) {
    const std::vector<std::vector<float>> planar{x};
    MemorySource src(planar, rate);
    RunOptions run;
    run.cancel = cancel;
    ClassicalKeyDetector d;
    const bool ok = d.detect(src, run, r, err);
    if (tuning) *tuning = d.tuning_cents();
    return ok;
}

void test_all_keys() {
    for (const double cents : {0.0, 30.0, -30.0}) {
        int wrong = 0;
        for (int t = 0; t < 12; ++t) {
            for (const bool minor : {false, true}) {
                KeyResult r;
                std::string err;
                double tuning = 0.0;
                const bool ok = detect(progression(t, minor, cents, 44100.0), 44100.0, r, err, &tuning);
                const bool right = ok && r.tonic == t && (r.mode == KeyMode::minor) == minor;
                if (!right) {
                    ++wrong;
                    std::fprintf(stderr, "wrong: %s %s at %+.0f cents -> %s %s (tuning %.1f) %s\n", tonic_name(t),
                                 minor ? "minor" : "major", cents, ok ? tonic_name(r.tonic) : "?",
                                 key_mode_name(r.mode), tuning, err.c_str());
                }
                CHECK(ok && r.method == KeyMethod::krumhansl && key_result_valid(r));
                CHECK(std::fabs(tuning - cents) < 6.0);
                CHECK(ok && r.confidence > 0.1);
            }
        }
        CHECK(wrong == 0);
    }
}

void test_sample_rates() {
    for (const double rate : {48000.0, 96000.0, 22050.0}) {
        for (const int t : {2, 9}) {
            KeyResult r;
            std::string err;
            CHECK(detect(progression(t, t == 9, 0.0, rate), rate, r, err));
            CHECK(r.tonic == t && (r.mode == KeyMode::minor) == (t == 9));
        }
    }
}

void test_noise_and_silence() {
    std::uint32_t s = 12345;
    std::vector<float> noise(44100 * 30);
    for (float& v : noise) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        v = 0.2f * (static_cast<float>(s) / 2147483648.0f - 1.0f);
    }
    KeyResult r;
    std::string err;
    CHECK(detect(noise, 44100.0, r, err));
    CHECK(r.confidence < 0.2);

    CHECK(!detect(std::vector<float>(44100 * 5, 0.0f), 44100.0, r, err));
    CHECK(err.find("no signal") != std::string::npos);
    // A progression at -60 dB of its level (about -75 dBFS RMS): under the gate.
    std::vector<float> faint = progression(0, false, 0.0, 44100.0);
    for (float& v : faint) v *= 0.001f;
    CHECK(!detect(faint, 44100.0, r, err));
    CHECK(!detect(std::vector<float>(1000, 0.1f), 44100.0, r, err));  // shorter than a frame
    CHECK(!detect(std::vector<float>(), 44100.0, r, err));
}

void test_cancel() {
    const std::atomic<bool> stop{true};
    KeyResult r;
    std::string err;
    CHECK(!detect(progression(0, false, 0.0, 44100.0), 44100.0, r, err, nullptr, &stop));
    CHECK(err == "cancelled");
}

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool write_wav(const fs::path& p, const std::vector<float>& x, double rate) {
    fs::remove(p);
    WavWriter w;
    if (!w.open(p.string(), static_cast<std::uint32_t>(rate), 1, WavWriter::Format::Float32, 0)) return false;
    const bool ok = w.write(x.data(), x.size());
    w.close();
    return ok;
}

void test_command() {
    const fs::path dir = fs::temp_directory_path() / "sonora_test_analyze_key";
    fs::create_directories(dir);
    const fs::path wav = dir / "g_minor.wav", silent = dir / "silent.wav", out = dir / "report.json";
    CHECK(write_wav(wav, progression(7, true, 0.0, 44100.0), 44100.0));
    CHECK(write_wav(silent, std::vector<float>(44100 * 3, 0.0f), 44100.0));

    const std::atomic<bool> go{false}, stop{true};
    fs::remove(out);
    CHECK(app::run_key_command({wav.string(), "--out", out.string()}, stop) == 130);
    CHECK(!fs::exists(out));

    CHECK(app::run_key_command({wav.string(), "--out", out.string()}, go) == 0);
    const std::string text = slurp(out);
    CHECK(text.find("\"pass\"") != std::string::npos && text.find("\"krumhansl\"") != std::string::npos);
    CHECK(text.find("\"tonic_name\"") != std::string::npos && text.find("\"G\"") != std::string::npos);
    CHECK(text.find("\"minor\"") != std::string::npos && text.find("\"confidence\"") != std::string::npos);
    CHECK(text.find("\"duration_s\"") != std::string::npos);

    fs::remove(out);
    CHECK(app::run_key_command({silent.string(), "--out", out.string()}, go) == 1);
    CHECK(!fs::exists(out));
    CHECK(app::run_key_command({(dir / "missing.wav").string()}, go) == 1);
    CHECK(app::run_key_command({}, go) == 2);
    CHECK(app::run_key_command({wav.string(), "--bogus"}, go) == 2);
    fs::remove_all(dir);
}

}  // namespace

int main() {
    test_all_keys();
    test_sample_rates();
    test_noise_and_silence();
    test_cancel();
    test_command();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("analyze_key: ok");
    return 0;
}
