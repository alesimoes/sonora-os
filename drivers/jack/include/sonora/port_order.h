// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cctype>
#include <cstdlib>
#include <string>

namespace sonora {

// Compares port names so that capture_AUX2 sorts before capture_AUX10.
//
// jack_get_ports returns ports in an unspecified order, and a plain
// lexicographic sort puts AUX10 before AUX2. On an 18-channel interface that
// silently records channel 10 onto track 3: nothing in the audio looks wrong,
// the session is simply patched incorrectly. Hence a named, tested function
// rather than a lambda.
inline bool natural_less(const std::string& a, const std::string& b) {
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const bool da = std::isdigit(static_cast<unsigned char>(a[i])) != 0;
        const bool db = std::isdigit(static_cast<unsigned char>(b[j])) != 0;
        if (da && db) {
            std::size_t i2 = i, j2 = j;
            while (i2 < a.size() &&
                   std::isdigit(static_cast<unsigned char>(a[i2]))) ++i2;
            while (j2 < b.size() &&
                   std::isdigit(static_cast<unsigned char>(b[j2]))) ++j2;
            const long na = std::strtol(a.substr(i, i2 - i).c_str(), nullptr, 10);
            const long nb = std::strtol(b.substr(j, j2 - j).c_str(), nullptr, 10);
            if (na != nb) return na < nb;
            i = i2;
            j = j2;
            continue;
        }
        if (a[i] != b[j]) return a[i] < b[j];
        ++i;
        ++j;
    }
    // Whichever name still has characters left is the longer one, and sorts
    // after. Comparing sizes directly would be wrong here because i and j can
    // differ after a numeric run of unequal width ("AUX02" versus "AUX2").
    return (a.size() - i) < (b.size() - j);
}

}  // namespace sonora
