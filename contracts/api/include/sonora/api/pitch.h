// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pitch editing of audio (time.pitch, ADR-0143). No protocol bump, no new
// command: the edit is the verb kPitchVerb of the existing `sample.edit`, and
// its PARAM is the note list below. The engine renders a shifted copy and
// submits one Compound; nothing here reaches the audio thread.
//
//   PARAM      n=START:END:CENTS[:FORMANT_CENTS[:GAIN_DB]];n=...
//              START, END  frames relative to the region's start, START < END
//              CENTS       the note's shift, kPitchCentsMin..kPitchCentsMax
//              FORMANT     formant shift in cents, same range, default 0
//              GAIN_DB     kPitchGainDbMin..kPitchGainDbMax, default 0,
//                          kept to 0.01 dB
//   Capability kCapabilityPitch in Hello::capabilities, only when the engine
//              was built with a shifter; `sample.edit` alone does not imply it.
//
// The list has 1..kMaxPitchNotes notes in strictly increasing order that do
// not overlap (one may start where the previous ends). No space anywhere: the
// sample.edit line splits on spaces. Every refusal fills `why` with a reason
// the operator reads as a held notice. Whether the notes fit inside the
// region is the engine's check (pitch_notes_fit), since only it knows the
// region's length.
//
// Numbers are parsed and printed by hand, not with strtod/printf, so a
// locale with a decimal comma cannot change the wire.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace sonora::api {

inline constexpr const char* kCapabilityPitch = "pitch";
inline constexpr const char* kPitchVerb = "pitch";

inline constexpr std::size_t kMaxPitchNotes = 64;
inline constexpr int32_t kPitchCentsMin = -2400;
inline constexpr int32_t kPitchCentsMax = 2400;
inline constexpr double kPitchGainDbMin = -48.0;
inline constexpr double kPitchGainDbMax = 12.0;
// Frames beyond this are refused: far past any region, and the sums below
// cannot overflow.
inline constexpr uint64_t kPitchMaxFrame = uint64_t{1} << 48;
// Longest PARAM accepted. A valid spec formats to at most 53 characters a note
// ("n=" 15 ':' 15 ':' -2400 ':' -2400 ':' -47.99 ';'); the slack is for a
// sign or extra decimals typed by hand. Well under the wire's kMaxText.
inline constexpr std::size_t kMaxPitchParamChars = kMaxPitchNotes * 64;

struct PitchNote {
    uint64_t start = 0;
    uint64_t end = 0;
    int32_t cents = 0;
    int32_t formant_cents = 0;
    double gain_db = 0.0;
};

struct PitchEditSpec {
    std::vector<PitchNote> notes;
};

namespace detail {

inline bool pitch_refuse(std::string& why, const std::string& reason) {
    why = reason;
    return false;
}

// Digits only; refuses empty, a sign and anything above kPitchMaxFrame.
inline bool parse_pitch_frame(const std::string& s, uint64_t& out) {
    if (s.empty() || s.size() > 15) return false;
    uint64_t v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<uint64_t>(c - '0');
    }
    if (v > kPitchMaxFrame) return false;
    out = v;
    return true;
}

// [-|+]digits, at most 5 digits so the range check below sees every value.
inline bool parse_pitch_int(const std::string& s, int32_t& out) {
    std::size_t i = 0;
    bool neg = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) neg = s[i++] == '-';
    if (i == s.size() || s.size() - i > 5) return false;
    int32_t v = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
    }
    out = neg ? -v : v;
    return true;
}

// [-|+]digits[.digits] in hundredths, rounded half away from zero at the
// third decimal; at most 3 integer digits so the range check sees every value.
inline bool parse_pitch_centi(const std::string& s, int32_t& out) {
    std::size_t i = 0;
    bool neg = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) neg = s[i++] == '-';
    int32_t whole = 0;
    std::size_t digits = 0;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i, ++digits) {
        if (digits == 3) return false;
        whole = whole * 10 + (s[i] - '0');
    }
    if (digits == 0) return false;
    int32_t frac = 0;
    if (i < s.size() && s[i] == '.') {
        ++i;
        std::size_t fd = 0;
        int32_t third = 0;
        for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i, ++fd) {
            if (fd < 2) frac = frac * 10 + (s[i] - '0');
            else if (fd == 2) third = s[i] - '0';
        }
        if (fd == 0) return false;
        if (fd == 1) frac *= 10;
        if (third >= 5) ++frac;
    }
    if (i != s.size()) return false;
    const int32_t v = whole * 100 + frac;
    out = neg ? -v : v;
    return true;
}

