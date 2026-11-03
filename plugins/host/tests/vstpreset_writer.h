// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A .vstpreset writer for the tests, from the SDK's published layout
// (src/vstpreset.h): the files they read, the good and the crafted.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace sonora::plugin_host::test {

inline void put_le(std::string& s, uint64_t v, int bytes) {
    for (int k = 0; k < bytes; ++k) s += static_cast<char>((v >> (8 * k)) & 0xFF);
}

// `chunks` in order, each (id, data); the list after them, as the SDK writes.
inline std::string vstpreset(const std::string& class_hex, const std::vector<std::pair<std::string, std::string>>& chunks,
                             int32_t version = 1) {
    std::string s = "VST3";
    put_le(s, static_cast<uint32_t>(version), 4);
    s += class_hex;
    s.resize(40, '0');
    const std::size_t list_at_field = s.size();
    put_le(s, 0, 8);
    std::vector<std::pair<uint64_t, uint64_t>> where;
    for (const auto& c : chunks) {
        where.push_back({s.size(), c.second.size()});
        s += c.second;
    }
    const uint64_t list = s.size();
    for (int k = 0; k < 8; ++k) s[list_at_field + std::size_t(k)] = static_cast<char>((list >> (8 * k)) & 0xFF);
    s += "List";
    put_le(s, chunks.size(), 4);
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        std::string id = chunks[i].first;
        id.resize(4, ' ');
        s += id;
        put_le(s, where[i].first, 8);
        put_le(s, where[i].second, 8);
    }
    return s;
}

// The state test plugin's component state: "SNTS" and a count.
inline std::string snts(int64_t n) {
    std::string s = "SNTS";
    put_le(s, static_cast<uint64_t>(n), 8);
    return s;
}

}  // namespace sonora::plugin_host::test
