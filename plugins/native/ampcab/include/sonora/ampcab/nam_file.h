// SPDX-License-Identifier: GPL-3.0-or-later
//
// The `.nam` reader (ADR-0011 §4.3, stage N3): the NAM project's open model
// format, JSON with the architecture, its configuration, the flat weight
// list and the sample rate the capture was trained at.
//
// Read once when a capture is loaded, never in process(). What it accepts
// is exactly what common/dsp/neural runs: a "WaveNet" whose configuration
// uses only the keys of format 0.5 (the trainer's standard, lite, feather
// and nano presets, gated or not), or the newer keys at their neutral
// value; or an "LSTM" (num_layers, input_size 1, hidden_size, flat weights).
// Anything else is refused with a sentence naming the key, so an operator's
// file is either run as its author trained it or not at all.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "sonora/dsp/neural/lstm.h"
#include "sonora/dsp/neural/wavenet.h"

namespace sonora::ampcab {

struct NamModel {
    std::string version;       // the file's "version", e.g. "0.5.4"
    std::string architecture;  // "WaveNet" or "LSTM"
    dsp::neural::WaveNetSpec wavenet;  // set when architecture is "WaveNet"
    dsp::neural::LstmSpec lstm;        // set when architecture is "LSTM"
    std::vector<float> weights;
    // The rate the capture was trained at. Early files carry none; the
    // format then means 48 kHz (sample_rate_given is false).
    double sample_rate = 48000.0;
    bool sample_rate_given = false;
    // metadata.loudness (dB) when the trainer recorded it: the processor's
    // output normalisation reads it.
    bool has_loudness = false;
    double loudness_db = 0.0;
};

// From the file's text. False with *error set (one sentence) on any
// problem: malformed JSON, a format version outside 0.5.x-0.6.x, a missing
// or mistyped field, an unsupported architecture or key, a network over
// the kernel's size caps, a weight count that does not match.
bool parse_nam(std::string_view text, NamModel* out, std::string* error);

// The same from a file. Refuses a file over 64 MiB (the JSON reader's cap)
// from its size, before reading it.
bool read_nam_file(const std::string& path, NamModel* out, std::string* error);

}  // namespace sonora::ampcab