// A gain parse would refuse (NaN, huge) maps to one it still refuses, so a
// bad spec never formats into a good PARAM and the cast below stays defined.
inline int32_t pitch_gain_centi(double db) {
    if (!(db >= -999.0 && db <= 999.0)) return 99999;
    const double scaled = db * 100.0;
    return static_cast<int32_t>(scaled < 0 ? scaled - 0.5 : scaled + 0.5);
}

inline std::string format_pitch_centi(int32_t centi) {
    std::string s = centi < 0 ? "-" : "";
    const int32_t a = centi < 0 ? -centi : centi;
    s += std::to_string(a / 100);
    const int32_t frac = a % 100;
    if (frac != 0) {
        s += '.';
        s += static_cast<char>('0' + frac / 10);
        if (frac % 10 != 0) s += static_cast<char>('0' + frac % 10);
    }
    return s;
}

inline std::vector<std::string> split_pitch(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::size_t from = 0;
    for (;;) {
        const std::size_t at = s.find(sep, from);
        out.push_back(s.substr(from, at == std::string::npos ? std::string::npos : at - from));
        if (at == std::string::npos) return out;
        from = at + 1;
    }
}

inline std::string pitch_note_label(std::size_t index) {
    return "note " + std::to_string(index + 1);
}

}  // namespace detail

// The checks parse applies, for a client that builds a spec by hand: count,
// order, overlap and every field's range.
inline bool validate_pitch_spec(const PitchEditSpec& spec, std::string& why) {
    if (spec.notes.empty()) return detail::pitch_refuse(why, "pitch: no notes to edit");
    if (spec.notes.size() > kMaxPitchNotes) {
        return detail::pitch_refuse(why, "pitch: " + std::to_string(spec.notes.size()) +
                                             " notes, at most " +
                                             std::to_string(kMaxPitchNotes));
    }
    for (std::size_t i = 0; i < spec.notes.size(); ++i) {
        const PitchNote& n = spec.notes[i];
        const std::string label = detail::pitch_note_label(i);
        if (n.end > kPitchMaxFrame) {
            return detail::pitch_refuse(why, "pitch: " + label + " ends past the last frame");
        }
        if (n.start >= n.end) {
            return detail::pitch_refuse(why, "pitch: " + label + " is empty (start not before end)");
        }
        if (i > 0) {
            const PitchNote& prev = spec.notes[i - 1];
            if (n.start <= prev.start) {
                return detail::pitch_refuse(why, "pitch: " + label +
                                                     " does not start after the note before it");
            }
            if (n.start < prev.end) {
                return detail::pitch_refuse(why, "pitch: " + label + " overlaps the note before it");
            }
        }
        if (n.cents < kPitchCentsMin || n.cents > kPitchCentsMax) {
            return detail::pitch_refuse(why, "pitch: " + label + " shift " + std::to_string(n.cents) +
                                                 " cents is outside -2400..2400");
        }
        if (n.formant_cents < kPitchCentsMin || n.formant_cents > kPitchCentsMax) {
            return detail::pitch_refuse(why, "pitch: " + label + " formant " +
                                                 std::to_string(n.formant_cents) +
                                                 " cents is outside -2400..2400");
        }
        // Written so a NaN fails it too.
        if (!(n.gain_db >= kPitchGainDbMin && n.gain_db <= kPitchGainDbMax)) {
            return detail::pitch_refuse(why, "pitch: " + label + " gain is outside -48..12 dB");
        }
    }
    return true;
}

