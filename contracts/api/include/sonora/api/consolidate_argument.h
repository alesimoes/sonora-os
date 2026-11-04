// SPDX-License-Identifier: GPL-3.0-or-later
//
// The argument of session.consolidate (ADR-0031 §6, protocol 43), written by
// the interface and read by the engine and the fake from this one place.
//
//   "track=<id> start=<frame> end=<frame> [playlist=<id>] [name=<text>]"
//
// Ids are model ids, decimal; the range is [start, end) in frames. The name
// runs to the end of the argument, spaces and all, so it comes last. Without
// "playlist" the track's active one is consolidated.
#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace sonora::api {

struct ConsolidateArgument {
    uint64_t track = 0;
    uint64_t playlist = 0;
    int64_t start = 0;
    int64_t end = 0;
    std::string name;
};

inline std::string format_consolidate_argument(const ConsolidateArgument& a) {
    std::string s = "track=" + std::to_string(a.track) + " start=" + std::to_string(a.start) +
                    " end=" + std::to_string(a.end);
    if (a.playlist != 0) s += " playlist=" + std::to_string(a.playlist);
    if (!a.name.empty()) s += " name=" + a.name;
    return s;
}

namespace detail {

inline bool parse_decimal(const std::string& text, bool allow_negative, int64_t& out) {
    std::size_t i = 0;
    bool negative = false;
    if (i < text.size() && text[i] == '-' && allow_negative) {
        negative = true;
        ++i;
    }
    if (i >= text.size() || text.size() - i > 18) return false;
    int64_t v = 0;
    for (; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        v = v * 10 + (text[i] - '0');
    }
    out = negative ? -v : v;
    return true;
}

}  // namespace detail

// False, with `why`, for anything but the form above: a missing track,
// start or end, a key twice or unknown, a value that is not a number.
inline bool parse_consolidate_argument(const std::string& text, ConsolidateArgument& out,
                                       std::string& why) {
    ConsolidateArgument a;
    bool has_track = false, has_start = false, has_end = false, has_playlist = false;
    std::size_t pos = 0;
    while (pos < text.size()) {
        if (text[pos] == ' ') {
            ++pos;
            continue;
        }
        const std::size_t eq = text.find('=', pos);
        if (eq == std::string::npos) {
            why = "session.consolidate: \"" + text.substr(pos) + "\" is not key=value";
            return false;
        }
        const std::string key = text.substr(pos, eq - pos);
        if (key == "name") {
            a.name = text.substr(eq + 1);
            break;
        }
        std::size_t stop = text.find(' ', eq);
        if (stop == std::string::npos) stop = text.size();
        const std::string value = text.substr(eq + 1, stop - eq - 1);
        pos = stop;
        int64_t n = 0;
        bool* seen = nullptr;
        if (key == "track") {
            seen = &has_track;
        } else if (key == "playlist") {
            seen = &has_playlist;
        } else if (key == "start") {
            seen = &has_start;
        } else if (key == "end") {
            seen = &has_end;
        } else {
            why = "session.consolidate: unknown key \"" + key + "\"";
            return false;
        }
        if (*seen) {
            why = "session.consolidate: \"" + key + "\" given twice";
            return false;
        }
        *seen = true;
        if (!detail::parse_decimal(value, key == "start" || key == "end", n)) {
            why = "session.consolidate: \"" + key + "=" + value + "\" is not a number";
            return false;
        }
        if (key == "track") a.track = static_cast<uint64_t>(n);
        if (key == "playlist") a.playlist = static_cast<uint64_t>(n);
        if (key == "start") a.start = n;
        if (key == "end") a.end = n;
    }
    if (!has_track || !has_start || !has_end) {
        why = "session.consolidate needs track=, start= and end=";
        return false;
    }
    if (a.track == 0) {
        why = "session.consolidate: track=0 is no track";
        return false;
    }
    if (a.start < 0 || a.end <= a.start) {
        why = "the range to consolidate is empty";
        return false;
    }
    out = std::move(a);
    return true;
}

}  // namespace sonora::api
