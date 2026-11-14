#include <algorithm>
#include <cctype>
#include <cmath>

#include "sonora/loops/loop_scan.h"

namespace sonora::loops {
namespace {

bool contains_ci(const std::string& hay, const std::string& needle) {
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b));
                          });
    return it != hay.end();
}

bool matches(const LoopEntry& e, const LoopQuery& q) {
    if (!q.category.empty() && e.category != q.category) return false;
    if (!q.key.empty() && e.key != q.key) return false;
    if (q.bpm > 0.0 &&
        (e.bpm <= 0.0 || std::fabs(e.bpm - q.bpm) > q.bpm * q.bpm_tolerance))
        return false;
    if (!q.text.empty()) {
        bool hit = contains_ci(e.name, q.text);
        for (const auto& t : e.tags) hit = hit || contains_ci(t, q.text);
        if (!hit) return false;
    }
    return true;
}

}  // namespace

std::vector<LoopEntry> filter(const std::vector<LoopEntry>& all,
                              const LoopQuery& q) {
    std::vector<LoopEntry> out;
    for (const auto& e : all)
        if (matches(e, q)) out.push_back(e);
    return out;
}

}  // namespace sonora::loops
