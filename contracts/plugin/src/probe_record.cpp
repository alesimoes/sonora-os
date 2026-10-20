// SPDX-License-Identifier: GPL-3.0-or-later
// The probe record's one codec (probe_record.h). A small strict JSON reader
// of its own: contracts depend on common/base only (ADR-0007), so
// common/json is not available here, and one codec on both sides of the
// boundary is what keeps the host and the engine from drifting apart.
#include "sonora/plugin/probe_record.h"

#include <charconv>
#include <initializer_list>
#include <cmath>
#include <memory>
#include <utility>

namespace sonora::plugin::probe {

namespace {

// ---- writing

void put_string(std::string& out, std::string_view s) {
    out += '"';
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    static const char kHex[] = "0123456789abcdef";
                    out += "\\u00";
                    out += kHex[c >> 4];
                    out += kHex[c & 15];
                } else {
                    out += ch;
                }
        }
    }
    out += '"';
}

void put_float(std::string& out, float v) {
    if (!std::isfinite(v)) v = 0.f;
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    std::string_view s(buf, std::size_t(r.ptr - buf));
    out += s;
    // A float field reads as one: "1" would be an integer lexeme.
    if (s.find_first_of(".eEn") == std::string_view::npos) out += ".0";
}

void put_uint(std::string& out, uint64_t v) { out += std::to_string(v); }

void key(std::string& out, const char* k, bool first = false) {
    if (!first) out += ',';
    out += '"';
    out += k;
    out += "\":";
}

const char* outcome_name(Outcome o) {
    switch (o) {
        case Outcome::Ok: return "ok";
        case Outcome::Refused: return "refused";
        case Outcome::NotInstalled: return "not_installed";
        case Outcome::Blocked: return "blocked";
    }
    return "refused";
}

// ---- reading: a DOM just big enough for a record

struct Node {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool boolean = false;
    std::string text;  // a string's content, a number's lexeme
    std::vector<Node> items;
    std::vector<std::pair<std::string, Node>> members;

    const Node* find(std::string_view k) const {
        for (const auto& [name, v] : members)
            if (name == k) return &v;
        return nullptr;
    }
};

class Parser {
public:
    Parser(std::string_view s, std::string& error) : s_(s), error_(error) {}

