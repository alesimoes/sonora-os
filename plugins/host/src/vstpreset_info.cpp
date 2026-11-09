// SPDX-License-Identifier: GPL-3.0-or-later
// The .vstpreset Info chunk, scanned for its Name attribute (ADR-0144): a
// bounded forward scan of untrusted XML, not a parser. The chunk is found
// and read by vstpreset.cpp (load_info).
#include "vstpreset.h"

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace sonora::plugin_host::vstpreset {

namespace {

bool at_word(std::string_view s, std::size_t at, std::string_view w) {
    return at <= s.size() && s.size() - at >= w.size() && s.substr(at, w.size()) == w;
}

bool xml_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

bool name_char(char c) {
    const auto u = static_cast<unsigned char>(c);
    return std::isalnum(u) || c == '_' || c == '-' || c == '.' || c == ':' || u >= 0x80;
}

void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// "#65" or "#x41": a Unicode scalar value, never NUL or a surrogate.
bool char_ref(std::string_view ref, uint32_t& cp) {
    const bool hex = ref.size() > 1 && ref[1] == 'x';
    const std::string_view digits = ref.substr(hex ? 2 : 1);
    if (digits.empty() || digits.size() > 7) return false;
    cp = 0;
    for (const char c : digits) {
        const auto u = static_cast<unsigned char>(c);
        if (hex ? !std::isxdigit(u) : !std::isdigit(u)) return false;
        cp = cp * (hex ? 16 : 10) + uint32_t(std::isdigit(u) ? c - '0' : std::toupper(u) - 'A' + 10);
    }
    return cp != 0 && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF);
}

// An attribute value as XML reads it: the predefined entities and character
// references, whitespace normalised to spaces. Any other entity is refused,
// never looked up, so a declared one cannot expand.
bool decode_value(std::string_view v, std::string& out) {
    out.clear();
    for (std::size_t i = 0; i < v.size();) {
        const char c = v[i];
        if (c == '<') return false;
        if (c != '&') {
            out += xml_space(c) ? ' ' : c;
            ++i;
            continue;
        }
        const std::size_t end = v.find(';', i);
        if (end == std::string_view::npos || end - i > 10) return false;
        const std::string_view ent = v.substr(i + 1, end - i - 1);
        uint32_t cp = 0;
        if (ent == "amp") out += '&';
        else if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (!ent.empty() && ent[0] == '#' && char_ref(ent, cp)) append_utf8(out, cp);
        else return false;
        i = end + 1;
    }
    return true;
}

// Strict UTF-8 (no overlong form, no surrogate, nothing past U+10FFFF) with
// no control character: a name the list shows as it is.
bool printable_utf8(std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        const auto b = static_cast<unsigned char>(s[i]);
        if (b < 0x80) {
            if (b < 0x20 || b == 0x7F) return false;
            ++i;
            continue;
        }
        const std::size_t n = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC2 ? 2 : 0;
        if (n == 0 || b > 0xF4 || s.size() - i < n) return false;
        uint32_t cp = b & (n == 2 ? 0x1F : n == 3 ? 0x0F : 0x07);
        for (std::size_t k = 1; k < n; ++k) {
            const auto t = static_cast<unsigned char>(s[i + k]);
            if ((t & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (t & 0x3F);
        }
        if ((n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF) ||
            (cp >= 0x80 && cp <= 0x9F))
            return false;
        i += n;
    }
    return true;
}

std::string_view trimmed(std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    return s;
}

// One start tag from `i` (at its '<'): its name, and the raw `id` and
// `value` attributes; `i` ends past its '>'. False for a malformed tag.
struct Tag {
    std::string_view name, id, value;
    bool has_id = false, has_value = false;
};

bool scan_tag(std::string_view x, std::size_t& i, Tag& t) {
    std::size_t j = i + 1;
    const std::size_t start = j;
    while (j < x.size() && name_char(x[j])) ++j;
    t = Tag{};
    t.name = x.substr(start, j - start);
    if (t.name.empty()) return false;
    for (;;) {
        while (j < x.size() && xml_space(x[j])) ++j;
        if (j >= x.size()) return false;
        if (x[j] == '>') break;
        if (x[j] == '/') {
            if (!at_word(x, j, "/>")) return false;
            ++j;
            break;
        }
        const std::size_t a = j;
        while (j < x.size() && name_char(x[j])) ++j;
        const std::string_view attr = x.substr(a, j - a);
        while (j < x.size() && xml_space(x[j])) ++j;
        if (attr.empty() || j >= x.size() || x[j] != '=') return false;
        ++j;
        while (j < x.size() && xml_space(x[j])) ++j;
        if (j >= x.size() || (x[j] != '"' && x[j] != '\'')) return false;
        const std::size_t close = x.find(x[j], j + 1);
        if (close == std::string_view::npos) return false;
        const std::string_view value = x.substr(j + 1, close - j - 1);
        j = close + 1;
        if (attr == "id" || attr == "value") {
            bool& has = attr == "id" ? t.has_id : t.has_value;
            if (has) return false;
            has = true;
            (attr == "id" ? t.id : t.value) = value;
        }
    }
    i = j + 1;
    return true;
}

// Past a comment, CDATA section or processing instruction at `i`; false when
// it never closes. `i` is unchanged when none starts there.
bool skip_markup(std::string_view x, std::size_t& i, bool& skipped) {
    static constexpr std::string_view kMarkup[][2] = {{"<!--", "-->"}, {"<![CDATA[", "]]>"}, {"<?", "?>"}};
    skipped = false;
    for (const auto& m : kMarkup) {
        const std::string_view open = m[0], close = m[1];
        if (!at_word(x, i, open)) continue;
        const std::size_t end = x.find(close, i + open.size());
        if (end == std::string_view::npos) return false;
        i = end + close.size();
        skipped = true;
        return true;
    }
    return true;
}

}  // namespace

bool info_name(std::string_view x, std::string& name) {
    if (x.size() > kMaxInfoBytes) return false;
    std::string found, id;
    bool has = false;
    for (std::size_t i = x.find('<'); i != std::string_view::npos; i = x.find('<', i)) {
        bool skipped = false;
        if (!skip_markup(x, i, skipped)) return false;
        if (skipped) continue;
        // A DOCTYPE, ENTITY or any other declaration: refused, not expanded.
        if (at_word(x, i, "<!")) return false;
        if (at_word(x, i, "</")) {
            const std::size_t end = x.find('>', i);
            if (end == std::string_view::npos) return false;
            i = end + 1;
            continue;
        }
        Tag t;
        if (!scan_tag(x, i, t)) return false;
        if (t.name != "Attr" || !t.has_id || !decode_value(t.id, id) || id != "Name") continue;
        if (has || !t.has_value || !decode_value(t.value, found)) return false;
        has = true;
    }
    if (!has || !printable_utf8(found)) return false;
    std::string_view n = trimmed(found);
    if (n.size() > kMaxNameBytes) {
        std::size_t cut = kMaxNameBytes;
        while (cut > 0 && (static_cast<unsigned char>(n[cut]) & 0xC0) == 0x80) --cut;
        n = trimmed(n.substr(0, cut));
    }
    if (n.empty()) return false;
    name = std::string(n);
    return true;
}

}  // namespace sonora::plugin_host::vstpreset
