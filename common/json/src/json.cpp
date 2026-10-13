// SPDX-License-Identifier: GPL-3.0-or-later
//
// Strict JSON, written rather than vendored. The keymap loader started this
// parser (ADR-0002 froze the package set, and a general library reports
// "parse error at byte 412" where keymaps/README.md and session-format.md
// require a line and a key path). ADR-0009 §5 moved it here so the session
// file and the key maps share one dialect instead of drifting into two.

#include "sonora/json/json.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <system_error>
#include <unordered_set>

namespace sonora::json {

namespace {

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// The JSON number grammar, over a whole lexeme. The writer checks a
// built number against it, so a non-finite float cannot reach a file.
bool valid_number_lexeme(std::string_view s) {
    std::size_t i = 0;
    if (i < s.size() && s[i] == '-') ++i;
    if (i >= s.size() || !is_digit(s[i])) return false;
    if (s[i] == '0') {
        ++i;
        if (i < s.size() && is_digit(s[i])) return false;
    } else {
        while (i < s.size() && is_digit(s[i])) ++i;
    }
    if (i < s.size() && s[i] == '.') {
        ++i;
        if (i >= s.size() || !is_digit(s[i])) return false;
        while (i < s.size() && is_digit(s[i])) ++i;
    }
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
        if (i >= s.size() || !is_digit(s[i])) return false;
        while (i < s.size() && is_digit(s[i])) ++i;
    }
    return i == s.size();
}

// from_chars reports overflow and underflow with the same errc. Telling them
// apart needs the number's decimal order: the power of ten of its first
// significant digit. Positive means too large; negative means too small,
// which is a zero, not an error.
bool order_is_positive(std::string_view s) {
    std::size_t i = 0;
    if (i < s.size() && s[i] == '-') ++i;
    long long order = 0;
    bool found = false;
    std::size_t int_digits = 0;
    while (i < s.size() && is_digit(s[i])) {
        if (!found && s[i] != '0') {
            found = true;
            order = 0;
        }
        if (found) ++int_digits;
        ++i;
    }
    if (found) order = static_cast<long long>(int_digits) - 1;
    if (i < s.size() && s[i] == '.') {
        ++i;
        long long zeros = 0;
        while (i < s.size() && is_digit(s[i])) {
            if (!found) {
                if (s[i] != '0') {
                    found = true;
                    order = -(zeros + 1);
                } else {
                    ++zeros;
                }
            }
            ++i;
        }
    }
    if (!found) return false;  // zero
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        bool neg = false;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
            neg = s[i] == '-';
            ++i;
        }
        long long exp = 0;
        // Saturates: an exponent with more digits than a long long holds is
        // far past either end of a double's range anyway.
        while (i < s.size() && is_digit(s[i])) {
            if (exp < 1'000'000'000) exp = exp * 10 + (s[i] - '0');
            ++i;
        }
        order += neg ? -exp : exp;
    }
    return order > 0;
}

template <typename F>
bool parse_floating(std::string_view s, F& out) {
    F v{};
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v,
                                   std::chars_format::general);
    if (r.ptr != s.data() + s.size()) return false;
    if (r.ec == std::errc::result_out_of_range) {
        if (order_is_positive(s)) return false;
        out = (!s.empty() && s[0] == '-') ? -F(0) : F(0);
        return true;
    }
    if (r.ec != std::errc()) return false;
    out = v;
    return true;
}

// Returns the length of a valid UTF-8 sequence starting at s[i], or 0.
// Overlong forms, surrogates and code points past U+10FFFF are not UTF-8.
std::size_t utf8_sequence(std::string_view s, std::size_t i) {
    const auto b = [&](std::size_t k) {
        return static_cast<unsigned char>(s[k]);
    };
    const unsigned c = b(i);
    if (c < 0x80) return 1;
    std::size_t n;
    unsigned cp;
    if (c >= 0xC2 && c <= 0xDF) {
        n = 2;
        cp = c & 0x1F;
    } else if (c >= 0xE0 && c <= 0xEF) {
        n = 3;
        cp = c & 0x0F;
    } else if (c >= 0xF0 && c <= 0xF4) {
        n = 4;
        cp = c & 0x07;
    } else {
        return 0;
    }
    if (i + n > s.size()) return 0;
    for (std::size_t k = 1; k < n; ++k) {
        if ((b(i + k) & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (b(i + k) & 0x3F);
    }
    if (n == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) return 0;
    if (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)) return 0;
    return n;
}

}  // namespace

