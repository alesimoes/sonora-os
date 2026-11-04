// SPDX-License-Identifier: GPL-3.0-or-later
#include "strip_silence.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace sonora::ui {

using namespace model;

bool parse_strip_params(const std::string& text, StripParams& out, std::string& why) {
    StripParams p;
    double* fields[] = {&p.threshold_db, &p.min_silence_ms, &p.start_pad_ms, &p.end_pad_ms};
    std::size_t pos = 0, i = 0;
    while (pos < text.size() && i < 4) {
        std::size_t comma = text.find(',', pos);
        if (comma == std::string::npos) comma = text.size();
        const std::string field = text.substr(pos, comma - pos);
        char* end = nullptr;
        const double v = std::strtod(field.c_str(), &end);
        if (field.empty() || end == nullptr || *end != '\0' || !std::isfinite(v)) {
            why = "\"" + field + "\" is not a number; Strip Silence takes "
                  "threshold dB, minimum silence ms, start pad ms, end pad ms";
            return false;
        }
        *fields[i++] = v;
        pos = comma + 1;
    }
    if (pos < text.size()) {
        why = "Strip Silence takes at most four values: threshold dB, minimum silence ms, "
              "start pad ms, end pad ms";
        return false;
    }
    if (p.threshold_db > 0.0 || p.threshold_db < -120.0) {
        why = "the threshold must be between -120 and 0 dB";
        return false;
    }
    if (p.min_silence_ms <= 0.0 || p.start_pad_ms < 0.0 || p.end_pad_ms < 0.0) {
        why = "the minimum silence must be above 0 ms and the pads 0 ms or more";
        return false;
    }
    out = p;
    return true;
}

std::vector<SilentSpan> find_silence(const PeakSource& peaks, const Region& r,
                                     uint32_t sample_rate, const StripParams& p) {
    std::vector<SilentSpan> out;
    if (r.length <= 0 || r.channels.empty() || sample_rate == 0) return out;
    const auto ms = [&](double v) {
        return static_cast<Frames>(std::llround(v * sample_rate / 1000.0));
    };
    const Frames bucket = std::max<Frames>(1, ms(1.0));
    const Frames buckets = (r.length + bucket - 1) / bucket;
    const float limit = static_cast<float>(std::pow(10.0, p.threshold_db / 20.0));
    const Frames min_len = std::max<Frames>(1, ms(p.min_silence_ms));
    const Frames pad_in = ms(p.start_pad_ms), pad_out = ms(p.end_pad_ms);

    constexpr int kBatch = 4096;
    std::vector<PeakPair> buf(kBatch);
    std::vector<bool> loud(static_cast<std::size_t>(buckets), false);
    for (Frames first = 0; first < buckets; first += kBatch) {
        const int n = static_cast<int>(std::min<Frames>(kBatch, buckets - first));
        for (const SourceRef& c : r.channels) {
            peaks.peaks(c.source, c.channel, r.source_offset + first * bucket,
                        static_cast<double>(bucket), n, buf.data());
            for (int i = 0; i < n; ++i) {
                if (std::max(std::fabs(buf[i].min), std::fabs(buf[i].max)) > limit) {
                    loud[static_cast<std::size_t>(first + i)] = true;
                }
            }
        }
    }

    Frames b = 0;
    while (b < buckets) {
        if (loud[static_cast<std::size_t>(b)]) {
            ++b;
            continue;
        }
        Frames e = b;
        while (e < buckets && !loud[static_cast<std::size_t>(e)]) ++e;
        Frames from = b * bucket, to = std::min(e * bucket, r.length);
        if (to - from >= min_len) {
            const bool whole = from == 0 && to == r.length;
            if (from > 0) from += pad_out;
            if (to < r.length) to -= pad_in;
            if (whole || to > from) out.push_back({r.position + from, r.position + to});
        }
        b = e;
    }
    return out;
}

}  // namespace sonora::ui
