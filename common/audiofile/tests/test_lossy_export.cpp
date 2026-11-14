// SPDX-License-Identifier: GPL-3.0-or-later
//
// Opus and Vorbis export (ADR-0182): a sine is rendered to a file, decoded
// with the codec's own library, and its rate, duration, level and tag are
// checked. A codec that is not registered fails before any file is made.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "sonora/lossy_export.h"
#include "sonora/wav_writer.h"

#ifdef SONORA_HAVE_OPUS
#include <ogg/ogg.h>
#include <opus/opus.h>
#endif
#ifdef SONORA_HAVE_VORBIS
#include <vorbis/vorbisfile.h>
#endif

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            return 1;                                                              \
        }                                                                          \
    } while (0)

namespace {

using namespace sonora;

constexpr double kPi = 3.14159265358979323846;
constexpr double kAmp = 0.3;
constexpr uint32_t kSourceRate = 44100;
constexpr int kSeconds = 2;

bool make_source(const std::string& path) {
    std::remove(path.c_str());
    WavWriter w;
    if (!w.open(path, kSourceRate, 2, WavWriter::Format::Float32, 0)) return false;
    const int frames = static_cast<int>(kSourceRate) * kSeconds;
    std::vector<float> il(static_cast<size_t>(frames) * 2);
    for (int i = 0; i < frames; ++i) {
        const float s = static_cast<float>(kAmp * std::sin(2 * kPi * 1000.0 * i / kSourceRate));
        il[2 * i] = s;
        il[2 * i + 1] = s;
    }
    return w.write(il.data(), static_cast<size_t>(frames)) && w.close();
}

bool exists(const std::string& path) { return std::ifstream(path, std::ios::binary).good(); }

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// Left channel RMS over the middle half, away from the codec's ramp-in.
double middle_rms(const std::vector<float>& il, int channels) {
    const size_t frames = il.size() / static_cast<size_t>(channels);
    double sum = 0;
    size_t n = 0;
    for (size_t i = frames / 4; i < frames * 3 / 4; ++i, ++n) {
        const double v = il[i * static_cast<size_t>(channels)];
        sum += v * v;
    }
    return std::sqrt(sum / static_cast<double>(n));
}

bool near_level(double rms) {
    const double want = kAmp / std::sqrt(2.0);
    return std::fabs(rms - want) < 0.1 * want;
}

int refuses_an_unregistered_codec() {
    const std::string src = "test_lossy_src.wav", dst = "test_lossy_none.opus";
    CHECK(make_source(src));
    std::remove(dst.c_str());
    LossyRegistry empty;
    std::string error;
    CHECK(!export_lossy(src, dst, LossyCodec::Opus, 128000, ExportMeta{}, 0.0, empty, error));
    CHECK(error.find("Opus") != std::string::npos);
    CHECK(error.find("not available") != std::string::npos);
    CHECK(!exists(dst));
    CHECK(!export_lossy(src, dst, LossyCodec::Mp3, 128000, ExportMeta{}, 0.0, empty, error));
    CHECK(!exists(dst));
    std::remove(src.c_str());
    return 0;
}

#ifdef SONORA_HAVE_OPUS
int opus_round_trip(const LossyRegistry& reg) {
    const std::string src = "test_lossy_src.wav", dst = "test_lossy_out.opus";
    CHECK(make_source(src));
    ExportMeta meta;
    meta.description = "take 7";
    std::string error;
    CHECK(export_lossy(src, dst, LossyCodec::Opus, 128000, meta, 0.0, reg, error));
    const std::string bytes = slurp(dst);
    CHECK(bytes.compare(0, 4, "OggS") == 0);
    CHECK(bytes.find("OpusHead") != std::string::npos);
    CHECK(bytes.find("DESCRIPTION=take 7") != std::string::npos);

    ogg_sync_state oy;
    ogg_sync_init(&oy);
    char* buf = ogg_sync_buffer(&oy, static_cast<long>(bytes.size()));
    std::copy(bytes.begin(), bytes.end(), buf);
    ogg_sync_wrote(&oy, static_cast<long>(bytes.size()));
    ogg_stream_state os;
    bool init = false;
    OpusDecoder* dec = nullptr;
    int preskip = 0, channels = 0;
    int64_t last_granule = 0;
    int packets = 0;
    std::vector<float> pcm;
    std::vector<float> frame(5760 * 2);
    ogg_page og;
    while (ogg_sync_pageout(&oy, &og) == 1) {
        if (!init) {
            ogg_stream_init(&os, ogg_page_serialno(&og));
            init = true;
        }
        ogg_stream_pagein(&os, &og);
        ogg_packet op;
        while (ogg_stream_packetout(&os, &op) == 1) {
            if (packets == 0) {
                CHECK(op.bytes >= 19);
                channels = op.packet[9];
                preskip = op.packet[10] | (op.packet[11] << 8);
                const uint32_t rate = static_cast<uint32_t>(op.packet[12]) |
                                      (static_cast<uint32_t>(op.packet[13]) << 8) |
                                      (static_cast<uint32_t>(op.packet[14]) << 16);
                CHECK(rate == 48000 && channels == 2);
                int err = 0;
                dec = opus_decoder_create(48000, channels, &err);
                CHECK(dec != nullptr && err == OPUS_OK);
            } else if (packets >= 2) {
                const int n = opus_decode_float(dec, op.packet, static_cast<opus_int32>(op.bytes),
                                                frame.data(), 5760, 0);
                CHECK(n > 0);
                pcm.insert(pcm.end(), frame.begin(), frame.begin() + n * channels);
                last_granule = op.granulepos;
            }
            ++packets;
        }
    }
    CHECK(dec != nullptr);
    // 2 s at 44.1 kHz is 96000 frames at 48 kHz.
    CHECK(last_granule - preskip == 96000);
    pcm.erase(pcm.begin(), pcm.begin() + static_cast<long>(preskip) * channels);
    pcm.resize(static_cast<size_t>(last_granule - preskip) * static_cast<size_t>(channels));
    CHECK(near_level(middle_rms(pcm, channels)));
    opus_decoder_destroy(dec);
    ogg_stream_clear(&os);
    ogg_sync_clear(&oy);
    std::remove(src.c_str());
    std::remove(dst.c_str());
    return 0;
}
#endif

#ifdef SONORA_HAVE_VORBIS
int vorbis_round_trip(const LossyRegistry& reg) {
    const std::string src = "test_lossy_src.wav", dst = "test_lossy_out.ogg";
    CHECK(make_source(src));
    ExportMeta meta;
    meta.description = "take 7";
    std::string error;
    CHECK(export_lossy(src, dst, LossyCodec::Vorbis, 128000, meta, 0.0, reg, error));
    CHECK(slurp(dst).find("DESCRIPTION=take 7") != std::string::npos);

    OggVorbis_File vf;
    CHECK(ov_fopen(dst.c_str(), &vf) == 0);
    const vorbis_info* vi = ov_info(&vf, -1);
    CHECK(vi->rate == static_cast<long>(kSourceRate) && vi->channels == 2);
    CHECK(ov_pcm_total(&vf, -1) == static_cast<ogg_int64_t>(kSourceRate) * kSeconds);
    std::vector<float> pcm;
    for (;;) {
        float** chans = nullptr;
        const long n = ov_read_float(&vf, &chans, 4096, nullptr);
        if (n <= 0) break;
        for (long i = 0; i < n; ++i) {
            pcm.push_back(chans[0][i]);
            pcm.push_back(chans[1][i]);
        }
    }
    ov_clear(&vf);
    CHECK(pcm.size() / 2 == static_cast<size_t>(kSourceRate) * kSeconds);
    CHECK(near_level(middle_rms(pcm, 2)));
    std::remove(src.c_str());
    std::remove(dst.c_str());
    return 0;
}
#endif

}  // namespace

int main() {
    if (refuses_an_unregistered_codec() != 0) return 1;
    LossyRegistry reg;
    std::string error;
    CHECK(register_builtin_lossy_codecs(reg, error));
#ifdef SONORA_HAVE_OPUS
    CHECK(reg.available(LossyCodec::Opus));
    if (opus_round_trip(reg) != 0) return 1;
#else
    CHECK(!reg.available(LossyCodec::Opus));
#endif
#ifdef SONORA_HAVE_VORBIS
    CHECK(reg.available(LossyCodec::Vorbis));
    if (vorbis_round_trip(reg) != 0) return 1;
#else
    CHECK(!reg.available(LossyCodec::Vorbis));
#endif
    std::printf("lossy_export: ok\n");
    return 0;
}