// The engine's check against the region it edits: no note may end past it.
inline bool pitch_notes_fit(const PitchEditSpec& spec, uint64_t region_length, std::string& why) {
    for (std::size_t i = 0; i < spec.notes.size(); ++i) {
        if (spec.notes[i].end > region_length) {
            return detail::pitch_refuse(why, "pitch: " + detail::pitch_note_label(i) +
                                                 " ends past the region (" +
                                                 std::to_string(spec.notes[i].end) + " > " +
                                                 std::to_string(region_length) + " frames)");
        }
    }
    return true;
}

// Omits trailing fields that are zero, so the shortest form is canonical;
// gain prints to 0.01 dB.
inline std::string format_pitch_param(const PitchEditSpec& spec) {
    std::string s;
    for (std::size_t i = 0; i < spec.notes.size(); ++i) {
        const PitchNote& n = spec.notes[i];
        if (i != 0) s += ';';
        s += "n=" + std::to_string(n.start) + ':' + std::to_string(n.end) + ':' +
             std::to_string(n.cents);
        const int32_t gain = detail::pitch_gain_centi(n.gain_db);
        if (n.formant_cents != 0 || gain != 0) s += ':' + std::to_string(n.formant_cents);
        if (gain != 0) s += ':' + detail::format_pitch_centi(gain);
    }
    return s;
}

// On success `spec` holds the notes; on refusal it is left as it was and
// `why` says what is wrong.
inline bool parse_pitch_param(const std::string& text, PitchEditSpec& spec, std::string& why) {
    if (text.empty()) return detail::pitch_refuse(why, "pitch: no notes to edit");
    if (text.size() > kMaxPitchParamChars) {
        return detail::pitch_refuse(why, "pitch: note list is longer than " +
                                             std::to_string(kMaxPitchParamChars) + " characters");
    }
    const std::vector<std::string> items = detail::split_pitch(text, ';');
    if (items.size() > kMaxPitchNotes) {
        return detail::pitch_refuse(why, "pitch: " + std::to_string(items.size()) +
                                             " notes, at most " + std::to_string(kMaxPitchNotes));
    }
    PitchEditSpec out;
    out.notes.reserve(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        const std::string label = detail::pitch_note_label(i);
        const std::string& item = items[i];
        if (item.compare(0, 2, "n=") != 0) {
            return detail::pitch_refuse(why, "pitch: " + label + " does not start with n=");
        }
        const std::vector<std::string> f = detail::split_pitch(item.substr(2), ':');
        if (f.size() < 3 || f.size() > 5) {
            return detail::pitch_refuse(why, "pitch: " + label +
                                                 " needs START:END:CENTS[:FORMANT[:GAIN]]");
        }
        PitchNote n;
        if (!detail::parse_pitch_frame(f[0], n.start)) {
            return detail::pitch_refuse(why, "pitch: " + label + " start '" + f[0] +
                                                 "' is not a frame number");
        }
        if (!detail::parse_pitch_frame(f[1], n.end)) {
            return detail::pitch_refuse(why, "pitch: " + label + " end '" + f[1] +
                                                 "' is not a frame number");
        }
        if (!detail::parse_pitch_int(f[2], n.cents)) {
            return detail::pitch_refuse(why, "pitch: " + label + " shift '" + f[2] +
                                                 "' is not a whole number of cents");
        }
        if (f.size() > 3 && !detail::parse_pitch_int(f[3], n.formant_cents)) {
            return detail::pitch_refuse(why, "pitch: " + label + " formant '" + f[3] +
                                                 "' is not a whole number of cents");
        }
        if (f.size() > 4) {
            int32_t centi = 0;
            if (!detail::parse_pitch_centi(f[4], centi)) {
                return detail::pitch_refuse(why, "pitch: " + label + " gain '" + f[4] +
                                                     "' is not a number of dB");
            }
            n.gain_db = centi / 100.0;
        }
        out.notes.push_back(n);
    }
    if (!validate_pitch_spec(out, why)) return false;
    spec = std::move(out);
    return true;
}

}  // namespace sonora::api
