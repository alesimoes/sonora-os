// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sample conversion: known bytes to known floats, the byte layout written,
// channel placement, clipping and the channels past the engine's outputs.
#include <cmath>
#include <cstdio>
#include <vector>

#include "sonora/alsa_convert.h"

namespace {

using sonora::alsa::SampleFormat;

int failed = 0;

void expect(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failed;
    }
}

bool near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

void test_known_bytes() {
    float out[1];
    float* dst[1] = {out};
    const uint8_t s24_neg[3] = {0x00, 0x00, 0x80};  // -8388608
    sonora::alsa::deinterleave(SampleFormat::S24_3, s24_neg, 1, dst, 1, 1);
    expect(out[0] == -1.0f, "S24_3LE 0x800000 is -1.0");
    const uint8_t s24_half[3] = {0x00, 0x00, 0x40};  // 4194304
    sonora::alsa::deinterleave(SampleFormat::S24_3, s24_half, 1, dst, 1, 1);
    expect(out[0] == 0.5f, "S24_3LE 0x400000 is 0.5");
    const uint8_t s24_4[4] = {0xFF, 0xFF, 0xFF, 0x00};  // low 24 bits all ones = -1 LSB
    sonora::alsa::deinterleave(SampleFormat::S24_4, s24_4, 1, dst, 1, 1);
    expect(near(out[0], -1.0f / 8388608.0f, 1e-12f), "S24_LE sign-extends the low 24 bits");
    const uint8_t s16[2] = {0x00, 0x40};  // 16384
    sonora::alsa::deinterleave(SampleFormat::S16, s16, 1, dst, 1, 1);
    expect(out[0] == 0.5f, "S16_LE 0x4000 is 0.5");
    const uint8_t s32[4] = {0x00, 0x00, 0x00, 0x80};
    sonora::alsa::deinterleave(SampleFormat::S32, s32, 1, dst, 1, 1);
    expect(out[0] == -1.0f, "S32_LE 0x80000000 is -1.0");
}

void test_written_bytes() {
    float in[1] = {0.5f};
    float* src[1] = {in};
    uint8_t b3[3];
    sonora::alsa::interleave(SampleFormat::S24_3, src, 1, b3, 1, 1);
    expect(b3[0] == 0x00 && b3[1] == 0x00 && b3[2] == 0x40, "0.5 is 00 00 40 in S24_3LE");
    uint8_t b4[4];
    sonora::alsa::interleave(SampleFormat::S24_4, src, 1, b4, 1, 1);
    expect(b4[0] == 0x00 && b4[1] == 0x00 && b4[2] == 0x40 && b4[3] == 0x00, "0.5 in S24_LE");
    in[0] = -0.5f;
    sonora::alsa::interleave(SampleFormat::S24_4, src, 1, b4, 1, 1);
    expect(b4[2] == 0xC0 && b4[3] == 0x00, "S24_LE keeps the top byte clear for negatives");
}

void test_clipping() {
    float in[2] = {2.0f, -2.0f};
    float* src[1] = {in};
    int16_t o[2];
    sonora::alsa::interleave(SampleFormat::S16, src, 1, reinterpret_cast<uint8_t*>(o), 1, 2);
    expect(o[0] == 32767 && o[1] == -32768, "out of range clips instead of wrapping (S16)");
    int32_t p[2];
    sonora::alsa::interleave(SampleFormat::S32, src, 1, reinterpret_cast<uint8_t*>(p), 1, 2);
    expect(p[0] == 2147483647 && p[1] == INT32_MIN, "out of range clips instead of wrapping (S32)");
}

void test_round_trip() {
    const SampleFormat formats[] = {SampleFormat::S16, SampleFormat::S24_3, SampleFormat::S24_4,
                                    SampleFormat::S32};
    const float tol[] = {1.0f / 32768.0f, 1.0f / 8388608.0f, 1.0f / 8388608.0f, 1e-6f};
    for (int f = 0; f < 4; ++f) {
        constexpr uint32_t kCh = 5, kFrames = 7;
        std::vector<std::vector<float>> planes(kCh, std::vector<float>(kFrames));
        std::vector<float*> src(kCh);
        for (uint32_t c = 0; c < kCh; ++c) {
            for (uint32_t i = 0; i < kFrames; ++i) {
                planes[c][i] = 0.1f * static_cast<float>(c) - 0.03f * static_cast<float>(i);
            }
            src[c] = planes[c].data();
        }
        std::vector<uint8_t> bytes(kCh * kFrames * 4);
        sonora::alsa::interleave(formats[f], src.data(), kCh, bytes.data(), kCh, kFrames);
        std::vector<std::vector<float>> back(kCh, std::vector<float>(kFrames));
        std::vector<float*> dst(kCh);
        for (uint32_t c = 0; c < kCh; ++c) dst[c] = back[c].data();
        sonora::alsa::deinterleave(formats[f], bytes.data(), kCh, dst.data(), kCh, kFrames);
        bool ok = true;
        for (uint32_t c = 0; c < kCh; ++c) {
            for (uint32_t i = 0; i < kFrames; ++i) ok = ok && near(back[c][i], planes[c][i], tol[f]);
        }
        expect(ok, "interleave then deinterleave returns the signal within one LSB");
    }
}

// Channel 10 must land in slot 10 of each frame, and nowhere else.
void test_channel_placement() {
    constexpr uint32_t kCh = 12, kFrames = 3;
    std::vector<float> plane(kFrames, 0.25f), zero(kFrames, 0.0f);
    std::vector<float*> src(kCh);
    for (uint32_t c = 0; c < kCh; ++c) src[c] = c == 10 ? plane.data() : zero.data();
    std::vector<uint8_t> bytes(kCh * kFrames * 3, 0xAA);
    sonora::alsa::interleave(SampleFormat::S24_3, src.data(), kCh, bytes.data(), kCh, kFrames);
    bool ok = true;
    for (uint32_t i = 0; i < kFrames; ++i) {
        for (uint32_t c = 0; c < kCh; ++c) {
            const uint8_t* p = bytes.data() + (i * kCh + c) * 3;
            const bool tone = p[2] == 0x20;
            ok = ok && (tone == (c == 10));
        }
    }
    expect(ok, "channel 10 lands only in slot 10");
}

// The card has 20 outputs and the engine fewer: the rest of each frame is
// silence, not what the ring held before.
void test_unused_channels_silent() {
    float a[2] = {0.5f, 0.5f};
    float* src[1] = {a};
    std::vector<uint8_t> bytes(2 * 3 * 3, 0xFF);
    sonora::alsa::interleave(SampleFormat::S24_3, src, 1, bytes.data(), 3, 2);
    bool ok = true;
    for (uint32_t i = 0; i < 2; ++i) {
        for (uint32_t c = 1; c < 3; ++c) {
            for (uint32_t k = 0; k < 3; ++k) ok = ok && bytes[(i * 3 + c) * 3 + k] == 0;
        }
    }
    expect(ok, "channels the engine does not drive are written as silence");
}

}  // namespace

int main() {
    test_known_bytes();
    test_written_bytes();
    test_clipping();
    test_round_trip();
    test_channel_placement();
    test_unused_channels_silent();
    if (failed == 0) std::printf("alsa_convert: all passed\n");
    return failed == 0 ? 0 : 1;
}
