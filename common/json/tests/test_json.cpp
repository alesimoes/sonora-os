// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/json/json.h"

#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

using sonora::json::Error;
using sonora::json::Limits;
using sonora::json::Value;

bool parses(const std::string& text, Value& v) {
    Error e;
    return sonora::json::parse(text, v, e);
}

// Fails to parse, with `needle` in the message, at the given position.
bool refused(const std::string& text, const char* needle, int line = 0,
             int column = 0, const Limits& limits = Limits{}) {
    Value v;
    Error e;
    if (sonora::json::parse(text, v, e, limits)) return false;
    if (e.message.find(needle) == std::string::npos) {
        std::fprintf(stderr, "  message was: %s\n", e.message.c_str());
        return false;
    }
    if (line != 0 && e.line != line) {
        std::fprintf(stderr, "  line was %d\n", e.line);
        return false;
    }
    if (column != 0 && e.column != column) {
        std::fprintf(stderr, "  column was %d\n", e.column);
        return false;
    }
    return true;
}

std::string written(const Value& v) {
    std::string out, err;
    if (!sonora::json::write(v, out, err)) return "<error: " + err + ">";
    return out;
}

int test_values() {
    Value v;
    CHECK(parses(R"({"a": [1, -2, 3.5, true, false, null, "x"], "b": {}})", v));
    CHECK(v.is_object());
    CHECK(v.members().size() == 2);
    const Value* a = v.find("a");
    CHECK(a != nullptr && a->is_array() && a->items().size() == 7);
    CHECK(a->items()[0].text() == "1");
    CHECK(a->items()[2].text() == "3.5");
    CHECK(a->items()[3].as_bool());
    CHECK(a->items()[5].is_null());
    CHECK(a->items()[6].text() == "x");
    CHECK(v.find("b")->is_object());
    CHECK(v.find("missing") == nullptr);
    // Members stay in file order: a canonical writer depends on it.
    CHECK(v.members()[0].first == "a");
    return 0;
}

int test_strictness() {
    CHECK(refused("{\"a\": 1,}", "expected a quoted member name"));
    CHECK(refused("[1, 2,]", "where a value was expected"));
    CHECK(refused("{\"a\": 1, \"a\": 2}", "duplicate member \"a\""));
    CHECK(refused("// c\n{}", "unexpected character '/'"));
    CHECK(refused("{} {}", "trailing content"));
    CHECK(refused("01", "leading zero"));
    CHECK(refused("-01", "leading zero"));
    CHECK(refused("1.", "no digits after the decimal point"));
    CHECK(refused("1e", "no digits in its exponent"));
    CHECK(refused("+1", "unexpected character '+'"));
    CHECK(refused("\"a\tb\"", "raw control character (0x09)"));
    CHECK(refused("\"\\x\"", "unknown escape \\x"));
    CHECK(refused("\"\\ud800\"", "leading UTF-16 surrogate"));
    CHECK(refused("\"\\udc00\"", "trailing UTF-16 surrogate"));
    CHECK(refused("\"\xC0\xAF\"", "not valid UTF-8"));      // overlong '/'
    CHECK(refused("\"\xED\xA0\x80\"", "not valid UTF-8"));  // surrogate
    CHECK(refused("\"\xFF\"", "not valid UTF-8"));
    CHECK(refused("\xEF\xBB\xBF{}", "byte-order mark"));
    CHECK(refused("", "unexpected end of input"));
    CHECK(refused("1e999", "too large to represent"));
    CHECK(refused("-1e999", "too large to represent"));
    // Underflow is a zero, not an error.
    Value v;
    CHECK(parses("1e-999", v));
    double d = 1.0;
    CHECK(v.to_double(d) && d == 0.0);
    // A duplicate in a large object is found too (the hashed path).
    std::string big = "{";
    for (int i = 0; i < 40; ++i) big += "\"k" + std::to_string(i) + "\": 0, ";
    big += "\"k7\": 1}";
    CHECK(refused(big, "duplicate member \"k7\""));
    // Escapes and non-ASCII decode to UTF-8.
    CHECK(parses("\"\\u00e9\\ud83c\\udfb5\xC3\xA9\"", v));
    CHECK(v.text() == "\xC3\xA9\xF0\x9F\x8E\xB5\xC3\xA9");
    return 0;
}

