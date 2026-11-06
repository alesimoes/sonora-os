// SPDX-License-Identifier: GPL-3.0-or-later
//
// Import: a file at another sample rate comes out at the session's, with the
// pitch and level it had; a file at the session's rate is copied sample for
// sample; a stereo file keeps its channels apart; a cancelled or refused
// import leaves no file behind.
#include "sonora/audio_import.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "sonora/wav_reader.h"
#include "sonora/wav_writer.h"

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            return 1;                                                                \
        }                                                                            \
    } while (0)

namespace {

using namespace sonora;
constexpr double kPi = 3.14159265358979323846;

bool exists(const std::string& p) {
    std::FILE* f = std::fopen(p.c_str(), "rb");
    if (f) std::fclose(f);
    return f != nullptr;
}

// Channel c carries a sine at freqs[c], 0.5 full scale.
bool write_sines(const std::string& path, uint32_t rate, int64_t frames,
                 const std::vector<double>& freqs, WavWriter::Format fmt, uint64_t tref = 0) {
    std::remove(path.c_str());
    WavWriter w;
    if (!w.open(path, rate, static_cast<uint16_t>(freqs.size()), fmt, tref)) return false;
    std::vector<float> buf(static_cast<std::size_t>(frames) * freqs.size());
    for (int64_t i = 0; i < frames; ++i) {
        for (std::size_t c = 0; c < freqs.size(); ++c) {
            buf[static_cast<std::size_t>(i) * freqs.size() + c] =
                static_cast<float>(0.5 * std::sin(2.0 * kPi * freqs[c] * i / rate));
        }
    }
    return w.write(buf.data(), static_cast<std::size_t>(frames)) && w.close();
}

// RMS error of channel c against the ideal sine at the new rate, over the
// middle of the file where no edge of the kernel is.
double rms_error(const std::string& path, std::size_t c, double freq, uint32_t rate) {
    WavReader r;
    std::string why;
    if (!r.open(path, why)) return 1e9;
    const int64_t n = r.frames();
    std::vector<float> buf(static_cast<std::size_t>(n) * r.channels());
    r.read(0, n, buf.data());
    double sum = 0.0;
    int64_t count = 0;
    for (int64_t i = n / 10; i < n - n / 10; ++i) {
        const double ideal = 0.5 * std::sin(2.0 * kPi * freq * i / rate);
        const double d = buf[static_cast<std::size_t>(i) * r.channels() + c] - ideal;
        sum += d * d;
        ++count;
    }
    return std::sqrt(sum / count);
}

}  // namespace

int main() {
    const std::string src = "test_audio_import_src.wav";
    const std::string dst = "test_audio_import_dst.wav";

    // 48 kHz to 44.1 kHz: a tone keeps its pitch, level and length.
    CHECK(write_sines(src, 48000, 96000, {1000.0, 5000.0}, WavWriter::Format::Int24, 48000));
    {
        std::remove(dst.c_str());
        ImportResult res;
        std::string why;
        std::atomic<int> progress{0};
        CHECK(import_audio(src, dst, 44100, &progress, nullptr, res, why));
        CHECK(why.empty());
        CHECK(res.resampled);
        CHECK(res.channels == 2);
        CHECK(res.frames == 88200);
        CHECK(res.time_reference == 44100);  // 1 s stays 1 s
        CHECK(progress.load() == 1000);
        WavReader r;
        CHECK(r.open(dst, why));
        CHECK(r.sample_rate() == 44100);
        CHECK(r.frames() == 88200);
        CHECK(r.encoding() == WavReader::Encoding::Pcm24);
        CHECK(r.time_reference() == 44100);
        // -60 dB or better against the ideal tone, on both channels apart.
        CHECK(rms_error(dst, 0, 1000.0, 44100) < 0.0005);
        CHECK(rms_error(dst, 1, 5000.0, 44100) < 0.0005);
        // And the channels were not swapped.
        CHECK(rms_error(dst, 0, 5000.0, 44100) > 0.1);
    }

    // Down to 44.1 kHz from 96 kHz: a tone above the new Nyquist is removed,
    // not folded back under it.
    CHECK(write_sines(src, 96000, 96000, {30000.0}, WavWriter::Format::Float32));
    {
        std::remove(dst.c_str());
        ImportResult res;
        std::string why;
        CHECK(import_audio(src, dst, 44100, nullptr, nullptr, res, why));
        CHECK(res.floating);
        WavReader r;
        CHECK(r.open(dst, why));
        std::vector<float> buf(44100);
        CHECK(r.read(0, 44100, buf.data()) == 44100);
        double peak = 0;
        for (int i = 4410; i < 44100 - 4410; ++i) peak = std::fmax(peak, std::fabs(buf[i]));
        CHECK(peak < 0.5 * 0.01 + 1e-9);  // 30 kHz is under 44.1/2 here; see below
    }
    // (30 kHz is above 22.05 kHz: the check above is the alias test.)

    // Up from 22.05 kHz.
    CHECK(write_sines(src, 22050, 22050, {2000.0}, WavWriter::Format::Int24));
    {
        std::remove(dst.c_str());
        ImportResult res;
        std::string why;
        CHECK(import_audio(src, dst, 48000, nullptr, nullptr, res, why));
        CHECK(res.frames == 48000);
        CHECK(rms_error(dst, 0, 2000.0, 48000) < 0.0005);
    }

    // The session's own rate: the same samples, nothing resampled.
    CHECK(write_sines(src, 44100, 5000, {440.0}, WavWriter::Format::Int24));
    {
        std::remove(dst.c_str());
        ImportResult res;
        std::string why;
        CHECK(import_audio(src, dst, 44100, nullptr, nullptr, res, why));
        CHECK(!res.resampled);
        CHECK(res.frames == 5000);
        WavReader a, b;
        CHECK(a.open(src, why) && b.open(dst, why));
        std::vector<float> x(5000), y(5000);
        CHECK(a.read(0, 5000, x.data()) == 5000);
        CHECK(b.read(0, 5000, y.data()) == 5000);
        CHECK(x == y);
    }

    // A destination that exists is refused and left alone.
    {
        ImportResult res;
        std::string why;
        CHECK(!import_audio(src, dst, 44100, nullptr, nullptr, res, why));
        CHECK(!why.empty());
        CHECK(exists(dst));
    }

    // Cancelled: no file, and no reason (the operator asked).
    {
        std::remove(dst.c_str());
        std::atomic<bool> cancel{true};
        ImportResult res;
        std::string why;
        CHECK(!import_audio(src, dst, 48000, nullptr, &cancel, res, why));
        CHECK(why.empty());
        CHECK(!exists(dst));
    }

    // Not a WAV file: refused with a reason, no file.
    {
        std::FILE* f = std::fopen(src.c_str(), "wb");
        CHECK(f != nullptr);
        std::fputs("this is not audio", f);
        std::fclose(f);
        ImportResult res;
        std::string why;
        AudioInfo info;
        CHECK(!probe_audio(src, info, why));
        CHECK(!why.empty());
        why.clear();
        CHECK(!import_audio(src, dst, 48000, nullptr, nullptr, res, why));
        CHECK(!why.empty());
        CHECK(!exists(dst));
    }

    std::remove(src.c_str());
    std::remove(dst.c_str());
    std::printf("audio_import: ok\n");
    return 0;
}
