// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADR-0077's offline runner: audio in, the contract's EvalReport out. Not
// real-time: it allocates freely. It uses only the tuner's public headers
// (tuner.h, tuner_pitch.h) and never links anything into the audio path.
#pragma once

#include <limits>
#include <string>
#include <vector>

#include "sonora/native/tuner_eval.h"

namespace sonora::native::tuner_eval {

constexpr int kRangeCount = 3;
extern const char* const kRangeNames[kRangeCount];

// Mono audio at its own rate.
struct Clip {
    std::vector<float> samples;
    double fs = 0.0;
};

// The YinDetector over the clip at one of the tuner's three ranges, as a
// series on the detector's own hop. Frame times are the middle of the
// analysis window (the detector reports when a window ends).
std::vector<PitchFrame> detect_pitch(const Clip& clip, int range);

// The estimate on the reference's time grid: for each reference frame the
// nearest estimate frame, or an unvoiced frame when none is within a hop.
std::vector<PitchFrame> resample_to(const std::vector<PitchFrame>& estimate,
                                    const std::vector<PitchFrame>& reference);

// Fraction of the voiced output frames within tol_cents of a note of the
// scale. The output pitch comes from a normalised-autocorrelation estimator
// written here, independent of YinDetector, so the tuner is not marked by
// its own detector. NaN when no frame is voiced.
double in_scale_fraction(const std::vector<float>& audio, double fs, int key, int scale,
                         double tol_cents);

// The tuner processor (hard retune, full amount) over the clip, latency
// removed. Empty when the processor refuses the configuration.
std::vector<float> run_tuner(const Clip& clip, int key, int scale, int range);

struct Options {
    std::string dataset;  // "vocadito" or "vocalset"
    std::string data_dir;
    int key = 0;
    int scale = 0;
    double tolerance_cents = 50.0;  // mir_eval's default for RPA and RCA
    double scale_tol_cents = 25.0;
};

struct RunResult {
    int exit_code = 0;    // 0 ok, 1 failure, 2 data missing
    std::string message;  // why, when not 0
    std::string json;     // the report, when exit_code == 0
};

// Reads the dataset, scores it, renders the report. Writes nothing itself.
RunResult run_dataset(const Options& opt);

// Pieces of run_dataset, public for the tests.
std::vector<PitchFrame> parse_f0_csv(const std::string& text);
bool load_wav_mono(const std::string& path, Clip& out, std::string& error);

// Detector figures (all three ranges) and processor figures for one set of
// clips; the building block run_dataset and the tests share.
struct TrackSet {
    std::vector<Clip> clips;
    std::vector<std::vector<PitchFrame>> references;  // empty: no f0 labels
};
struct SetReport {
    EvalReport detector[kRangeCount];
    EvalReport processor;
};
SetReport evaluate(const TrackSet& set, const Options& opt);

}  // namespace sonora::native::tuner_eval