int test_positions() {
    // Line and column of the offending byte, 1-based.
    CHECK(refused("{\n  \"a\": 1,\n  \"a\": 2\n}", "duplicate member", 3, 6));
    CHECK(refused("[\n  1,\n  x\n]", "unexpected character 'x'", 3, 3));
    Value v;
    CHECK(parses("{\n  \"a\": [\n    7\n  ]\n}", v));
    const Value& seven = v.find("a")->items()[0];
    CHECK(seven.line() == 3 && seven.column() == 5);
    return 0;
}

int test_limits() {
    Limits l;
    l.max_depth = 4;
    CHECK(refused("[[[[1]]]]", "nesting deeper than 4 levels", 0, 0, l));
    Value v;
    Error e;
    CHECK(sonora::json::parse("[[[1]]]", v, e, l));
    // The default cap holds a hostile nesting as an error, not a crash.
    CHECK(refused(std::string(100000, '['), "nesting deeper than 64 levels"));

    l = Limits{};
    l.max_input_bytes = 8;
    CHECK(refused("[1, 2, 3, 4]", "over the cap of 8 bytes", 0, 0, l));

    l = Limits{};
    l.max_values = 3;
    CHECK(refused("[1, 2, 3]", "more than 3 values", 0, 0, l));
    CHECK(sonora::json::parse("[1, 2]", v, e, l));

    l = Limits{};
    l.max_dom_bytes = 1000;
    CHECK(refused("\"" + std::string(2000, 'a') + "\"", "bytes of memory", 0, 0, l));
    return 0;
}

int test_numbers() {
    Value v;
    std::int64_t n = 0;
    CHECK(parses("9007199254740991", v));
    CHECK(v.is_integer_lexeme() && v.to_int64(n) && n == 9007199254740991);
    CHECK(parses("-42", v) && v.to_int64(n) && n == -42);
    // Integers are read from the lexeme, never through a double: 2^63 - 1
    // is exact, and one more is refused rather than rounded.
    CHECK(parses("9223372036854775807", v) && v.to_int64(n) &&
          n == std::numeric_limits<std::int64_t>::max());
    CHECK(parses("9223372036854775808", v) && !v.to_int64(n));
    // A fraction or an exponent is not an integer, even when whole.
    CHECK(parses("1.0", v) && !v.is_integer_lexeme() && !v.to_int64(n));
    CHECK(parses("1e3", v) && !v.to_int64(n));

    // Straight into float: 0.1f read from its shortest lexeme is exactly
    // the float the writer started from.
    float f = 0.0f;
    CHECK(parses("-2.5", v) && v.to_float(f) && f == -2.5f);
    CHECK(parses("0.1", v) && v.to_float(f) && f == 0.1f);
    CHECK(parses("1e39", v) && !v.to_float(f));  // past float, inside double
    CHECK(parses("1e-50", v) && v.to_float(f) && f == 0.0f);
    return 0;
}

int test_float_round_trip() {
    // Every float written and read back is bit-identical. A sweep over
    // exponents and mantissas rather than all 2^32, to keep the test fast.
    for (std::uint32_t bits = 0; bits < 0x7F800000u; bits += 0x00012345u) {
        for (const std::uint32_t sign : {0u, 0x80000000u}) {
            const std::uint32_t b = bits | sign;
            float f;
            std::memcpy(&f, &b, sizeof f);
            const Value w = Value::number_float(f);
            CHECK(w.text().find_first_of(".e") != std::string::npos);
            Value r;
            CHECK(parses(w.text(), r));
            float back = 1.0f;
            CHECK(r.to_float(back));
            std::uint32_t bb;
            std::memcpy(&bb, &back, sizeof bb);
            CHECK(bb == b);
        }
    }
    CHECK(Value::number_float(0.0f).text() == "0.0");
    CHECK(Value::number_float(-3.0f).text() == "-3.0");
    CHECK(Value::number_float(-2.5f).text() == "-2.5");
    // No JSON form for these: the writer refuses rather than write "inf".
    std::string out, err;
    CHECK(!sonora::json::write(Value::number_float(INFINITY), out, err));
    CHECK(err.find("no JSON form") != std::string::npos);
    CHECK(!sonora::json::write(Value::number_float(NAN), out, err));
    return 0;
}

