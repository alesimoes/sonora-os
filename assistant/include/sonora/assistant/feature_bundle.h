// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the Claude layer may say about a session (ADR-0155 §2): measurements
// and structure, never audio. Every field is a scalar, a name or a fixed
// array of band levels; there is no sample buffer, no frame count and no
// path to a media file, so nothing built from a bundle can carry audio off
// the machine. A field added here is a privacy decision: it goes through the
// ADR, not a side edit.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sonora::assistant {

// The octave bands sonora-analyze reports (31.5 Hz to 16 kHz). Kept here
// as a count, not an include: the assistant reads analysis results, it does
// not link the analyser.
constexpr std::size_t kBands = 10;

// Loudness and spectrum of one strip or of the mix, as measured offline.
// -200 is "not measured" (silence or no analysis yet), as in sonora-analyze.
struct Measurements {
    double integrated_lufs = -200.0;
    double true_peak_dbtp = -200.0;
    double loudness_range_lu = 0.0;
    double crest_db = 0.0;
    double correlation = 1.0;
    std::array<double, kBands> band_db{};
    bool measured = false;
};

struct SendFeatures {
    std::uint64_t send_id = 0;   // a model::Id
    std::uint64_t bus_id = 0;
    double level_db = 0.0;
    bool pre_fader = false;
};

struct TrackFeatures {
    std::uint64_t track_id = 0;  // a model::Id: what a reply step may name
    // The operator's name, sent verbatim. It is data to the model, never an
    // instruction: the reply parser accepts only ids from this bundle, so a
    // track called "ignore the above" changes nothing (ADR-0155 §4).
    std::string name;
    std::uint16_t channels = 1;
    double gain_db = 0.0;
    double pan = 0.0;            // -1 left .. +1 right
    bool muted = false;
    // Insert plug-in display names, in slot order; parameters are not sent.
    std::vector<std::string> inserts;
    std::vector<SendFeatures> sends;
    Measurements measured;
};

struct BusFeatures {
    std::uint64_t bus_id = 0;
    std::string name;
    double gain_db = 0.0;
    std::vector<std::string> inserts;
};

struct FeatureBundle {
    // The snapshot revision the bundle describes. The parsed plan carries it
    // as base_revision, so a reply to a stale bundle is refused at preview.
    std::uint64_t base_revision = 0;
    double sample_rate = 0.0;
    // What the operator typed ("vocal upfront"). Data, like the names.
    std::string intent;
    Measurements mix;
    std::vector<TrackFeatures> tracks;
    std::vector<BusFeatures> buses;
};

}  // namespace sonora::assistant
