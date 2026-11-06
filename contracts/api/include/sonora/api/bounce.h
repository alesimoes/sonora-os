// SPDX-License-Identifier: GPL-3.0-or-later
//
// Bounce Mix on the wire (ui.mixer.bounce, protocol 49).
//
//   Command  "mix.bounce"         argument: format_bounce_argument()
//   Command  "mix.bounce_cancel"  no argument; refused when none runs.
//   Event    EventKind::Bounce while one runs or ended:
//              frame  = frames rendered so far, in thousandths of the whole
//                       while Running (the fraction, as Consolidate says it)
//              insert = frames in all, once known (Done), else 0
//              flags  = BounceState
//              text   = the file's path relative to the session folder once
//                       Done, the reason once Failed or Cancelled, empty
//                       while Running
//            One stands from the first progress on; a client that attaches
//            later is sent it. Done, Failed and Cancelled are held notices
//            the interface shows until dismissed, not a status line.
//   Capability "mix.bounce" in Hello::capabilities.
//
// The argument is "key=value" words split on spaces, the name last and the
// rest of the field (a name may hold spaces, ';' and '='), the style of
// session.consolidate's:
//   "mode=offline|realtime start=<frame> end=<frame> file=bwf|wav|aiff|flac
//    format=int16|int24|float32 [rate=<Hz>] dither=auto|none|tpdf|shaped
//    [normalise=<LUFS>] [name=<text>]"
// Protocol 61 added file, rate, dither and int16. Each is optional on the
// way in (the defaults are a BWF at the session's rate, dithered when 16-bit).
// FLAC and AIFF hold no 32-bit float: the pair is refused when parsed.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>

