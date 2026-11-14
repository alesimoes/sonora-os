// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/ui_tce/tce_ratio.h"

#include <cmath>

#include "sonora/api/elastic.h"

namespace sonora::ui::tce {
namespace {

RatioResult refuse(std::string why) {
    RatioResult r;
    r.why = std::move(why);
    return r;
}

RatioResult limited(double ratio) {
    if (!(ratio >= api::kMinElasticRatio) || ratio > api::kMaxElasticRatio) {
        return refuse("a ratio of " + api::elastic_detail::format_micro(ratio) + " is outside " +
                      api::elastic_detail::format_micro(api::kMinElasticRatio) + ".." +
                      api::elastic_detail::format_micro(api::kMaxElasticRatio));
    }
    RatioResult r;
    r.ok = true;
    r.ratio = ratio;
    return r;
}

}  // namespace

RatioResult from_frames(int64_t source_frames, int64_t target_frames) {
    if (source_frames <= 0 || target_frames <= 0) return refuse("lengths must be positive");
    return limited(static_cast<double>(target_frames) / static_cast<double>(source_frames));
}

RatioResult from_seconds(int64_t source_frames, int sample_rate, double seconds) {
    if (sample_rate <= 0 || !(seconds > 0)) return refuse("the length must be positive");
    return from_frames(source_frames, std::llround(seconds * sample_rate));
}

RatioResult from_bars(int64_t source_frames, int sample_rate, double bpm, int beats_per_bar,
                      double bars) {
    if (!(bpm > 0) || beats_per_bar <= 0 || !(bars > 0)) {
        return refuse("tempo, meter and bars must be positive");
    }
    return from_seconds(source_frames, sample_rate, bars * beats_per_bar * 60.0 / bpm);
}

RatioResult from_tempo(double source_bpm, double target_bpm) {
    if (!(source_bpm > 0) || !(target_bpm > 0)) return refuse("tempos must be positive");
    return limited(source_bpm / target_bpm);
}

RatioResult from_percent(double percent) {
    if (!(percent > 0)) return refuse("the percent must be positive");
    return limited(percent / 100.0);
}

int64_t resulting_length(int64_t source_frames, double ratio) {
    return std::llround(static_cast<double>(source_frames) * ratio);
}

}  // namespace sonora::ui::tce