bool valid_utf8(std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        const std::size_t n = utf8_sequence(s, i);
        if (n == 0) return false;
        i += n;
    }
    return true;
}

// --- Value -----------------------------------------------------------------

Value Value::boolean(bool b) {
    Value v;
    v.type_ = Type::Bool;
    v.boolean_ = b;
    return v;
}

Value Value::string(std::string s) {
    Value v;
    v.type_ = Type::String;
    v.text_ = std::move(s);
    return v;
}

Value Value::array() {
    Value v;
    v.type_ = Type::Array;
    return v;
}

Value Value::object() {
    Value v;
    v.type_ = Type::Object;
    return v;
}

Value Value::integer(std::int64_t n) {
    char buf[24];
    const auto r = std::to_chars(buf, buf + sizeof buf, n);
    return number_lexeme(std::string(buf, r.ptr));
}

Value Value::number_float(float f) {
    if (!std::isfinite(f)) return number_lexeme(std::isnan(f) ? "nan" : "inf");
    char buf[48];
    const auto r = std::to_chars(buf, buf + sizeof buf, f);
    std::string s(buf, r.ptr);
    // "0" or "-3" would read back fine, but a float field written as an
    // integer looks like an integer field to anyone reading the file.
    if (s.find_first_of(".e") == std::string::npos) s += ".0";
    return number_lexeme(std::move(s));
}

Value Value::number_lexeme(std::string lexeme) {
    Value v;
    v.type_ = Type::Number;
    v.text_ = std::move(lexeme);
    return v;
}

