#include <cctype>
#include <cstdlib>
#include <cstring>

#include "sonora/loops/loop_scan.h"

namespace sonora::loops {
namespace {

std::uint32_t rd32(const std::string& b, std::size_t o) {
    return static_cast<std::uint8_t>(b[o]) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[o + 1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[o + 2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[o + 3])) << 24);
}
std::uint16_t rd16(const std::string& b, std::size_t o) {
    return static_cast<std::uint16_t>(static_cast<std::uint8_t>(b[o]) |
                                      (static_cast<std::uint8_t>(b[o + 1]) << 8));
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> split_tokens(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '_' || c == '-' || c == ' ' || c == '.') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// "120bpm" -> 120; returns 0 when the token is not a tempo.
double bpm_token(const std::string& tok) {
    std::string t = lower(tok);
    if (t.size() < 4 || t.compare(t.size() - 3, 3, "bpm") != 0) return 0.0;
    std::string num = t.substr(0, t.size() - 3);
    for (char c : num)
        if (!std::isdigit(static_cast<unsigned char>(c)) && c != '.') return 0.0;
    return num.empty() ? 0.0 : std::atof(num.c_str());
}

// "Cmin", "F#maj", "Am": note letter, optional #/b, optional maj/min/m.
bool key_token(const std::string& tok, std::string& key) {
    if (tok.empty() || tok[0] < 'A' || tok[0] > 'G') return false;
    std::size_t i = 1;
    if (i < tok.size() && (tok[i] == '#' || tok[i] == 'b')) ++i;
    std::string rest = lower(tok.substr(i));
    if (rest != "maj" && rest != "min" && rest != "m") return false;
    key = tok;
    return true;
}

// Minimal flat-object reader: string, number and string-array values only.
struct JsonCursor {
    const std::string& s;
    std::size_t i = 0;
    void ws() { while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; }
    bool lit(char c) { ws(); if (i < s.size() && s[i] == c) { ++i; return true; } return false; }
    bool str(std::string& out) {
        ws();
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        out.clear();
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) ++i;
            out += s[i++];
        }
        if (i >= s.size()) return false;
        ++i;
        return true;
    }
};

}  // namespace

void parse_filename_tokens(const std::string& stem, LoopEntry& e) {
    for (const auto& tok : split_tokens(stem)) {
        if (double b = bpm_token(tok); b > 0.0) {
            e.bpm = b;
            continue;
        }
        std::string k;
        if (e.key.empty() && key_token(tok, k)) e.key = k;
    }
}

bool parse_sidecar_json(const std::string& text, LoopEntry& e) {
    JsonCursor c{text};
    if (!c.lit('{')) return false;
    if (c.lit('}')) return true;
    do {
        std::string name;
        if (!c.str(name) || !c.lit(':')) return false;
        c.ws();
        if (c.i < text.size() && text[c.i] == '"') {
            std::string v;
            if (!c.str(v)) return false;
            if (name == "name") e.name = v;
            else if (name == "category") e.category = v;
            else if (name == "key") e.key = v;
        } else if (c.i < text.size() && text[c.i] == '[') {
            ++c.i;
            std::vector<std::string> tags;
            while (!c.lit(']')) {
                std::string v;
                if (!c.str(v)) return false;
                tags.push_back(v);
                c.lit(',');
            }
            if (name == "tags") e.tags = tags;
        } else {
            char* end = nullptr;
            double v = std::strtod(text.c_str() + c.i, &end);
            if (end == text.c_str() + c.i) return false;
            c.i = static_cast<std::size_t>(end - text.c_str());
            if (name == "bpm") e.bpm = v;
            else if (name == "beats") e.beats = static_cast<int>(v);
        }
    } while (c.lit(','));
    return c.lit('}');
}

bool parse_wav_header(const std::string& b, LoopEntry& e) {
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0 ||
        std::memcmp(b.data() + 8, "WAVE", 4) != 0)
        return false;
    std::uint16_t channels = 0, bits = 0;
    std::size_t pos = 12;
    while (pos + 8 <= b.size()) {
        std::uint32_t len = rd32(b, pos + 4);
        std::size_t body = pos + 8;
        if (std::memcmp(b.data() + pos, "fmt ", 4) == 0 && len >= 16 &&
            body + 16 <= b.size()) {
            channels = rd16(b, body + 2);
            e.sample_rate = rd32(b, body + 4);
            bits = rd16(b, body + 14);
        } else if (std::memcmp(b.data() + pos, "data", 4) == 0) {
            if (channels && bits >= 8) e.frames = len / (channels * (bits / 8u));
        } else if (std::memcmp(b.data() + pos, "acid", 4) == 0 && len >= 24 &&
                   body + 24 <= b.size()) {
            float tempo;
            std::uint32_t raw = rd32(b, body + 20);
            std::memcpy(&tempo, &raw, 4);
            if (tempo > 0.0f) e.bpm = tempo;
            std::uint32_t beats = rd32(b, body + 12);
            if (beats > 0 && beats < 100000) e.beats = static_cast<int>(beats);
        }
        pos = body + len + (len & 1u);
        if (len > b.size()) break;  // truncated read of a large data chunk
    }
    return true;
}

}  // namespace sonora::loops
