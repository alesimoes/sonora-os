#pragma once

// transport.varispeed's argument (ADR-0190, amending ADR-0033 §4): the
// playback ratio as decimal text, 0.5 to 2.0, "1" resets. One parser for
// the engine and the fake, so both refuse with the same words.

#include <cstddef>
#include <cstdint>
#include <string>

namespace sonora::api {

inline constexpr const char* kVarispeedCommand = "transport.varispeed";
inline constexpr double kVarispeedMin = 0.5;
inline constexpr double kVarispeedMax = 2.0;
// Session::set_varispeed()'s own refusal, word for word: the control path
// may hand the engine's text straight back.
inline constexpr const char* kVarispeedOutOfRange = "varispeed ratio outside 0.5 to 2.0";

namespace varispeed_detail {
// A ratio is kept to the micro: finer than any audible speed step, and an
// integer, so the text round-trips exactly.
inline constexpr int64_t kMicro = 1000000;
inline constexpr std::size_t kMaxFraction = 6;
}  // namespace varispeed_detail

// "D[.D...]" with at most six fraction digits, by hand: strtod reads the
// locale's decimal point, and "nan", "inf" and "1e0" are not ratios a user
// types. A minus sign before a valid number is out of range, not unreadable.
// Never throws; `ratio` is untouched on refusal.
inline bool parse_varispeed(const std::string& text, double& ratio, std::string& why) {
    using namespace varispeed_detail;
    if (text.empty()) {
        why = "no varispeed ratio given (0.5 to 2.0, 1 resets)";
        return false;
    }
    const bool negative = text[0] == '-';
    std::size_t i = negative ? 1 : 0;
    int64_t whole = 0;
    std::size_t whole_digits = 0;
    bool huge = false;
    for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i, ++whole_digits) {
        if (whole > 1000) {
            huge = true;  // already out of range; stop accumulating before overflow
        } else {
            whole = whole * 10 + (text[i] - '0');
        }
    }
    int64_t fraction = 0;
    std::size_t fraction_digits = 0;
    bool dot = false;
    if (i < text.size() && text[i] == '.') {
        dot = true;
        for (++i; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i, ++fraction_digits) {
            if (fraction_digits < kMaxFraction) fraction = fraction * 10 + (text[i] - '0');
        }
    }
    if (i != text.size() || whole_digits == 0 || (dot && fraction_digits == 0) ||
        fraction_digits > kMaxFraction) {
        why = "\"" + text + "\" is not a varispeed ratio such as 1.25";
        return false;
    }
    for (std::size_t d = fraction_digits; d < kMaxFraction; ++d) fraction *= 10;
    const int64_t micro = whole * kMicro + fraction;
    if (negative || huge || micro < kMicro / 2 || micro > 2 * kMicro) {
        why = kVarispeedOutOfRange;
        return false;
    }
    ratio = static_cast<double>(micro) / static_cast<double>(kMicro);
    return true;
}

// The ratio as the argument would carry it: shortest decimal, no trailing
// zeros ("1", "1.5", "0.749"), rounded to the micro.
inline std::string format_varispeed(double ratio) {
    using namespace varispeed_detail;
    const int64_t micro = static_cast<int64_t>(ratio * static_cast<double>(kMicro) + 0.5);
    std::string s = std::to_string(micro / kMicro);
    const int64_t fraction = micro % kMicro;
    if (fraction == 0) return s;
    std::string digits = std::to_string(fraction);
    digits.insert(0, kMaxFraction - digits.size(), '0');
    while (digits.back() == '0') digits.pop_back();
    return s + "." + digits;
}

}  // namespace sonora::api
