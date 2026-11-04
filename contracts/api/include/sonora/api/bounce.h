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
//   "mode=offline|realtime start=<frame> end=<frame> format=int24|float32
//    [normalise=<LUFS>] [name=<text>]"
#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>

namespace sonora::api {

enum class BounceMode : uint32_t { Offline = 0, RealTime = 1 };
enum class BounceFormat : uint32_t { Int24 = 0, Float32 = 1 };
enum class BounceState : uint32_t { Running = 0, Done = 1, Failed = 2, Cancelled = 3 };

inline constexpr const char* kCapabilityBounce = "mix.bounce";

struct BounceOptions {
    BounceMode mode = BounceMode::Offline;
    // [start, end) in timeline frames; end 0 is the end of the last region.
    uint64_t start = 0;
    uint64_t end = 0;
    BounceFormat format = BounceFormat::Int24;
    // The file is always a BWF: the writer's bext carries the start as its
    // TimeReference, and a BWF opens as a WAV everywhere.
    // Off by default. On, the integrated loudness is brought to
    // `target_lufs` by one gain, never past a -1 dBFS sample peak.
    bool normalise = false;
    double target_lufs = -16.0;
    std::string name;   // empty: "<session> Bounce"
};

inline std::string format_bounce_argument(const BounceOptions& o) {
    std::string s = std::string("mode=") + (o.mode == BounceMode::RealTime ? "realtime" : "offline");
    s += " start=" + std::to_string(o.start) + " end=" + std::to_string(o.end);
    s += std::string(" format=") + (o.format == BounceFormat::Float32 ? "float32" : "int24");
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
            else { why = "bounce format is int24 or float32, not \"" + v + "\""; return false; }
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
    out = o;
    return true;
}

}  // namespace sonora::api
