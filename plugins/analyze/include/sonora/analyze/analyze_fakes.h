// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fakes of the analysis interfaces (ADR-0066 §5), so each slice's tests
// and the app's tests run without audio files, model files or ONNX
// Runtime. Each returns what it was set up with and records its calls.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "sonora/analyze/inference.h"
#include "sonora/analyze/key_detector.h"
#include "sonora/analyze/model_manifest.h"
#include "sonora/analyze/timbre_match.h"

namespace sonora::analyze {

inline bool fake_cancelled(const RunOptions& run, std::string& error) {
    if (run.cancel && run.cancel->load()) {
        error = "cancelled";
        return true;
    }
    return false;
}

class FakeKeyDetector final : public KeyDetector {
public:
    KeyResult result;
    std::string fail_with;  // non-empty: detect fails with this error
    int calls = 0;

    bool detect(AudioSource&, const RunOptions& run, KeyResult& out, std::string& error) override {
        ++calls;
        if (fake_cancelled(run, error)) return false;
        if (!fail_with.empty()) {
            error = fail_with;
            return false;
        }
        out = result;
        return true;
    }
};

class FakeTimbreMeasurer final : public TimbreMeasurer {
public:
    TimbreCurve curve;
    int calls = 0;

    bool measure(AudioSource&, const RunOptions& run, TimbreCurve& out, std::string& error) override {
        ++calls;
        if (fake_cancelled(run, error)) return false;
        out = curve;
        return true;
    }
};

// The plain difference, clamped, with no EQ fitting: enough for a test
// of what consumes a match, not a model of the real matcher.
class FakeTimbreMatcher final : public TimbreMatcher {
public:
    std::vector<EqBand> bands;
    int calls = 0;

    bool match(const TimbreCurve& reference, const TimbreCurve& target, const TimbreMatchParams& params,
               TimbreMatchResult& out, std::string& error) override {
        ++calls;
        if (!timbre_params_valid(params)) {
            error = "timbre: params out of range";
            return false;
        }
        TimbreMatchResult r;
        for (int b = 0; b < kTimbreBands; ++b) {
            if (!reference.valid[b] || !target.valid[b]) continue;
            const double d = (reference.gain_db[b] - target.gain_db[b]) * params.amount;
            r.curve_db[b] = std::max(-params.max_gain_db, std::min(params.max_gain_db, d));
        }
        r.eq_bands.assign(bands.begin(), bands.begin() + std::min<std::size_t>(bands.size(), params.max_bands));
        out = std::move(r);
        return true;
    }
};

class FakeFileDigest final : public FileDigest {
public:
    struct File {
        std::int64_t size_bytes = 0;
        std::string sha256;
    };
    std::map<std::string, File> files;
    std::vector<std::string> asked;

    bool digest(const std::string& path, std::int64_t& size_bytes, std::string& sha256_hex,
                std::string& error) override {
        asked.push_back(path);
        const auto it = files.find(path);
        if (it == files.end()) {
            error = "no such file";
            return false;
        }
        size_bytes = it->second.size_bytes;
        sha256_hex = it->second.sha256;
        return true;
    }
};

// Returns `outputs` as set; with none set, echoes the inputs renamed to
// the expected output names (a round-trip check).
class FakeModelRunner final : public ModelRunner {
public:
    std::vector<std::string> output_names;
    std::vector<Tensor> outputs;
    std::vector<std::vector<Tensor>> seen;

    bool run(const std::vector<Tensor>& inputs, std::vector<Tensor>& out, std::string& error) override {
        for (const Tensor& t : inputs)
            if (!tensor_consistent(t)) {
                error = "tensor " + t.name + ": data does not match its shape";
                return false;
            }
        seen.push_back(inputs);
        if (!outputs.empty()) {
            out = outputs;
            return true;
        }
        out = inputs;
        for (std::size_t i = 0; i < out.size() && i < output_names.size(); ++i) out[i].name = output_names[i];
        return true;
    }
};

// Opens a FakeModelRunner per model, or refuses as a build without ONNX
// Runtime does when `available` is false.
class FakeRunnerFactory final : public RunnerFactory {
public:
    bool available = true;
    std::vector<std::string> opened;

    std::unique_ptr<ModelRunner> open(const ManifestEntry& entry, const std::string& path,
                                      std::string& error) override {
        if (!available) {
            error = entry.name + ": built without ONNX Runtime";
            return nullptr;
        }
        opened.push_back(path);
        auto r = std::make_unique<FakeModelRunner>();
        r->output_names = entry.outputs;
        return r;
    }
};

}  // namespace sonora::analyze
