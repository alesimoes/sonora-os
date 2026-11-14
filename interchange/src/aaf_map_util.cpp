// SPDX-License-Identifier: GPL-3.0-or-later
#include "aaf_map_util.h"

#include <cctype>
#include <variant>

namespace sonora::interchange::aaf_detail {

bool valid_utf8(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x20) return false;
        std::size_t extra = 0;
        if (c >= 0x80) {
            if ((c >> 5) == 6) extra = 1;
            else if ((c >> 4) == 14) extra = 2;
            else if ((c >> 3) == 30) extra = 3;
            else return false;
        }
        if (extra != 0 && i + extra >= s.size()) return false;
        for (std::size_t k = 1; k <= extra; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        }
        i += extra + 1;
    }
    return true;
}

// 64 lower-case hex digits derived from `seed`, the same every run.
std::string generated_umid(const std::string& seed) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (std::uint64_t k = 0; k < 4; ++k) {
        std::uint64_t h = 1469598103934665603ULL ^ (k * 0x9E3779B97F4A7C15ULL);
        for (const char c : seed) h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
        for (int s = 60; s >= 0; s -= 4) out += digits[(h >> s) & 0xF];
    }
    return out;
}

// The "urn:smpte:umid:" prefix, dots, dashes and colons are formatting; what
// is left must be 64 hex digits.
std::string umid_from(const std::string& mob_id) {
    std::string lower;
    for (const char c : mob_id) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::string prefix = "urn:smpte:umid:";
    if (lower.compare(0, prefix.size(), prefix) == 0) lower.erase(0, prefix.size());
    std::string out;
    for (const char c : lower) {
        if (c == '.' || c == '-' || c == ':') continue;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return {};
        out += c;
    }
    return out.size() == 64 ? out : std::string();
}

bool path_ok(const std::string& p) {
    if (p.empty() || p.front() == '/' || p.find(':') != std::string::npos || !valid_utf8(p)) {
        return false;
    }
    std::size_t start = 0;
    for (;;) {
        const std::size_t slash = p.find('/', start);
        const std::string part =
            p.substr(start, slash == std::string::npos ? slash : slash - start);
        if (part.empty() || part == "." || part == "..") return false;
        if (slash == std::string::npos) return true;
        start = slash + 1;
    }
}

std::int64_t length_of(const AafComponent& c) {
    if (const auto* s = std::get_if<AafSourceClip>(&c)) return s->length;
    if (const auto* f = std::get_if<AafFiller>(&c)) return f->length;
    return 0;
}

}  // namespace sonora::interchange::aaf_detail
