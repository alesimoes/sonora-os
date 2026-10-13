// SPDX-License-Identifier: GPL-3.0-or-later
//
// Strict JSON for the project's own formats: the session file and the key
// maps (ADR-0009 §5, brain/specs/session-format.md "Encoding").
//
// Not part of common/base: a DOM allocates, and common/base is RT-safe by
// definition. Nothing here may be called from the audio thread.
//
// Numbers keep their lexeme. A caller reads an integer field with
// to_int64() and a float field with to_float(), both through
// std::from_chars, so the C locale never enters (a de_DE machine reads
// "-2.5" as -2.5) and a float never passes through a double on the way in.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sonora::json {

// The caps from session-format.md. A corrupt or hostile file must fail with
// a message naming the cap, not exhaust memory or the stack.
struct Limits {
    std::size_t max_input_bytes = std::size_t{64} << 20;
    int max_depth = 64;
    std::size_t max_values = 4'000'000;
    std::size_t max_dom_bytes = std::size_t{256} << 20;
};

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;

    static Value null() { return Value(); }
    static Value boolean(bool b);
    static Value string(std::string s);
    static Value array();
    static Value object();
    // Integers are written as integers: the lexeme is the decimal digits.
    static Value integer(std::int64_t n);
    // Shortest round-trip form for a float, always with a '.' or an
    // exponent so the field still reads as a float. Non-finite values have
    // no JSON form; the writer refuses them (see write()).
    static Value number_float(float f);
    // A number from its lexeme, unchecked. The parser's constructor.
    static Value number_lexeme(std::string lexeme);

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    bool as_bool() const { return boolean_; }
    // The string's content, or a number's lexeme.
    const std::string& text() const { return text_; }

    const std::vector<Value>& items() const { return items_; }
    // Members in file order (parsed) or insertion order (built). The writer
    // emits them in this order, which is how a caller makes output
    // canonical: it builds objects in the order its spec lists the keys.
    const std::vector<std::pair<std::string, Value>>& members() const {
        return members_;
    }
    const Value* find(std::string_view key) const;

    // Building. push/set do not check for duplicate keys: the writer's
    // callers build from their own structs, and the parser checks its input.
    Value& push(Value v);
    Value& set(std::string key, Value v);

    // Where the value starts in the parsed text, 1-based. Zero for a built
    // value.
    int line() const { return line_; }
    int column() const { return column_; }

    // --- number conversions, all through std::from_chars ---

    // True for a lexeme that is an integer in JSON's own grammar: no
    // fraction and no exponent. "1.0" and "1e0" are not integers.
    bool is_integer_lexeme() const;
    // Exact, from the lexeme. False on a fraction, an exponent, or a value
    // outside int64_t.
    bool to_int64(std::int64_t& out) const;
    // Straight into float. False when the value is outside float's range
    // (finite overflow); underflow to zero or a subnormal is a value.
    bool to_float(float& out) const;
    bool to_double(double& out) const;

    bool operator==(const Value& o) const;
    bool operator!=(const Value& o) const { return !(*this == o); }

private:
    friend class Parser;

    Type type_ = Type::Null;
    bool boolean_ = false;
    std::string text_;
    std::vector<Value> items_;
    std::vector<std::pair<std::string, Value>> members_;
    int line_ = 0;
    int column_ = 0;
};

struct Error {
    std::string message;  // what was wrong, without the position
    int line = 0;         // 1-based
    int column = 0;       // 1-based, in bytes
};

// Strict RFC 8259 plus the caps: no comments, no trailing commas, no
// duplicate keys, no leading zeros, no raw control characters in strings,
// UTF-8 only, no byte-order mark, one value and nothing after it. A number
// whose magnitude is beyond a double is refused here, before any caller can
// narrow an infinity into an integer.
bool parse(std::string_view text, Value& out, Error& error,
           const Limits& limits = Limits{});

// The parser's definition of UTF-8: no overlong forms, no surrogates,
// nothing past U+10FFFF. A writer's caller checks names with it before they
// enter a document, so a save never produces a file the parser refuses.
bool valid_utf8(std::string_view s);

// Canonical form: two-space indent, one member per line, arrays of scalars
// on one line ("[0, 1]"), LF line endings, a final newline, non-ASCII text
// written as UTF-8 rather than escaped. The same Value always gives the
// same bytes. Fails only on a number with no JSON form (a non-finite
// float), naming it in `error`.
bool write(const Value& v, std::string& out, std::string& error);

}  // namespace sonora::json
