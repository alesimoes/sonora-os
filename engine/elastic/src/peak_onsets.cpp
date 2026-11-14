// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cmath>

#include "sonora/elastic/onset_source.h"

namespace sonora::elastic {

bool PeakOnsetSource::find(const stretch::Planar& audio, std::int64_t first, std::int64_t count,
                           std::uint32_t sample_rate, std::vector<std::int64_t>& out,
                           std::string& why) {
    out.clear();
    if (audio.empty() || sample_rate == 0 || first < 0 || count < 1 ||
        static_cast<std::int64_t>(audio[0].size()) < first + count) {
        why = "the region's audio is not there to find onsets in";
        return false;
    }
    const std::int64_t hop = std::max<std::int64_t>(1, sample_rate / 200);   // 5 ms
    const std::size_t back = 6;                                              // 30 ms
    const std::size_t hold = 10;                                             // 50 ms
    std::vector<double> db;
    db.reserve(static_cast<std::size_t>(count / hop + 1));
    for (std::int64_t h = 0; h < count; h += hop) {
        const std::int64_t n = std::min(hop, count - h);
        double energy = 0.0;
        for (const auto& ch : audio) {
            for (std::int64_t i = 0; i < n; ++i) {
                const double v = ch[static_cast<std::size_t>(first + h + i)];
                energy += v * v;
            }
        }
        energy /= static_cast<double>(n * static_cast<std::int64_t>(audio.size()));
        db.push_back(10.0 * std::log10(energy + 1e-12));
    }
    std::size_t last = 0;
    bool any = false;
    for (std::size_t k = 1; k < db.size(); ++k) {
        if (db[k] < kFloorDb || (any && k - last < hold)) continue;
        const double before = *std::min_element(db.begin() + static_cast<std::ptrdiff_t>(
                                                                k >= back ? k - back : 0),
                                                db.begin() + static_cast<std::ptrdiff_t>(k));
        if (db[k] - before < kRiseDb) continue;
        // The first frame of the hop that rose: the attack, give or take 5 ms.
        out.push_back(static_cast<std::int64_t>(k) * hop);
        last = k;
        any = true;
    }
    return true;
}

}  // namespace sonora::elastic
