// SPDX-License-Identifier: GPL-3.0-or-later
// A WAV impulse response read with common/audiofile, resampled to the
// session rate and rendered: the file path of the cabinet.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "sonora/measure/measure.h"
#include "sonora/native/cab_file.h"
#include "sonora/native/state.h"

namespace {
int failures = 0;
// A smooth onset keeps the response band-limited, which the resampler can
// only be held to (a slope jump at t = 0 is not representable).
double burst(double t) {
    const double u = t / 0.0005;
    return (1.0 - std::exp(-u * u * u * u)) * std::exp(-t / 0.005) * std::sin(2 * 3.14159265358979 * 1000.0 * t);
}
#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); \
            ++failures;                                                   \
        }                                                                 \
    } while (0)
// A 44-byte PCM16 header that declares far more data than the file holds.
void write_hostile(const std::string& path, uint16_t ch, uint32_t rate, uint32_t data_bytes) {
    unsigned char h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0,
                           1, 0};
    auto p16 = [&](int o, uint16_t v) { h[o] = v & 255; h[o + 1] = v >> 8; };
    auto p32 = [&](int o, uint32_t v) { p16(o, v & 0xffff); p16(o + 2, static_cast<uint16_t>(v >> 16)); };
    p16(22, ch);
    p32(24, rate);
    p32(28, rate * ch * 2);
    p16(32, static_cast<uint16_t>(ch * 2));
    p16(34, 16);
    std::memcpy(h + 36, "data", 4);
    p32(40, data_bytes);
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f) {
        std::fwrite(h, 1, sizeof h, f);
        std::fclose(f);
    }
}
}  // namespace