    bool document(Node& out) {
        if (!value(out, 0)) return false;
        skip_ws();
        if (i_ != s_.size()) return fail("text after the record");
        return true;
    }

private:
    bool fail(const std::string& why) {
        if (error_.empty()) error_ = why + " (at byte " + std::to_string(i_) + ")";
        return false;
    }
    void skip_ws() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
    }
    bool literal(std::string_view word) {
        if (s_.substr(i_, word.size()) != word) return fail("not a JSON value");
        i_ += word.size();
        return true;
    }

    bool value(Node& out, int depth) {
        // A record nests four deep (record, params, a param, its labels).
        if (depth > 8) return fail("nested too deep");
        skip_ws();
        if (i_ >= s_.size()) return fail("the record ends early");
        const char c = s_[i_];
        if (c == '{') return object(out, depth);
        if (c == '[') return array(out, depth);
        if (c == '"') {
            out.type = Node::Type::String;
            return string(out.text);
        }
        if (c == 't') {
            out.type = Node::Type::Bool;
            out.boolean = true;
            return literal("true");
        }
        if (c == 'f') {
            out.type = Node::Type::Bool;
            return literal("false");
        }
        if (c == 'n') return literal("null");
        return number(out);
    }

    bool object(Node& out, int depth) {
        out.type = Node::Type::Object;
        ++i_;
        skip_ws();
        if (i_ < s_.size() && s_[i_] == '}') {
            ++i_;
            return true;
        }
        for (;;) {
            skip_ws();
            std::string k;
            if (i_ >= s_.size() || s_[i_] != '"') return fail("an object key is not a string");
            if (!string(k)) return false;
            for (const auto& m : out.members)
                if (m.first == k) return fail("key \"" + k + "\" twice");
            skip_ws();
            if (i_ >= s_.size() || s_[i_] != ':') return fail("no ':' after a key");
            ++i_;
            out.members.emplace_back(std::move(k), Node{});
            if (!value(out.members.back().second, depth + 1)) return false;
            skip_ws();
            if (i_ < s_.size() && s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (i_ < s_.size() && s_[i_] == '}') {
                ++i_;
                return true;
            }
            return fail("an object is not closed");
        }
    }

    bool array(Node& out, int depth) {
        out.type = Node::Type::Array;
        ++i_;
        skip_ws();
        if (i_ < s_.size() && s_[i_] == ']') {
            ++i_;
            return true;
        }
        for (;;) {
            if (out.items.size() >= kMaxRecordParams) return fail("an array over the record's caps");
            out.items.emplace_back();
            if (!value(out.items.back(), depth + 1)) return false;
            skip_ws();
            if (i_ < s_.size() && s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (i_ < s_.size() && s_[i_] == ']') {
                ++i_;
                return true;
            }
            return fail("an array is not closed");
        }
    }

    static void utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out += char(cp);
        } else if (cp < 0x800) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(uint32_t& v) {
        if (i_ + 4 > s_.size()) return fail("a short \\u escape");
        v = 0;
        for (int k = 0; k < 4; ++k) {
            const char h = s_[i_++];
            v <<= 4;
            if (h >= '0' && h <= '9') v |= uint32_t(h - '0');
            else if (h >= 'a' && h <= 'f') v |= uint32_t(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') v |= uint32_t(h - 'A' + 10);
            else return fail("not a hex digit in a \\u escape");
        }
        return true;
    }

    bool string(std::string& out) {
        ++i_;  // the quote
        for (;;) {
            if (i_ >= s_.size()) return fail("a string is not closed");
            if (out.size() > kMaxRecordText) return fail("a string over " + std::to_string(kMaxRecordText) + " bytes");
            const char c = s_[i_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return fail("a control character in a string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i_ >= s_.size()) return fail("a string ends in an escape");
            const char e = s_[i_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00) {
                        uint32_t lo = 0;
                        if (s_.substr(i_, 2) != "\\u") return fail("a lone surrogate");
                        i_ += 2;
                        if (!hex4(lo) || lo < 0xDC00 || lo >= 0xE000) return fail("a lone surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp < 0xE000) {
                        return fail("a lone surrogate");
                    }
                    utf8(out, cp);
                    break;
                }
                default: return fail("an unknown escape");
            }
        }
    }

    bool number(Node& out) {
        const std::size_t start = i_;
        if (i_ < s_.size() && s_[i_] == '-') ++i_;
        const auto digits = [&] {
            const std::size_t from = i_;
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
            return i_ > from;
        };
        if (!digits()) return fail("not a JSON value");
        if (i_ < s_.size() && s_[i_] == '.') {
            ++i_;
            if (!digits()) return fail("a number with no digits after '.'");
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
            if (!digits()) return fail("a number with no exponent digits");
        }
        out.type = Node::Type::Number;
        out.text = std::string(s_.substr(start, i_ - start));
        return true;
    }

    std::string_view s_;
    std::string& error_;
    std::size_t i_ = 0;
};

// ---- reading a record from the DOM

struct Reader {
    std::string& error;

    bool fail(const std::string& why) {
        if (error.empty()) error = why;
        return false;
    }

    // Every key of `o` must be one of `known`.
    bool only(const Node& o, std::initializer_list<const char*> known, const char* what) {
        for (const auto& [k, v] : o.members) {
            bool found = false;
            for (const char* n : known) found = found || k == n;
            if (!found) return fail(std::string(what) + " has an unknown key \"" + k + "\"");
        }
        return true;
    }

    const Node* need(const Node& o, const char* k, Node::Type t, const char* what) {
        const Node* v = o.find(k);
        if (!v) {
            fail(std::string(what) + " has no \"" + k + "\"");
            return nullptr;
        }
        if (v->type != t) {
            fail(std::string(what) + "'s \"" + k + "\" is of the wrong type");
            return nullptr;
        }
        return v;
    }

    bool str(const Node& o, const char* k, std::string& out, const char* what) {
        const Node* v = need(o, k, Node::Type::String, what);
        if (!v) return false;
        out = v->text;
        return true;
    }

    bool u32(const Node& o, const char* k, uint32_t& out, const char* what) {
        const Node* v = need(o, k, Node::Type::Number, what);
        if (!v) return false;
        const char* b = v->text.data();
        const char* e = b + v->text.size();
        const auto r = std::from_chars(b, e, out);
        if (r.ec != std::errc() || r.ptr != e) return fail(std::string(what) + "'s \"" + k + "\" is not a 32-bit count");
        return true;
    }

    bool f32(const Node& o, const char* k, float& out, const char* what) {
        const Node* v = need(o, k, Node::Type::Number, what);
        if (!v) return false;
        const char* b = v->text.data();
        const char* e = b + v->text.size();
        const auto r = std::from_chars(b, e, out);
        if (r.ec != std::errc() || r.ptr != e || !std::isfinite(out))
            return fail(std::string(what) + "'s \"" + k + "\" is not a finite float");
        return true;
    }

    bool boolean(const Node& o, const char* k, bool& out, const char* what) {
        const Node* v = need(o, k, Node::Type::Bool, what);
        if (!v) return false;
        out = v->boolean;
        return true;
    }

    bool strings(const Node& o, const char* k, std::vector<std::string>& out, std::size_t cap, const char* what) {
        const Node* v = need(o, k, Node::Type::Array, what);
        if (!v) return false;
        if (v->items.size() > cap)
            return fail(std::string(what) + "'s \"" + k + "\" has more than " + std::to_string(cap) + " entries");
        out.clear();
        for (const Node& s : v->items) {
            if (s.type != Node::Type::String) return fail(std::string(what) + "'s \"" + k + "\" holds a non-string");
            out.push_back(s.text);
        }
        return true;
    }

    bool param(const Node& o, Param& p) {
        const char* w = "a parameter";
        if (o.type != Node::Type::Object) return fail("a parameter is not an object");
        if (!only(o, {"key", "symbol", "name", "min", "max", "default", "flags", "unit", "steps", "labels"}, w))
            return false;
        if (!str(o, "key", p.key, w) || !str(o, "symbol", p.symbol, w) || !str(o, "name", p.name, w) ||
            !f32(o, "min", p.min, w) || !f32(o, "max", p.max, w) || !f32(o, "default", p.default_value, w) ||
            !u32(o, "flags", p.flags, w) || !str(o, "unit", p.unit, w) || !u32(o, "steps", p.steps, w) ||
            !strings(o, "labels", p.labels, kMaxRecordLabels, w))
            return false;
        if (p.key.empty()) return fail("a parameter has an empty key");
        if ((p.flags & ~kKnownParamFlags) != 0) return fail("parameter " + p.key + " has unknown flags");
        if (p.steps == 1) return fail("parameter " + p.key + " has 1 step");
        if (!p.labels.empty() && p.labels.size() != p.steps)
            return fail("parameter " + p.key + " has " + std::to_string(p.labels.size()) + " labels for " +
                        std::to_string(p.steps) + " steps");
        return true;
    }

    bool record(const Node& o, Record& r) {
        const char* w = "the record";
        if (o.type != Node::Type::Object) return fail("the record is not a JSON object");
        // The version first: a record of another version is refused as
        // that, whatever else it holds.
        if (!u32(o, "record", r.record, w)) return false;
        if (r.record != kProbeRecordVersion)
            return fail("a probe record of version " + std::to_string(r.record) + ", not " +
                        std::to_string(kProbeRecordVersion));
        if (!only(o, {"record", "host", "uri", "format", "bundle", "name", "vendor", "version", "categories",
                      "has_editor", "audio_in", "audio_out", "sidechain_in", "ok", "reason", "channels", "replicas",
                      "latency_frames", "sample_rate", "outcome", "bypass", "params"},
                  w))
            return false;
        bool ok = false;
        std::string outcome, bypass;
        if (!str(o, "host", r.host, w) || !str(o, "uri", r.uri, w) || !str(o, "format", r.format, w) ||
            !str(o, "bundle", r.bundle, w) || !str(o, "name", r.name, w) || !str(o, "vendor", r.vendor, w) ||
            !str(o, "version", r.version, w) ||
            !strings(o, "categories", r.categories, kMaxRecordCategories, w) ||
            !boolean(o, "has_editor", r.has_editor, w) || !u32(o, "audio_in", r.audio_in, w) ||
            !u32(o, "audio_out", r.audio_out, w) || !u32(o, "sidechain_in", r.sidechain_in, w) ||
            !boolean(o, "ok", ok, w) || !u32(o, "channels", r.channels, w) || !u32(o, "replicas", r.replicas, w) ||
            !u32(o, "latency_frames", r.latency_frames, w) || !u32(o, "sample_rate", r.sample_rate, w) ||
            !str(o, "outcome", outcome, w) || !str(o, "bypass", bypass, w))
            return false;
        if (r.uri.empty()) return fail("the record names no plugin");
        if (r.format != "lv2" && r.format != "vst3") return fail("the record's format is \"" + r.format + "\"");
        if (outcome == "ok") r.outcome = Outcome::Ok;
        else if (outcome == "refused") r.outcome = Outcome::Refused;
        else if (outcome == "not_installed") r.outcome = Outcome::NotInstalled;
        else if (outcome == "blocked") r.outcome = Outcome::Blocked;
        else return fail("the record's outcome is \"" + outcome + "\"");
        if (ok != r.ok()) return fail("the record's ok and outcome disagree");
        r.reason.clear();
        if (const Node* reason = o.find("reason")) {
            if (ok) return fail("a usable record gives a reason");
            if (reason->type != Node::Type::String) return fail("the record's reason is not a string");
            r.reason = reason->text;
        } else if (!ok) {
            return fail("a refusal gives no reason");
        }
        if (bypass != "plugin" && bypass != "host") return fail("the record's bypass is \"" + bypass + "\"");
        r.plugin_bypass = bypass == "plugin";
        const Node* params = need(o, "params", Node::Type::Array, w);
        if (!params) return false;
        if (params->items.size() > kMaxRecordParams) return fail("the record has too many parameters");
        r.params.clear();
        r.params.reserve(params->items.size());
        for (const Node& p : params->items) {
            r.params.emplace_back();
            if (!param(p, r.params.back())) return false;
        }
        return true;
    }
};

}  // namespace

std::string encode(const Record& r) {
    std::string out = "{";
    key(out, "record", true);
    put_uint(out, kProbeRecordVersion);
    key(out, "host");
    put_string(out, r.host);
    key(out, "uri");
    put_string(out, r.uri);
    key(out, "format");
    put_string(out, r.format);
    key(out, "bundle");
    put_string(out, r.bundle);
    key(out, "name");
    put_string(out, r.name);
    key(out, "vendor");
    put_string(out, r.vendor);
    key(out, "version");
    put_string(out, r.version);
    key(out, "categories");
    out += '[';
    for (std::size_t i = 0; i < r.categories.size(); ++i) {
        if (i) out += ',';
        put_string(out, r.categories[i]);
    }
    out += ']';
    key(out, "has_editor");
    out += r.has_editor ? "true" : "false";
    key(out, "audio_in");
    put_uint(out, r.audio_in);
    key(out, "audio_out");
    put_uint(out, r.audio_out);
    key(out, "sidechain_in");
    put_uint(out, r.sidechain_in);
    key(out, "ok");
    out += r.ok() ? "true" : "false";
    if (!r.ok()) {
        key(out, "reason");
        put_string(out, r.reason);
    }
    key(out, "channels");
    put_uint(out, r.channels);
    key(out, "replicas");
    put_uint(out, r.replicas);
    key(out, "latency_frames");
    put_uint(out, r.latency_frames);
    key(out, "sample_rate");
    put_uint(out, r.sample_rate);
    key(out, "outcome");
    put_string(out, outcome_name(r.outcome));
    key(out, "bypass");
    put_string(out, r.plugin_bypass ? "plugin" : "host");
    key(out, "params");
    out += '[';
    for (std::size_t i = 0; i < r.params.size(); ++i) {
        const Param& p = r.params[i];
        if (i) out += ',';
        out += '{';
        key(out, "key", true);
        put_string(out, p.key);
        key(out, "symbol");
        put_string(out, p.symbol);
        key(out, "name");
        put_string(out, p.name);
        key(out, "min");
        put_float(out, p.min);
        key(out, "max");
        put_float(out, p.max);
        key(out, "default");
        put_float(out, p.default_value);
        key(out, "flags");
        put_uint(out, p.flags);
        key(out, "unit");
        put_string(out, p.unit);
        key(out, "steps");
        put_uint(out, p.steps);
        key(out, "labels");
        out += '[';
        for (std::size_t k = 0; k < p.labels.size(); ++k) {
            if (k) out += ',';
            put_string(out, p.labels[k]);
        }
        out += "]}";
    }
    out += "]}";
    return out;
}

bool decode(std::string_view line, Record& out, std::string& error) {
    error.clear();
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.remove_suffix(1);
    if (line.size() > kMaxRecordBytes) {
        error = "a probe record of " + std::to_string(line.size()) + " bytes, over the cap of " +
                std::to_string(kMaxRecordBytes);
        return false;
    }
    Node root;
    if (!Parser(line, error).document(root)) return false;
    Record r;
    Reader rd{error};
    if (!rd.record(root, r)) return false;
    out = std::move(r);
    return true;
}

bool parse_probe_version(std::string_view line, uint32_t& probe) {
    // Space-separated "key value" pairs after the program name, as
    // protocol.h's grammar has them.
    std::size_t i = line.find(' ');
    bool found = false;
    while (i != std::string_view::npos && i < line.size()) {
        const std::size_t ks = line.find_first_not_of(' ', i);
        if (ks == std::string_view::npos) break;
        const std::size_t ke = line.find(' ', ks);
        if (ke == std::string_view::npos) break;
        const std::size_t vs = line.find_first_not_of(' ', ke);
        if (vs == std::string_view::npos) break;
        std::size_t ve = line.find_first_of(" \n", vs);
        if (ve == std::string_view::npos) ve = line.size();
        if (line.substr(ks, ke - ks) == "probe") {
            if (found) return false;
            uint32_t v = 0;
            const auto r = std::from_chars(line.data() + vs, line.data() + ve, v);
            if (r.ec != std::errc() || r.ptr != line.data() + ve) return false;
            probe = v;
            found = true;
        }
        i = ve;
    }
    return found;
}

}  // namespace sonora::plugin::probe
