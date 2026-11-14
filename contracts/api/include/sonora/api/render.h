// SPDX-License-Identifier: GPL-3.0-or-later
//
// Render in place on the wire (plugins.graph.freeze, plugins.offline;
// ADR-0051, protocol 56).
//
//   Command  "track.freeze"            argument: format_render_argument()
//   Command  "track.commit"            the same
//   Command  "track.process_selection" the same, with start and end
//   Command  "track.unfreeze"          "track=<id>"; applied at once, no render
//   Command  "track.render_cancel"     no argument; refused when none runs.
//   Event    EventKind::Render while one runs or ended:
//              frame  = the fraction rendered, in thousandths, while Running
//              insert = the track's model id
//              flags  = RenderState
//              text   = Done: what was done ("Vox frozen"); Failed and
//                       Cancelled: the reason; empty while Running
//            One stands from the first progress on; a client that attaches
//            later is sent it. Done, Failed and Cancelled are held notices
//            the interface shows until dismissed, not a status line.
//   Capability "track.render" in Hello::capabilities.
//
// The three renders run the track's own plug-in chain, hosted out of process
// as always (ADR-0004), with the transport rolling over the range in real
// time: the hosts run only at the audio callback. Any block the chain did
// not play, or the disk did not serve, fails the render and nothing is kept
// (ADR-0012 §3). The argument is "key=value" words split on spaces, the
// name last and the rest of the field:
//   "track=<id> [start=<frame> end=<frame>] [tail=<ms>] [insert=<id>] [name=<text>]"
//
// insert=<id> (ADR-0087) is taken by track.process_selection only: the
// selection goes through that one insert, the track's other inserts silenced
// for the render and left as they are in the document. Freeze and Commit
// refuse it, as an engine without "track.render.insert" in its capabilities
// refuses the word: a client checks the capability before sending it, so no
// protocol bump is needed.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>

namespace sonora::api {

enum class RenderState : uint32_t { Running = 0, Done = 1, Failed = 2, Cancelled = 3 };

inline constexpr const char* kCapabilityRender = "track.render";
// The engine renders Process Selection through one chosen insert (ADR-0087).
inline constexpr const char* kCapabilityRenderInsert = "track.render.insert";

// The one command whose argument may carry insert=<id>.
inline constexpr const char* kCommandProcessSelection = "track.process_selection";

// The longest ring-out a render waits for after the audio ends.
inline constexpr uint32_t kMaxRenderTailMs = 60'000;

struct RenderOptions {
    uint64_t track = 0;       // the track's model id
    // [start, end) in timeline frames. Process Selection needs both; Freeze
    // and Commit take the extent of the track's audio and ignore them.
    uint64_t start = 0;
    uint64_t end = 0;
    // Frames of ring-out kept past the audio, in ms: a reverb's tail is the
    // plug-in's, not the take's. Freeze and Commit default to 2 s and Process
    // to none (-1 means the command's own default).
    int64_t tail_ms = -1;
    // Process Selection only: the model id of the one insert the selection
    // goes through; 0 is the track's whole active chain (ADR-0087).
    uint64_t only_insert = 0;
    std::string name;         // empty: "<track> Frozen", "<track> Committed", "<track> Processed"
};

inline std::string format_render_argument(const RenderOptions& o) {
    std::string s = "track=" + std::to_string(o.track);
    if (o.end != 0) s += " start=" + std::to_string(o.start) + " end=" + std::to_string(o.end);
    if (o.tail_ms >= 0) s += " tail=" + std::to_string(o.tail_ms);
    if (o.only_insert != 0) s += " insert=" + std::to_string(o.only_insert);
    if (!o.name.empty()) s += " name=" + o.name;
    return s;
}

namespace detail {

inline bool parse_render_words(const std::string& arg, bool insert_allowed, RenderOptions& out,
                               std::string& why) {
    RenderOptions o;
    bool have_track = false;
    std::size_t pos = 0;
    while (pos < arg.size()) {
        if (arg[pos] == ' ') {
            ++pos;
            continue;
        }
        const std::size_t eq = arg.find('=', pos);
        if (eq == std::string::npos) {
            why = "\"" + arg.substr(pos) + "\" is not key=value";
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
        if (key != "track" && key != "start" && key != "end" && key != "tail" &&
            key != "insert") {
            why = "unknown word \"" + key + "\"";
            return false;
        }
        if (key == "insert" && !insert_allowed) {
            why = "insert= applies only to Process Selection; freeze and commit print the whole chain";
            return false;
        }
        // 18 digits: far past any session, and strtoull cannot overflow.
        char* endp = nullptr;
        const unsigned long long n = std::strtoull(v.c_str(), &endp, 10);
        if (v.empty() || v.size() > 18 || *endp != '\0' || v[0] == '-' || v[0] == '+') {
            why = key + " is a whole number, not \"" + v + "\"";
            return false;
        }
        if (key == "track") {
            o.track = n;
            have_track = true;
        } else if (key == "start") {
            o.start = n;
        } else if (key == "end") {
            o.end = n;
        } else if (key == "insert") {
            if (n == 0) {
                why = "insert=0 names no insert; leave the word out for the whole chain";
                return false;
            }
            o.only_insert = n;
        } else {
            if (n > kMaxRenderTailMs) {
                why = "tail is at most " + std::to_string(kMaxRenderTailMs) + " ms";
                return false;
            }
            o.tail_ms = static_cast<int64_t>(n);
        }
    }
    if (!have_track || o.track == 0) {
        why = "name the track: track=<id>";
        return false;
    }
    if (o.end != 0 && o.end <= o.start) {
        why = "the range to render is empty";
        return false;
    }
    out = o;
    return true;
}

}  // namespace detail

// The argument of `command`: insert=<id> is read for track.process_selection
// and refused, with the reason, for any other. False, `why` set, for a word
// it does not know or a value it cannot read.
inline bool parse_render_argument(const std::string& command, const std::string& arg,
                                  RenderOptions& out, std::string& why) {
    return detail::parse_render_words(arg, command == kCommandProcessSelection, out, why);
}

// The argument as an engine without "track.render.insert" reads it: insert=
// is refused for every command, so a render is never run on more of the
// chain than the operator asked for.
inline bool parse_render_argument(const std::string& arg, RenderOptions& out, std::string& why) {
    return detail::parse_render_words(arg, false, out, why);
}

}  // namespace sonora::api
