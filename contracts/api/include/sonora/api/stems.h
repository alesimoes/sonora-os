// SPDX-License-Identifier: GPL-3.0-or-later
//
// Export Stems on the wire (interchange.stems, ADR-0085). No protocol bump:
// the command is new, its capability says whether an engine has it, and its
// progress rides the existing EventKind::Bounce.
//
//   Command  "mix.stems"          argument: format_stems_argument()
//   Command  "mix.bounce_cancel"  cancels a stem export too: one export runs
//                                 at a time, a bounce or a stem export.
//   Event    EventKind::Bounce while a stem export runs or ended:
//              frame  = thousandths of the whole export, over every file
//                       (stem k of n at fraction f is (k + f) / n)
//              insert = the number of files written, once Done, else 0
//              flags  = BounceState
//              text   = the folder relative to the session folder once Done,
//                       kStemsParent + "<name>" + kStemsFolderSuffix (e.g.
//                       "Bounced Files/Mix Stems"); the reason once Failed or
//                       Cancelled; empty while Running
//            A client tells it from a bounce by the command it sent. The
//            held-notice rule of bounce.h holds: Done, Failed and Cancelled
//            stay on screen until dismissed.
//   Capability kCapabilityStems in Hello::capabilities.
//
// The argument is the bounce argument with two words in front of it:
//   "by=track|output [tracks=<id>,<id>...] <the mix.bounce argument>"
// by=track: one stem per audio track (every unmuted one, or those named in
// tracks=). by=output: one stem per output the tracks feed (a bus or the
// master); tracks= is refused with it. An empty or absent tracks= is all.
// Stems render offline only: mode=realtime is refused, because n passes at
// the speed of the music is n times the session length of the operator's
// time with no gain in what the files hold.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "sonora/api/bounce.h"

namespace sonora::api {

enum class StemsBy : uint32_t { Track = 0, Output = 1 };

inline constexpr const char* kCapabilityStems = "mix.stems";
// Where the stems of one export go, relative to the session folder:
// kStemsParent + name + kStemsFolderSuffix. Every file in it has the same
// start and length, so they line up when dropped at the same position.
inline constexpr const char* kStemsParent = "Bounced Files/";
inline constexpr const char* kStemsFolderSuffix = " Stems";

struct StemsOptions {
    // Mode is always Offline once parsed; name names the folder.
    BounceOptions base;
    StemsBy by = StemsBy::Track;
    // Track ids, in the order given; empty: every track.
    std::vector<uint64_t> tracks;
};

inline std::string stems_folder(const std::string& name) {
    return std::string(kStemsParent) + name + kStemsFolderSuffix;
}

inline std::string format_stems_argument(const StemsOptions& o) {
    std::string s = std::string("by=") + (o.by == StemsBy::Output ? "output" : "track");
    if (!o.tracks.empty()) {
        s += " tracks=";
        for (std::size_t i = 0; i < o.tracks.size(); ++i) {
            if (i != 0) s += ',';
            s += std::to_string(o.tracks[i]);
        }
    }
    return s + " " + format_bounce_argument(o.base);
}

namespace detail {

// "1,2,3" into ids; refuses an empty item, a non-number, 0 and a repeat.
inline bool parse_stems_track_list(const std::string& v, std::vector<uint64_t>& ids,
                                   std::string& why) {
    std::size_t pos = 0;
    while (pos <= v.size()) {
        std::size_t comma = v.find(',', pos);
        if (comma == std::string::npos) comma = v.size();
        const std::string item = v.substr(pos, comma - pos);
        char* endp = nullptr;
        const unsigned long long n = std::strtoull(item.c_str(), &endp, 10);
        // 18 digits: no overflow in strtoull; ids are never that long.
        if (item.empty() || item.size() > 18 || *endp != '\0' || item[0] == '-' ||
            item[0] == '+' || n == 0) {
            why = "stems tracks is a list of track ids, not \"" + v + "\"";
            return false;
        }
        for (const uint64_t seen : ids) {
            if (seen == n) {
                why = "stems tracks names track " + item + " twice";
                return false;
            }
        }
        ids.push_back(n);
        pos = comma + 1;
    }
    return true;
}

// The bounce parser's reasons say "mix.bounce:"; the operator sent mix.stems.
inline std::string stems_reason(std::string why) {
    static const std::string bounce = "mix.bounce:";
    if (why.compare(0, bounce.size(), bounce) == 0) why.replace(0, bounce.size(), "mix.stems:");
    return why;
}

}  // namespace detail

// False, `why` set, for a word it does not know, a value it cannot read, a
// repeated by= or tracks=, tracks= with by=output, or mode=realtime.
inline bool parse_stems_argument(const std::string& arg, StemsOptions& out, std::string& why) {
    StemsOptions o;
    bool have_by = false;
    bool have_tracks = false;
    std::string rest;  // every other word, in order, the name last
    std::size_t pos = 0;
    while (pos < arg.size()) {
        if (arg[pos] == ' ') {
            ++pos;
            continue;
        }
        const std::size_t eq = arg.find('=', pos);
        const std::size_t space = arg.find(' ', pos);
        if (eq != std::string::npos && (space == std::string::npos || eq < space)) {
            const std::string key = arg.substr(pos, eq - pos);
            if (key == "name") {
                // The name is the rest of the field, spaces and '=' included.
                rest += (rest.empty() ? "" : " ") + arg.substr(pos);
                break;
            }
            const std::size_t stop = space == std::string::npos ? arg.size() : space;
            const std::string v = arg.substr(eq + 1, stop - eq - 1);
            if (key == "by" || key == "tracks") {
                bool& seen = key == "by" ? have_by : have_tracks;
                if (seen) {
                    why = "mix.stems: \"" + key + "\" is given twice";
                    return false;
                }
                seen = true;
                if (key == "by") {
                    if (v == "track") o.by = StemsBy::Track;
                    else if (v == "output") o.by = StemsBy::Output;
                    else { why = "stems by is track or output, not \"" + v + "\""; return false; }
                } else if (!v.empty() && !detail::parse_stems_track_list(v, o.tracks, why)) {
                    return false;
                }
                pos = stop;
                continue;
            }
        }
        // Not ours: handed on whole, so the bounce parser gives its own reason.
        const std::size_t stop = space == std::string::npos ? arg.size() : space;
        rest += (rest.empty() ? "" : " ") + arg.substr(pos, stop - pos);
        pos = stop;
    }
    if (!parse_bounce_argument(rest, o.base, why)) {
        why = detail::stems_reason(why);
        return false;
    }
    if (o.base.mode == BounceMode::RealTime) {
        why = "stems render offline only: a real-time pass per stem takes the session's "
              "length once for every file and records nothing a faster pass does not";
        return false;
    }
    if (o.by == StemsBy::Output && !o.tracks.empty()) {
        why = "stems by output take every track; tracks= is for stems by track";
        return false;
    }
    out = o;
    return true;
}

}  // namespace sonora::api