int test_writer() {
    Value root = Value::object();
    root.set("version", Value::integer(1));
    root.set("name", Value::string("Vox \"lead\"\n\xC3\xA9"));
    Value& pair = root.set("outputs", Value::array());
    pair.push(Value::integer(0));
    pair.push(Value::integer(1));
    root.set("empty", Value::array());
    root.set("obj", Value::object());
    Value& list = root.set("list", Value::array());
    Value& item = list.push(Value::object());
    item.set("gain_db", Value::number_float(-2.5f));
    item.set("mute", Value::boolean(false));
    root.set("ctl", Value::string(std::string("\x01", 1)));

    const std::string expected =
        "{\n"
        "  \"version\": 1,\n"
        "  \"name\": \"Vox \\\"lead\\\"\\n\xC3\xA9\",\n"
        "  \"outputs\": [0, 1],\n"
        "  \"empty\": [],\n"
        "  \"obj\": {},\n"
        "  \"list\": [\n"
        "    {\n"
        "      \"gain_db\": -2.5,\n"
        "      \"mute\": false\n"
        "    }\n"
        "  ],\n"
        "  \"ctl\": \"\\u0001\"\n"
        "}\n";
    CHECK(written(root) == expected);

    // Parse then write is the identity on canonical text.
    Value back;
    CHECK(parses(expected, back));
    CHECK(back == root);
    CHECK(written(back) == expected);

    // A string the parser would refuse is refused by the writer too.
    std::string out, err;
    CHECK(!sonora::json::write(Value::string("\xFF"), out, err));
    CHECK(err.find("UTF-8") != std::string::npos);
    return 0;
}

// The bug the keymap had: strtod honours LC_NUMERIC, and under de_DE the
// decimal separator is ',', so "0.5" read as 0. from_chars ignores the
// locale. Skipped, loudly, where no comma locale is installed.
int test_comma_locale() {
    const char* names[] = {"de_DE.UTF-8", "de_DE.utf8", "de_DE", "fr_FR.UTF-8",
                           "fr_FR.utf8"};
    const char* got = nullptr;
    for (const char* n : names) {
        if (std::setlocale(LC_ALL, n) != nullptr) {
            got = n;
            break;
        }
    }
    if (got == nullptr) {
        std::printf("SKIP comma locale: none of de_DE/fr_FR is installed\n");
        return 0;
    }
    const bool comma = std::strcmp(std::localeconv()->decimal_point, ",") == 0;
    Value v;
    float f = 0.0f;
    double d = 0.0;
    const bool ok = parses("-2.5", v) && v.to_float(f) && f == -2.5f &&
                    v.to_double(d) && d == -2.5 &&
                    Value::number_float(0.5f).text() == "0.5";
    std::setlocale(LC_ALL, "C");
    CHECK(comma);
    CHECK(ok);
    std::printf("comma locale %s: numbers read and written unchanged\n", got);
    return 0;
}

}  // namespace

int main() {
    int failures = 0;
    failures += test_values();
    failures += test_strictness();
    failures += test_positions();
    failures += test_limits();
    failures += test_numbers();
    failures += test_float_round_trip();
    failures += test_writer();
    failures += test_comma_locale();
    if (failures != 0) {
        std::fprintf(stderr, "%d json test group(s) failed\n", failures);
        return 1;
    }
    std::printf("json: 8 test groups passed\n");
    return 0;
}
