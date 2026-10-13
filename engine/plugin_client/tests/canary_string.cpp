// SPDX-License-Identifier: GPL-3.0-or-later
// Must FAIL tests/rt_reachability.cmake, built at -O2 (rt-safety-auditor M1
// on PR #49): libstdc++ declares basic_string<char> an extern template, so
// a string built here allocates inside libstdc++ through out-of-line
// members whose names a denylist does not know. Never linked.
#include <string>

std::size_t canary_string(const char* p) {
    std::string s(p);
    return s.size();
}