const Value* Value::find(std::string_view key) const {
    for (const auto& kv : members_) {
        if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

Value& Value::push(Value v) {
    items_.push_back(std::move(v));
    return items_.back();
}

Value& Value::set(std::string key, Value v) {
    members_.emplace_back(std::move(key), std::move(v));
    return members_.back().second;
}

bool Value::is_integer_lexeme() const {
    return type_ == Type::Number &&
           text_.find_first_of(".eE") == std::string::npos;
}

bool Value::to_int64(std::int64_t& out) const {
    if (!is_integer_lexeme()) return false;
    std::int64_t v = 0;
    const auto r = std::from_chars(text_.data(), text_.data() + text_.size(), v);
    if (r.ec != std::errc() || r.ptr != text_.data() + text_.size()) return false;
    out = v;
    return true;
}

bool Value::to_float(float& out) const {
    return type_ == Type::Number && parse_floating(text_, out);
}

bool Value::to_double(double& out) const {
    return type_ == Type::Number && parse_floating(text_, out);
}

bool Value::operator==(const Value& o) const {
    return type_ == o.type_ && boolean_ == o.boolean_ && text_ == o.text_ &&
           items_ == o.items_ && members_ == o.members_;
}

// --- Parser ----------------------------------------------------------------

class Parser {
public:
    Parser(std::string_view text, const Limits& limits)
        : text_(text), limits_(limits) {}

    bool parse(Value& out) {
        if (text_.size() > limits_.max_input_bytes) {
            return fail("the input is " + std::to_string(text_.size()) +
                        " bytes, over the cap of " +
                        std::to_string(limits_.max_input_bytes) + " bytes");
        }
        if (text_.size() >= 3 && text_.compare(0, 3, "\xEF\xBB\xBF") == 0) {
            return fail("a UTF-8 byte-order mark at the start; save the file"
                        " without one");
        }
        skip_space();
        if (!value(out)) return false;
        skip_space();
        if (pos_ != text_.size()) return fail("trailing content after the value");
        return true;
    }

    const Error& error() const { return error_; }

private:
    bool fail(const std::string& msg) {
        if (error_.message.empty()) {
            error_.message = msg;
            error_.line = line_;
            error_.column = static_cast<int>(pos_ - line_start_) + 1;
        }
        return false;
    }

    void skip_space() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == '\n') {
                ++line_;
                ++pos_;
                line_start_ = pos_;
            } else if (c == ' ' || c == '\t' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool eof() const { return pos_ >= text_.size(); }
    char peek() const { return text_[pos_]; }

    bool literal(const char* word) {
        const std::size_t n = std::strlen(word);
        if (text_.compare(pos_, n, word) != 0) return false;
        pos_ += n;
        return true;
    }

    // RAII rather than a decrement before each of value()'s many returns,
    // which is where a depth counter usually goes wrong.
    class Nest {
    public:
        explicit Nest(int& depth) : depth_(depth) { ++depth_; }
        ~Nest() { --depth_; }
        Nest(const Nest&) = delete;
        Nest& operator=(const Nest&) = delete;

    private:
        int& depth_;
    };

    bool account(std::size_t bytes) {
        ++values_;
        if (values_ > limits_.max_values) {
            return fail("more than " + std::to_string(limits_.max_values) +
                        " values, the cap");
        }
        dom_bytes_ += sizeof(Value) + bytes;
        if (dom_bytes_ > limits_.max_dom_bytes) {
            return fail("the document needs more than " +
                        std::to_string(limits_.max_dom_bytes) +
                        " bytes of memory, the cap");
        }
        return true;
    }

    bool value(Value& out) {
        if (eof()) return fail("unexpected end of input where a value was expected");
        const Nest nest(depth_);
        // The recursive descent below would otherwise run out of stack on a
        // hostile file, and a signal tells the operator nothing.
        if (depth_ > limits_.max_depth) {
            return fail("nesting deeper than " + std::to_string(limits_.max_depth) +
                        " levels");
        }
        if (!account(0)) return false;
        out.line_ = line_;
        out.column_ = static_cast<int>(pos_ - line_start_) + 1;
        const char c = peek();
        if (c == '{') return object(out);
        if (c == '[') return array(out);
        if (c == '"') {
            out.type_ = Value::Type::String;
            if (!string(out.text_)) return false;
            return account_bytes(out.text_.size());
        }
        if (c == 't') {
            if (!literal("true")) return fail("expected true");
            out.type_ = Value::Type::Bool;
            out.boolean_ = true;
            return true;
        }
        if (c == 'f') {
            if (!literal("false")) return fail("expected false");
            out.type_ = Value::Type::Bool;
            out.boolean_ = false;
            return true;
        }
        if (c == 'n') {
            if (!literal("null")) return fail("expected null");
            out.type_ = Value::Type::Null;
            return true;
        }
        if (c == '-' || is_digit(c)) return number(out);
        if (static_cast<unsigned char>(c) < 0x20 ||
            static_cast<unsigned char>(c) >= 0x7F) {
            char hex[8];
            std::snprintf(hex, sizeof hex, "0x%02x",
                          static_cast<unsigned>(static_cast<unsigned char>(c)));
            return fail(std::string("unexpected byte ") + hex +
                        " where a value was expected");
        }
        return fail(std::string("unexpected character '") + c +
                    "' where a value was expected");
    }

    bool account_bytes(std::size_t bytes) {
        dom_bytes_ += bytes;
        if (dom_bytes_ > limits_.max_dom_bytes) {
            return fail("the document needs more than " +
                        std::to_string(limits_.max_dom_bytes) +
                        " bytes of memory, the cap");
        }
        return true;
    }

    bool object(Value& out) {
        out.type_ = Value::Type::Object;
        ++pos_;  // '{'
        skip_space();
        if (!eof() && peek() == '}') {
            ++pos_;
            return true;
        }
        // A linear scan per key is quadratic in a hostile object with a
        // million members; past a handful, a set keeps it linear.
        std::unordered_set<std::string> seen;
        for (;;) {
            skip_space();
            if (eof() || peek() != '"') return fail("expected a quoted member name");
            std::string key;
            if (!string(key)) return false;
            // Checked before the value is read, while line_ is still the
            // key's line. Afterwards it is wherever a multi-line value ended,
            // and the message points at a closing brace.
            bool duplicate = false;
            if (out.members_.size() < 16) {
                for (const auto& kv : out.members_) {
                    if (kv.first == key) duplicate = true;
                }
            } else {
                if (seen.empty()) {
                    for (const auto& kv : out.members_) seen.insert(kv.first);
                }
                duplicate = !seen.insert(key).second;
            }
            if (duplicate) return fail("duplicate member \"" + key + "\"");
            if (!account_bytes(key.size())) return false;
            skip_space();
            if (eof() || peek() != ':') {
                return fail("expected ':' after member \"" + key + "\"");
            }
            ++pos_;
            skip_space();
            Value v;
            if (!value(v)) return false;
            out.members_.emplace_back(std::move(key), std::move(v));
            skip_space();
            if (eof()) return fail("unterminated object");
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == '}') {
                ++pos_;
                return true;
            }
            return fail(std::string("expected ',' or '}' in an object, got '") +
                        peek() + "'");
        }
    }

    bool array(Value& out) {
        out.type_ = Value::Type::Array;
        ++pos_;  // '['
        skip_space();
        if (!eof() && peek() == ']') {
            ++pos_;
            return true;
        }
        for (;;) {
            skip_space();
            Value v;
            if (!value(v)) return false;
            out.items_.push_back(std::move(v));
            skip_space();
            if (eof()) return fail("unterminated array");
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == ']') {
                ++pos_;
                return true;
            }
            return fail(std::string("expected ',' or ']' in an array, got '") +
                        peek() + "'");
        }
    }

    bool string(std::string& out) {
        ++pos_;  // '"'
        out.clear();
        for (;;) {
            if (eof()) return fail("unterminated string");
            const char c = text_[pos_];
            if (c == '"') {
                ++pos_;
                return true;
            }
            if (c == '\n') return fail("a newline inside a string");
            // RFC 8259 requires these escaped. In a hand-edited file a raw
            // one is nearly always a tab the editor inserted and nobody can
            // see, which is a bad thing to have inside a key name.
            if (static_cast<unsigned char>(c) < 0x20) {
                char hex[8];
                std::snprintf(hex, sizeof hex, "0x%02x",
                              static_cast<unsigned>(static_cast<unsigned char>(c)));
                return fail(std::string("a raw control character (") + hex +
                            ") inside a string; write it as an escape");
            }
            if (static_cast<unsigned char>(c) >= 0x80) {
                const std::size_t n = utf8_sequence(text_, pos_);
                if (n == 0) return fail("a string that is not valid UTF-8");
                out.append(text_.data() + pos_, n);
                pos_ += n;
                continue;
            }
            ++pos_;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (eof()) return fail("unterminated escape sequence");
            const char e = text_[pos_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    unsigned cp = 0;
                    if (!hex4(cp)) return false;
                    // A character outside the BMP is written as a surrogate
                    // pair, so decoding the pair is the only way to accept a
                    // name with an emoji in it. A surrogate on its own is
                    // not a character at all, and is refused rather than
                    // encoded into the ill-formed UTF-8 that CESU-8 would be.
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (text_.compare(pos_, 2, "\\u") != 0) {
                            return fail("a leading UTF-16 surrogate escape with"
                                        " no trailing surrogate after it");
                        }
                        pos_ += 2;
                        unsigned low = 0;
                        if (!hex4(low)) return false;
                        if (low < 0xDC00 || low > 0xDFFF) {
                            return fail("a leading UTF-16 surrogate escape"
                                        " followed by an escape that is not a"
                                        " trailing surrogate");
                        }
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return fail("a trailing UTF-16 surrogate escape with no"
                                    " leading surrogate before it");
                    }
                    append_utf8(cp, out);
                    break;
                }
                default:
                    return fail(std::string("unknown escape \\") + e);
            }
        }
    }

    bool hex4(unsigned& cp) {
        cp = 0;
        for (int i = 0; i < 4; ++i) {
            if (eof()) return fail("truncated \\u escape");
            const char h = text_[pos_++];
            unsigned d;
            if (h >= '0' && h <= '9') d = static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f') d = static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') d = static_cast<unsigned>(h - 'A' + 10);
            else return fail("a non-hex digit in a \\u escape");
            cp = cp * 16 + d;
        }
        return true;
    }

    static void append_utf8(unsigned cp, std::string& out) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool number(Value& out) {
        const std::size_t start = pos_;
        if (!eof() && peek() == '-') ++pos_;
        if (eof() || !is_digit(peek())) return fail("a malformed number");
        // JSON has no octal and no padding. A reader that takes "01" as 1
        // loads a file every other JSON tool refuses.
        if (peek() == '0' && pos_ + 1 < text_.size() && is_digit(text_[pos_ + 1])) {
            return fail("a number with a leading zero");
        }
        while (!eof() && is_digit(peek())) ++pos_;
        if (!eof() && peek() == '.') {
            ++pos_;
            if (eof() || !is_digit(peek())) {
                return fail("a number with no digits after the decimal point");
            }
            while (!eof() && is_digit(peek())) ++pos_;
        }
        if (!eof() && (peek() == 'e' || peek() == 'E')) {
            ++pos_;
            if (!eof() && (peek() == '+' || peek() == '-')) ++pos_;
            if (eof() || !is_digit(peek())) {
                return fail("a number with no digits in its exponent");
            }
            while (!eof() && is_digit(peek())) ++pos_;
        }
        const std::string_view lexeme = text_.substr(start, pos_ - start);
        // An infinity that a caller later narrows to an integer is how a
        // plausible file ends up naming the wrong thing, so a number no
        // double can hold is refused here, whatever field it is in.
        double d = 0.0;
        if (!parse_floating(lexeme, d)) {
            return fail("the number " + std::string(lexeme) +
                        " is too large to represent");
        }
        out.type_ = Value::Type::Number;
        out.text_.assign(lexeme.data(), lexeme.size());
        return account_bytes(lexeme.size());
    }

    std::string_view text_;
    const Limits& limits_;
    std::size_t pos_ = 0;
    std::size_t line_start_ = 0;
    int depth_ = 0;
    int line_ = 1;
    std::size_t values_ = 0;
    std::size_t dom_bytes_ = 0;
    Error error_;
};

