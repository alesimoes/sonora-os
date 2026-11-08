// SPDX-License-Identifier: GPL-3.0-or-later
//
// The tuner's evaluation on real voices (ADR-0077). Offline only: nothing
// here is reachable from Processor::process. Metrics follow mir_eval.melody
// (Salamon, Gomez, Ellis and Richard, IEEE SPM 2014) so our figures compare
// with published ones (ADR-0077 point 5 needs pYIN's).
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace sonora::native::tuner_eval {

// One analysis frame. hz <= 0 means "no pitch reported"; voiced is the
// voicing decision, which mir_eval scores separately from the pitch.
struct PitchFrame {
    double time_s = 0.0;
    double hz = 0.0;
    bool voiced = false;
};

// Fractions in [0, 1]. A metric with no frame to count over is NaN, and
// in_scale_fraction is NaN unless a processor-level run measured it
// (VocalSet: voiced output frames within X cents of a scale note, by an
// estimator independent of the detector).
struct EvalReport {
    double rpa = std::numeric_limits<double>::quiet_NaN();
    double rca = std::numeric_limits<double>::quiet_NaN();
    double voicing_recall = std::numeric_limits<double>::quiet_NaN();
    double voicing_false_alarm = std::numeric_limits<double>::quiet_NaN();
    double in_scale_fraction = std::numeric_limits<double>::quiet_NaN();
    std::size_t frames = 0;
};

// A dataset's f0 annotations (vocadito's CSVs; a fake in tests). load()
// returns an empty vector when the path is missing or unreadable: the caller
// reports the run as skipped, never as passed (ADR-0077 point 1).
class AnnotationSource {
public:
    virtual ~AnnotationSource() = default;
    virtual std::vector<PitchFrame> load(const std::string& path) = 0;
};

// In-memory annotations keyed by path, for the contract test and for tools
// that synthesise a reference.
class FakeAnnotationSource final : public AnnotationSource {
public:
    void add(std::string path, std::vector<PitchFrame> frames) {
        tracks_[std::move(path)] = std::move(frames);
    }
    std::vector<PitchFrame> load(const std::string& path) override {
        const auto it = tracks_.find(path);
        return it == tracks_.end() ? std::vector<PitchFrame>{} : it->second;
    }

private:
    std::map<std::string, std::vector<PitchFrame>> tracks_;
};

namespace detail {

inline double ratio(std::size_t num, std::size_t den) {
    return den == 0 ? std::numeric_limits<double>::quiet_NaN()
                    : static_cast<double>(num) / static_cast<double>(den);
}

// Error folded to the nearest octave, as mir_eval's raw_chroma_accuracy:
// |c - 1200 * round(c / 1200)|.
inline double chroma_cents(double cents) {
    return std::fabs(cents - 1200.0 * std::floor(cents / 1200.0 + 0.5));
}

inline void append_number(std::string& out, const char* key, double v, bool last = false) {
    out += '"';
    out += key;
    out += "\": ";
    if (std::isfinite(v)) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.6g", v);
        out += buf;
    } else {
        out += "null";
    }
    out += last ? "\n" : ",\n";
}

inline void append_string(std::string& out, const char* key, const std::string& v) {
    out += '"';
    out += key;
    out += "\": \"";
    for (const char c : v) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
            out += buf;
        } else {
            out += c;
        }
    }
    out += "\",\n";
}

}  // namespace detail

// Frames are paired by index over the shorter series: both must already be
// on one time grid (resampling is the caller's step, as mir_eval's
// resample_melody_series). RPA and RCA count reference-voiced frames whose
// estimate reports a pitch within tol_cents, whatever the estimate's voicing
// flag; unvoiced reference frames never enter them.
inline EvalReport score_detector(const std::vector<PitchFrame>& reference,
                                 const std::vector<PitchFrame>& estimate, double tol_cents) {
    const std::size_t n = reference.size() < estimate.size() ? reference.size() : estimate.size();
    std::size_t ref_voiced = 0, ref_unvoiced = 0, pitch_hits = 0, chroma_hits = 0;
    std::size_t recalled = 0, false_alarms = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const PitchFrame& r = reference[i];
        const PitchFrame& e = estimate[i];
        const bool ref_is_voiced = r.voiced && r.hz > 0.0;
        if (!ref_is_voiced) {
            ++ref_unvoiced;
            if (e.voiced) ++false_alarms;
            continue;
        }
        ++ref_voiced;
        if (e.voiced) ++recalled;
        if (!(e.hz > 0.0) || !std::isfinite(e.hz)) continue;
        const double cents = 1200.0 * std::log2(e.hz / r.hz);
        if (std::fabs(cents) <= tol_cents) ++pitch_hits;
        if (detail::chroma_cents(cents) <= tol_cents) ++chroma_hits;
    }
    EvalReport rep;
    rep.rpa = detail::ratio(pitch_hits, ref_voiced);
    rep.rca = detail::ratio(chroma_hits, ref_voiced);
    rep.voicing_recall = detail::ratio(recalled, ref_voiced);
    rep.voicing_false_alarm = detail::ratio(false_alarms, ref_unvoiced);
    rep.frames = n;
    return rep;
}

// The report: numbers and the dataset's identity, never audio (ADR-0077
// point 3). NaN is written as null.
inline std::string render_json(const EvalReport& rep, const std::string& dataset,
                               const std::string& licence, const std::string& version) {
    std::string out = "{\n";
    detail::append_string(out, "dataset", dataset);
    detail::append_string(out, "licence", licence);
    detail::append_string(out, "version", version);
    detail::append_number(out, "rpa", rep.rpa);
    detail::append_number(out, "rca", rep.rca);
    detail::append_number(out, "voicing_recall", rep.voicing_recall);
    detail::append_number(out, "voicing_false_alarm", rep.voicing_false_alarm);
    detail::append_number(out, "in_scale_fraction", rep.in_scale_fraction);
    detail::append_number(out, "frames", static_cast<double>(rep.frames), true);
    out += "}\n";
    return out;
}

}  // namespace sonora::native::tuner_eval
