// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Rubber Band adapter (ADR-0136 §3): offline mode, studied whole, then
// processed with the warp map as its key-frame map. Built only where
// pkg-config finds rubberband; no_library_stretcher.cpp stands in otherwise.
#include <rubberband/RubberBandStretcher.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <map>
#include <memory>
#include <vector>

#include "sonora/elastic/library_stretcher.h"

// The R3 engine ("finer") came with Rubber Band 3.0, API 2.7.
#if defined(RUBBERBAND_API_MAJOR_VERSION) && defined(RUBBERBAND_API_MINOR_VERSION) && \
    (RUBBERBAND_API_MAJOR_VERSION > 2 ||                                             \
     (RUBBERBAND_API_MAJOR_VERSION == 2 && RUBBERBAND_API_MINOR_VERSION >= 7))
#define SONORA_RUBBERBAND_R3 1
#else
#define SONORA_RUBBERBAND_R3 0
#endif

namespace sonora::elastic {

namespace {

using RubberBand::RubberBandStretcher;

constexpr std::size_t kBlock = 16384;

RubberBandStretcher::Options options_for(stretch::Algorithm a) {
    RubberBandStretcher::Options o =
        RubberBandStretcher::OptionProcessOffline | RubberBandStretcher::OptionChannelsTogether;
    switch (a) {
        case stretch::Algorithm::Rhythmic:
            // R3 ignores the transient options; R2 keeps attacks crisp.
#if SONORA_RUBBERBAND_R3
            o |= RubberBandStretcher::OptionEngineFaster;
#endif
            return o | RubberBandStretcher::OptionTransientsCrisp |
                   RubberBandStretcher::OptionDetectorPercussive;
        case stretch::Algorithm::Monophonic:
#if SONORA_RUBBERBAND_R3
            o |= RubberBandStretcher::OptionEngineFiner;
#endif
            return o | RubberBandStretcher::OptionFormantPreserved;
        case stretch::Algorithm::Polyphonic:
        case stretch::Algorithm::Varispeed:
            break;
    }
#if SONORA_RUBBERBAND_R3
    o |= RubberBandStretcher::OptionEngineFiner;
#endif
    return o;
}

class RubberBandAdapter final : public stretch::Stretcher {
public:
    RubberBandAdapter(std::uint32_t sample_rate, const std::atomic<bool>& cancel)
        : rate_(sample_rate), cancel_(cancel) {}

    bool process(const stretch::Planar& input, const stretch::WarpMap& map,
                 stretch::Algorithm algorithm, stretch::Planar& output,
                 std::string& why) override {
        if (!check(input, map, why)) return false;
        // Varispeed is a plain resample: the pitch follows the speed.
        if (algorithm == stretch::Algorithm::Varispeed) {
            return stretch::LinearResampleStretcher().process(input, map, algorithm, output, why);
        }
        const std::size_t n = input[0].size();
        const auto out_n = static_cast<std::size_t>(map.output_length());
        RubberBandStretcher rb(rate_, input.size(), options_for(algorithm),
                               static_cast<double>(out_n) / static_cast<double>(n), 1.0);
        rb.setExpectedInputDuration(n);
        rb.setMaxProcessSize(kBlock);
        if (!feed(rb, input, true, nullptr, why)) return false;
        std::map<std::size_t, std::size_t> keys;
        for (const stretch::Marker& m : map.markers()) {
            keys[static_cast<std::size_t>(m.source)] = static_cast<std::size_t>(m.target);
        }
        rb.setKeyFrameMap(keys);
        // Zeros past what the library gave: its length is within a few
        // frames of the ratio's, and the copy must be exactly the map's.
        output.assign(input.size(), std::vector<float>(out_n, 0.0f));
        written_ = 0;
        return feed(rb, input, false, &output, why);
    }

private:
    static bool check(const stretch::Planar& input, const stretch::WarpMap& map,
                      std::string& why) {
        if (input.empty() || input[0].empty()) {
            why = "no audio";
            return false;
        }
        for (const auto& ch : input) {
            if (ch.size() != input[0].size()) {
                why = "channels differ in length";
                return false;
            }
        }
        if (static_cast<std::int64_t>(input[0].size()) != map.source_length()) {
            why = "input length differs from the map's source length";
            return false;
        }
        return true;
    }

    // One pass over the input: studied, or processed with the output drained
    // into `output` as it comes.
    bool feed(RubberBandStretcher& rb, const stretch::Planar& input, bool study,
              stretch::Planar* output, std::string& why) {
        const std::size_t n = input[0].size();
        std::vector<const float*> in(input.size());
        for (std::size_t i = 0; i < n; i += kBlock) {
            if (cancel_.load(std::memory_order_relaxed)) {
                why = "cancelled";
                return false;
            }
            const std::size_t count = std::min(kBlock, n - i);
            for (std::size_t c = 0; c < input.size(); ++c) in[c] = input[c].data() + i;
            const bool last = i + count >= n;
            if (study) {
                rb.study(in.data(), count, last);
            } else {
                rb.process(in.data(), count, last);
                drain(rb, *output);
            }
        }
        return true;
    }

    void drain(RubberBandStretcher& rb, stretch::Planar& output) {
        const std::size_t out_n = output[0].size();
        std::vector<std::vector<float>> block(output.size(), std::vector<float>(kBlock));
        std::vector<float*> ptrs(output.size());
        for (std::size_t c = 0; c < output.size(); ++c) ptrs[c] = block[c].data();
        for (int avail = rb.available(); avail > 0; avail = rb.available()) {
            const std::size_t got =
                rb.retrieve(ptrs.data(), std::min(static_cast<std::size_t>(avail), kBlock));
            if (got == 0) break;
            const std::size_t keep = std::min(got, out_n - std::min(out_n, written_));
            for (std::size_t c = 0; c < output.size(); ++c) {
                std::copy(block[c].begin(), block[c].begin() + static_cast<std::ptrdiff_t>(keep),
                          output[c].begin() + static_cast<std::ptrdiff_t>(written_));
            }
            written_ += keep;
        }
    }

    std::uint32_t rate_;
    const std::atomic<bool>& cancel_;
    std::size_t written_ = 0;
};

}  // namespace

StretcherFactory library_stretcher() {
    return [](std::uint32_t sample_rate, const std::atomic<bool>& cancel) {
        return std::unique_ptr<stretch::Stretcher>(new RubberBandAdapter(sample_rate, cancel));
    };
}

}  // namespace sonora::elastic
