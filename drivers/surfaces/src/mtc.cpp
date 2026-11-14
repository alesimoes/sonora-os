// SPDX-License-Identifier: GPL-3.0-or-later
// Timeline maths for MTC sync (ADR-0122). The codec and the drop-frame
// label arithmetic are the contract's, inline in mtc.h; this is what the
// chase and the generator add on top: signed positions, quarter-frame
// indices and the settings check.
#include <cstdio>
#include <string>

#include "sonora/surfaces/mtc_sync.h"

namespace sonora::surfaces {

namespace {

// Floor division: a position before timecode zero is in the frame before.
std::int64_t floor_div(std::int64_t a, std::int64_t b) {
    const std::int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

bool known_rate(FrameRate r) {
    return static_cast<std::uint8_t>(r) <= static_cast<std::uint8_t>(FrameRate::Fps30);
}

const char* mode_name(MtcMode m) {
    switch (m) {
        case MtcMode::Chase: return "chase";
        case MtcMode::Generate: return "generate";
    }
    return "unknown";
}

}  // namespace

std::int64_t offset_for_session_start(const Timecode& at_sample_zero, std::uint32_t sample_rate) {
    return -static_cast<std::int64_t>(to_samples(at_sample_zero, sample_rate));
}

std::int64_t quarter_frame_index(std::int64_t samples, FrameRate rate, std::uint32_t sample_rate) {
    const RateRatio r = rate_ratio(rate);
    return floor_div(samples * 4 * static_cast<std::int64_t>(r.num),
                     static_cast<std::int64_t>(sample_rate) * static_cast<std::int64_t>(r.den));
}

Timecode timecode_at(std::int64_t samples, FrameRate rate, std::uint32_t sample_rate) {
    const RateRatio r = rate_ratio(rate);
    const std::int64_t frames =
        floor_div(samples * static_cast<std::int64_t>(r.num),
                  static_cast<std::int64_t>(sample_rate) * static_cast<std::int64_t>(r.den));
    return from_frames(frames, rate);
}

std::string format_timecode(const Timecode& tc) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02u:%02u:%02u%c%02u", unsigned{tc.hours},
                  unsigned{tc.minutes}, unsigned{tc.seconds},
                  tc.rate == FrameRate::Fps2997Drop ? ';' : ':', unsigned{tc.frames});
    return buf;
}

std::string validate_mtc_settings(const MtcSyncSettings& s, MtcMode expected) {
    if (s.mode != MtcMode::Chase && s.mode != MtcMode::Generate) {
        return "MTC sync: the mode is neither chase nor generate";
    }
    if (s.mode != expected) {
        return std::string("MTC sync: settings ask for ") + mode_name(s.mode) +
               " but this instance can only " + mode_name(expected) +
               "; chase and generate never run on one instance";
    }
    if (!known_rate(s.rate)) return "MTC sync: unknown frame rate code";
    // Below this a quarter frame (1/120 s at 30 fps) is less than a few
    // samples and the cadence is meaningless; above it no interface runs.
    if (s.sample_rate < 8000 || s.sample_rate > 384000) {
        return "MTC sync: sample rate " + std::to_string(s.sample_rate) + " Hz is out of range";
    }
    const std::int64_t day = static_cast<std::int64_t>(
        frames_to_samples(frames_per_day(s.rate), s.rate, s.sample_rate));
    if (s.offset_samples > day || s.offset_samples < -day) {
        return "MTC sync: the timecode offset is more than a day";
    }
    return {};
}

}  // namespace sonora::surfaces
