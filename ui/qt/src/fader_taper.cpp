// SPDX-License-Identifier: GPL-3.0-or-later
#include "fader_taper.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <charconv>
#include <cstdio>

namespace sonora::ui {

namespace {

struct TaperPoint {
    float db;
    float pos;
};

constexpr TaperPoint kTaper[] = {
    {kFaderSilenceDb, 0.00f}, {-60.0f, 0.08f}, {-40.0f, 0.20f}, {-30.0f, 0.30f},
    {-20.0f, 0.42f},          {-10.0f, 0.58f}, {-5.0f, 0.68f},  {0.0f, 0.78f},
    {6.0f, 0.90f},            {12.0f, 1.00f},
};
constexpr int kPoints = sizeof kTaper / sizeof kTaper[0];

}  // namespace

float fader_position_from_db(float db) {
    if (!(db > kTaper[0].db)) return 0.0f;  // also NaN
    if (db >= kTaper[kPoints - 1].db) return 1.0f;
    for (int i = 1; i < kPoints; ++i) {
        if (db <= kTaper[i].db) {
            const TaperPoint& a = kTaper[i - 1];
            const TaperPoint& b = kTaper[i];
            return a.pos + (db - a.db) / (b.db - a.db) * (b.pos - a.pos);
        }
    }
    return 1.0f;
}

float fader_db_from_position(float p) {
    if (!(p > 0.0f)) return kFaderSilenceDb;
    if (p >= 1.0f) return kTaper[kPoints - 1].db;
    for (int i = 1; i < kPoints; ++i) {
        if (p <= kTaper[i].pos) {
            const TaperPoint& a = kTaper[i - 1];
            const TaperPoint& b = kTaper[i];
            return a.db + (p - a.pos) / (b.pos - a.pos) * (b.db - a.db);
        }
    }
    return kTaper[kPoints - 1].db;
}

std::string gain_text(float db) {
    if (!(db > kFaderSilenceDb + 0.05f)) return "-inf";
    const float r = std::round(db * 10.0f) / 10.0f;
    char buf[16];
    if (r == 0.0f) return "0.0";
    std::snprintf(buf, sizeof buf, "%+.1f", static_cast<double>(r));
    return buf;
}

std::string pan_text(float pan) {
    const int v = static_cast<int>(std::lround(std::clamp(pan, -1.0f, 1.0f) * 100.0f));
    if (v == 0) return "C";
    return v < 0 ? "<" + std::to_string(-v) : std::to_string(v) + ">";
}

float fader_drag(float db, float dy, float travel, bool fine) {
    if (travel <= 0) return db;
    const float dp = -dy / travel * (fine ? 0.1f : 1.0f);
    // A gain past an end of the travel stays put until the drag heads back
    // onto it; the position of +18 dB is the top, and +18 is not +12.
    if (dp == 0.0f || (dp > 0.0f && db >= kFaderMaxDb) || (dp < 0.0f && db <= kFaderSilenceDb)) {
        return db;
    }
    return fader_db_from_position(std::clamp(fader_position_from_db(db) + dp, 0.0f, 1.0f));
}

namespace {

std::string trimmed_lower(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    return out;
}

// The whole of `s` as a number: "3x" and "" are not numbers. from_chars
// takes no leading '+', so it is skipped here.
bool whole_number(const std::string& s, float& out) {
    std::size_t at = 0;
    if (!s.empty() && s[0] == '+') at = 1;
    if (at == s.size()) return false;
    const char* first = s.data() + at;
    const char* last = s.data() + s.size();
    const auto r = std::from_chars(first, last, out);
    return r.ec == std::errc() && r.ptr == last && std::isfinite(out);
}

bool ends_with(const std::string& s, const char* suffix, std::string& head) {
    const std::string x(suffix);
    if (s.size() < x.size() || s.compare(s.size() - x.size(), x.size(), x) != 0) return false;
    head = s.substr(0, s.size() - x.size());
    return true;
}

}  // namespace

bool parse_gain_text(const std::string& text, float& db, std::string& why) {
    std::string s = trimmed_lower(text);
    std::string head;
    if (ends_with(s, "db", head)) s = head;
    if (s == "-inf" || s == "inf" || s == "-\xe2\x88\x9e") {
        db = kFaderSilenceDb;
        return true;
    }
    float v = 0.0f;
    if (!whole_number(s, v)) {
        why = "\"" + text + "\" is not a gain: type dB, like -6.5, or -inf";
        return false;
    }
    if (v > kFaderMaxDb) {
        why = "a fader goes from -inf to +12 dB; " + gain_text(v) + " is off the top";
        return false;
    }
    db = v <= kFaderSilenceDb ? kFaderSilenceDb : v;
    return true;
}

bool parse_pan_text(const std::string& text, float& pan, std::string& why) {
    const std::string s = trimmed_lower(text);
    std::string body = s;
    float sign = 1.0f;
    bool sided = false;
    std::string head;
    if (s == "c" || s == "<>" || s == "<0>") {
        pan = 0.0f;
        return true;
    }
    if (!s.empty() && (s[0] == '<' || s[0] == 'l')) {
        body = s.substr(1);
        sign = -1.0f;
        sided = true;
    } else if (ends_with(s, ">", head)) {
        body = head;
        sided = true;
    } else if (!s.empty() && s[0] == 'r') {
        body = s.substr(1);
        sided = true;
    }
    float v = 0.0f;
    if (!whole_number(body, v) || (sided && (body[0] == '-' || body[0] == '+'))) {
        why = "\"" + text + "\" is not a pan: type <30, 30>, L30, R30, -30 or C";
        return false;
    }
    v *= sign;
    if (v < -100.0f || v > 100.0f) {
        why = "a pan goes from 100 left to 100 right";
        return false;
    }
    pan = v / 100.0f;
    return true;
}

float fader_linear_gain(float db) {
    if (!(db > kFaderSilenceDb)) return 0.0f;
    return std::min(4.0f, std::pow(10.0f, db / 20.0f));
}

}  // namespace sonora::ui