bool parse(std::string_view text, Value& out, Error& error, const Limits& limits) {
    Parser p(text, limits);
    Value v;
    if (!p.parse(v)) {
        error = p.error();
        return false;
    }
    out = std::move(v);
    return true;
}

// --- Writer ----------------------------------------------------------------

namespace {

bool write_string(std::string_view s, std::string& out, std::string& error) {
    // A string the parser would refuse must not reach a file: that is a
    // save that cannot be opened again.
    if (!valid_utf8(s)) {
        error = "a string that is not valid UTF-8";
        return false;
    }
    out.push_back('"');
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char hex[8];
                    std::snprintf(hex, sizeof hex, "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += hex;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
    return true;
}

bool is_scalar(const Value& v) { return !v.is_array() && !v.is_object(); }

bool write_value(const Value& v, int indent, std::string& out, std::string& error) {
    switch (v.type()) {
        case Value::Type::Null: out += "null"; return true;
        case Value::Type::Bool: out += v.as_bool() ? "true" : "false"; return true;
        case Value::Type::Number:
            if (!valid_number_lexeme(v.text())) {
                error = "the number " + v.text() + " has no JSON form";
                return false;
            }
            out += v.text();
            return true;
        case Value::Type::String: return write_string(v.text(), out, error);
        case Value::Type::Array: {
            const auto& items = v.items();
            if (items.empty()) {
                out += "[]";
                return true;
            }
            bool scalars = true;
            for (const auto& i : items) scalars = scalars && is_scalar(i);
            if (scalars) {
                out.push_back('[');
                for (std::size_t k = 0; k < items.size(); ++k) {
                    if (k > 0) out += ", ";
                    if (!write_value(items[k], indent, out, error)) return false;
                }
                out.push_back(']');
                return true;
            }
            out += "[\n";
            for (std::size_t k = 0; k < items.size(); ++k) {
                out.append(static_cast<std::size_t>(indent + 2), ' ');
                if (!write_value(items[k], indent + 2, out, error)) return false;
                out += k + 1 < items.size() ? ",\n" : "\n";
            }
            out.append(static_cast<std::size_t>(indent), ' ');
            out.push_back(']');
            return true;
        }
        case Value::Type::Object: {
            const auto& members = v.members();
            if (members.empty()) {
                out += "{}";
                return true;
            }
            out += "{\n";
            for (std::size_t k = 0; k < members.size(); ++k) {
                out.append(static_cast<std::size_t>(indent + 2), ' ');
                if (!write_string(members[k].first, out, error)) return false;
                out += ": ";
                if (!write_value(members[k].second, indent + 2, out, error)) {
                    return false;
                }
                out += k + 1 < members.size() ? ",\n" : "\n";
            }
            out.append(static_cast<std::size_t>(indent), ' ');
            out.push_back('}');
            return true;
        }
    }
    return false;
}

}  // namespace

bool write(const Value& v, std::string& out, std::string& error) {
    std::string text;
    if (!write_value(v, 0, text, error)) return false;
    text.push_back('\n');
    out = std::move(text);
    return true;
}

}  // namespace sonora::json
