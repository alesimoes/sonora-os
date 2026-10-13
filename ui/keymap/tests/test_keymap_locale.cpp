// SPDX-License-Identifier: GPL-3.0-or-later
//
// Regression: the key map's JSON reader parsed numbers with strtod, which
// honours LC_NUMERIC. Under de_DE the decimal separator is ',', so strtod
// stopped at the '.' and "target": 2.5 read as 2: a file that should be
// refused loaded and armed track 2. Its own executable, because setlocale is
// process-wide and test_keymap's other checks must not run under it.
#include "sonora/keymap.h"

#include <clocale>
#include <cstdio>
#include <cstring>
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

bool mentions(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

int main() {
    const char* names[] = {"de_DE.UTF-8", "de_DE.utf8", "de_DE"};
    const char* got = nullptr;
    for (const char* n : names) {
        if (std::setlocale(LC_ALL, n) != nullptr) {
            got = n;
            break;
        }
    }
    if (got == nullptr) {
        std::printf("SKIP keymap_locale: no de_DE locale installed\n");
        return 0;
    }
    // Proves the test is testing something: this locale really does use a
    // comma, so strtod would stop at the '.'.
    CHECK(std::strcmp(std::localeconv()->decimal_point, ",") == 0);

    sonora::KeyMap map;
    std::string err;
    CHECK(!sonora::KeyMap::load_string(R"({ "name": "x", "bindings": {
        "q": { "command": "track.toggle_arm", "target": 2.5 } } })",
                                       "<test>", map, err));
    CHECK(mentions(err, "whole track number"));

    err.clear();
    CHECK(!sonora::KeyMap::load_string(R"({ "name": "x", "bindings": {
        "q": { "command": "track.toggle_arm", "target": 1.5 } } })",
                                       "<test>", map, err));
    CHECK(mentions(err, "whole track number"));

    // A whole number still loads, so the fix did not just refuse numbers.
    err.clear();
    CHECK(sonora::KeyMap::load_string(R"({ "name": "x", "bindings": {
        "q": { "command": "track.toggle_arm", "target": 3 } } })",
                                      "<test>", map, err));
    CHECK(map.resolve("q") != nullptr && map.resolve("q")->ctx.target == 2);

    std::printf("keymap_locale: numbers read the same under %s\n", got);
    return 0;
}