namespace sonora::api {

enum class BounceMode : uint32_t { Offline = 0, RealTime = 1 };
enum class BounceFormat : uint32_t { Int24 = 0, Float32 = 1, Int16 = 2 };
// What the file is: a BWF (a WAV with a bext chunk, the default: it opens as
// a WAV everywhere and carries the start on the timeline), a plain WAV, AIFF
// or FLAC.
enum class BounceFile : uint32_t { Bwf = 0, Wav = 1, Aiff = 2, Flac = 3 };
// Auto is TPDF when the file is 16-bit and none otherwise. The others apply
// to any integer depth; 32-bit float is never dithered.
enum class BounceDither : uint32_t { Auto = 0, None = 1, Tpdf = 2, Shaped = 3 };
enum class BounceState : uint32_t { Running = 0, Done = 1, Failed = 2, Cancelled = 3 };

inline constexpr const char* kCapabilityBounce = "mix.bounce";

struct BounceOptions {
    BounceMode mode = BounceMode::Offline;
    // [start, end) in timeline frames; end 0 is the end of the last region.
    uint64_t start = 0;
    uint64_t end = 0;
    BounceFormat format = BounceFormat::Int24;
    BounceFile file = BounceFile::Bwf;
    // 0: the session's rate; else the file is converted to it.
    uint32_t sample_rate = 0;
    BounceDither dither = BounceDither::Auto;
    // Off by default. On, the integrated loudness is brought to
    // `target_lufs` by one gain, never past a -1 dBFS sample peak.
    bool normalise = false;
    double target_lufs = -16.0;
    std::string name;   // empty: "<session> Bounce"
};

inline std::string format_bounce_argument(const BounceOptions& o) {
    std::string s = std::string("mode=") + (o.mode == BounceMode::RealTime ? "realtime" : "offline");
    s += " start=" + std::to_string(o.start) + " end=" + std::to_string(o.end);
    static const char* const files[] = {"bwf", "wav", "aiff", "flac"};
    static const char* const dithers[] = {"auto", "none", "tpdf", "shaped"};
    s += std::string(" file=") + files[static_cast<uint32_t>(o.file) & 3];
    s += std::string(" format=") + (o.format == BounceFormat::Float32 ? "float32"
                                    : o.format == BounceFormat::Int16 ? "int16" : "int24");
    if (o.sample_rate != 0) s += " rate=" + std::to_string(o.sample_rate);
    s += std::string(" dither=") + dithers[static_cast<uint32_t>(o.dither) & 3];
    if (o.normalise) s += " normalise=" + std::to_string(o.target_lufs);
    if (!o.name.empty()) s += " name=" + o.name;
    return s;
}

// False, `why` set, for a word it does not know or a value it cannot read.
inline bool parse_bounce_argument(const std::string& arg, BounceOptions& out, std::string& why) {
    BounceOptions o;
    std::size_t pos = 0;
    while (pos < arg.size()) {
        if (arg[pos] == ' ') {
            ++pos;
            continue;
        }
        const std::size_t eq = arg.find('=', pos);
        if (eq == std::string::npos) {
            why = "mix.bounce: \"" + arg.substr(pos) + "\" is not key=value";
            return false;
        }
        const std::string key = arg.substr(pos, eq - pos);
        if (key == "name") {
            o.name = arg.substr(eq + 1);
            break;
        }
        std::size_t stop = arg.find(' ', eq);
        if (stop == std::string::npos) stop = arg.size();
        const std::string v = arg.substr(eq + 1, stop - eq - 1);
        pos = stop;
        char* endp = nullptr;
        if (key == "mode") {
            if (v == "offline") o.mode = BounceMode::Offline;
            else if (v == "realtime") o.mode = BounceMode::RealTime;
            else { why = "bounce mode is offline or realtime, not \"" + v + "\""; return false; }
        } else if (key == "start" || key == "end") {
            const unsigned long long n = std::strtoull(v.c_str(), &endp, 10);
            // 18 digits: far past any session, and strtoull cannot overflow.
            if (v.empty() || v.size() > 18 || *endp != '\0' || v[0] == '-' || v[0] == '+') {
                why = "bounce " + key + " is a frame number, not \"" + v + "\"";
                return false;
            }
            (key == "start" ? o.start : o.end) = n;
        } else if (key == "format") {
            if (v == "int24") o.format = BounceFormat::Int24;
            else if (v == "float32") o.format = BounceFormat::Float32;
            else if (v == "int16") o.format = BounceFormat::Int16;
            else { why = "bounce format is int16, int24 or float32, not \"" + v + "\""; return false; }
        } else if (key == "file") {
            if (v == "bwf") o.file = BounceFile::Bwf;
            else if (v == "wav") o.file = BounceFile::Wav;
            else if (v == "aiff") o.file = BounceFile::Aiff;
            else if (v == "flac") o.file = BounceFile::Flac;
            else { why = "bounce file is bwf, wav, aiff or flac, not \"" + v + "\""; return false; }
        } else if (key == "rate") {
            const unsigned long n = std::strtoul(v.c_str(), &endp, 10);
            if (v.empty() || v.size() > 7 || *endp != '\0' || n < 8000 || n > 384000) {
                why = "bounce rate is between 8000 and 384000 Hz, not \"" + v + "\"";
                return false;
            }
            o.sample_rate = static_cast<uint32_t>(n);
        } else if (key == "dither") {
            if (v == "auto") o.dither = BounceDither::Auto;
            else if (v == "none") o.dither = BounceDither::None;
            else if (v == "tpdf") o.dither = BounceDither::Tpdf;
            else if (v == "shaped") o.dither = BounceDither::Shaped;
            else { why = "bounce dither is auto, none, tpdf or shaped, not \"" + v + "\""; return false; }
        } else if (key == "normalise") {
            const double d = std::strtod(v.c_str(), &endp);
            // !(in range) also refuses NaN.
            if (v.empty() || *endp != '\0' || !(d >= -70.0 && d <= 0.0)) {
                why = "bounce normalise is a target between -70 and 0 LUFS";
                return false;
            }
            o.normalise = true;
            o.target_lufs = d;
        } else {
            why = "mix.bounce: unknown word \"" + key + "\"";
            return false;
        }
    }
    if (o.end != 0 && o.end <= o.start) {
        why = "the range to bounce is empty";
        return false;
    }
    if (o.format == BounceFormat::Float32 &&
        (o.file == BounceFile::Flac || o.file == BounceFile::Aiff)) {
        why = std::string(o.file == BounceFile::Flac ? "FLAC" : "AIFF") +
              " holds 16 or 24 bits, not 32-bit float";
        return false;
    }
    out = o;
    return true;
}

}  // namespace sonora::api