int main() {
    using namespace sonora;
    const auto dir = std::filesystem::temp_directory_path() / "sonora_cab_file_test";
    std::filesystem::create_directories(dir);
    const std::string path = (dir / "ir.wav").string();

    // An exponentially decaying 1 kHz burst at 44.1 kHz, stereo file: channel 0 is read.
    const double ir_fs = 44100.0;
    measure::Chans ir = measure::silence(ir_fs, 0.05);
    for (std::size_t i = 0; i < ir[0].size(); ++i) {
        const double t = static_cast<double>(i) / ir_fs;
        ir[0][i] = static_cast<float>(burst(t));
        ir[1][i] = 0.0f;
    }
    CHECK(measure::write_wav_f32(path, ir, ir_fs));

    for (double fs : {44100.0, 48000.0, 96000.0}) {
        auto cab = native::make_cab();
        cab->prepare(fs, 128, 1);
        std::string err;
        CHECK(native::load_cab_ir_wav(*cab, path, err) == native::Cab::LoadResult::Ok);
        measure::Chans x = measure::silence(fs, 0.1, 1);
        x[0][0] = 1.0f;
        // The load is taken over at the next block; render a block first.
        measure::render(*cab, measure::silence(fs, 0.02, 1));
        measure::Chans y = measure::render(*cab, x);
        double worst = 0.0;
        for (std::size_t i = 0; i < y[0].size() && i < static_cast<std::size_t>(0.04 * fs); ++i) {
            const double t = static_cast<double>(i) / fs;
            worst = std::fmax(worst, std::fabs(y[0][i] - burst(t)));
        }
        std::printf("%.0f Hz: max error %.2e\n", fs, worst);
        CHECK(worst < 5e-3);
    }

    auto cab = native::make_cab();
    cab->prepare(48000.0, 128, 1);
    std::string err;
    CHECK(native::load_cab_ir_wav(*cab, (dir / "missing.wav").string(), err) == native::Cab::LoadResult::Invalid);
    CHECK(!err.empty());
    // Hostile headers must be refused without asking for the declared size.
    const std::string bad = (dir / "bad.wav").string();
    const struct { uint16_t ch; uint32_t rate; uint32_t data; } cases[] = {{65535, 48000, 0xFFFFFFFEu}, {1, 4000000000u, 0xFFFFFFFEu},
                                                            {2, 48000, 4000000}, {1, 48000, 1000000}};
    // The declared size is never allocated: a refusal is immediate (the old code
    // zero-filled GBs first, 37 s measured, or died of bad_alloc on a small host).
    const auto t0 = std::chrono::steady_clock::now();
    for (const auto& c : cases) {
        write_hostile(bad, c.ch, c.rate, c.data);
        std::string e;
        CHECK(native::load_cab_ir_wav(*cab, bad, e) == native::Cab::LoadResult::Invalid);
        CHECK(!e.empty());
    }
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3));
    // The file slot (ADR-0029 section 6, amendment): the response chosen
    // through plugin state.
    {
        CHECK(native::make_cab()->files() == nullptr);  // no reader, no slot
        const double fs = 48000.0;
        auto write_ir = [&](const std::string& p, std::size_t at, float g) {
            measure::Chans c = measure::silence(fs, 0.01, 1);
            c[0][at] = g;
            return measure::write_wav_f32(p, c, fs);
        };
        const std::string p1 = (dir / "slot1.wav").string(), p2 = (dir / "slot2.wav").string();
        CHECK(write_ir(p1, 7, 0.5f));
        CHECK(write_ir(p2, 20, 0.25f));
        // Silent blocks first: a load is taken over at a block boundary and
        // the wet path then dips and fades back.
        auto response = [&](native::Cab& c) {
            measure::render(c, measure::silence(fs, 0.05, 1));
            measure::Chans x = measure::silence(fs, 0.01, 1);
            x[0][0] = 1.0f;
            return measure::render(c, x)[0];
        };
        auto prepared = [&]() {
            auto c = native::make_cab_plugin();
            c->prepare(fs, 128, 1);
            return c;
        };

        auto a = prepared();
        native::FileSlots* slots = a->files();
        CHECK(slots != nullptr && slots->slot_count() == 1);
        CHECK(std::string(slots->slot(0).id) == "ir" && std::string(slots->slot(0).extensions) == "wav");
        CHECK(std::fabs(response(*a)[0] - 1.0f) < 1e-6f);  // nothing loaded: the unit impulse

        std::string why;
        CHECK(slots->load(0, p1, why));
        CHECK(slots->path(0) == p1);
        std::vector<float> y = response(*a);
        CHECK(std::fabs(y[7] - 0.5f) < 1e-4f && std::fabs(y[0]) < 1e-6f);

        // A second load before the audio thread took the first over cannot
        // be made: it fails after the bounded wait and the path is unchanged.
        why.clear();
        const auto w0 = std::chrono::steady_clock::now();
        CHECK(slots->load(0, p2, why) == true);  // the first was taken over by response()
        CHECK(!slots->load(0, p1, why));         // no block ran since
        CHECK(!why.empty());
        CHECK(std::chrono::steady_clock::now() - w0 < std::chrono::seconds(3));
        CHECK(slots->path(0) == p2);
        y = response(*a);
        CHECK(std::fabs(y[20] - 0.25f) < 1e-4f && std::fabs(y[7]) < 1e-6f);

        // The state carries the path; a new cab restored from it sounds the same.
        const std::string s = native::save_state(*a);
        CHECK(s.find("slot2.wav") != std::string::npos);
        auto b = prepared();
        CHECK(native::load_state(*b, s, why));
        CHECK(b->files()->path(0) == p2);
        const std::vector<float> yb = response(*b);
        CHECK(yb == y);

        // A file that cannot be read fails the whole state: nothing changes.
        a->set_param(native::cab::kOutput, -6.0f);
        const std::string missing = (dir / "gone.wav").string();
        CHECK(!native::load_state(*b, "{\"processor\": \"org.sonora.cab\", \"state_version\": 1, \"params\": "
                                      "{\"output\": -12.0}, \"files\": {\"ir\": \"" + missing + "\"}}", why));
        CHECK(why.find("ir") != std::string::npos);
        CHECK(b->files()->path(0) == p2 && b->param(native::cab::kOutput) == 0.0f);

        // A state without a file puts the unit impulse back.
        CHECK(native::load_state(*b, "{\"processor\": \"org.sonora.cab\", \"state_version\": 1, \"params\": {}}", why));
        CHECK(b->files()->path(0).empty());
        CHECK(std::fabs(response(*b)[0] - 1.0f) < 1e-6f);
    }

    std::filesystem::remove_all(dir);
    return failures == 0 ? 0 : 1;
}
