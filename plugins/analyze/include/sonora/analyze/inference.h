// SPDX-License-Identifier: GPL-3.0-or-later
//
// General inference for the analysis passes (ADR-0011 §3.4, ADR-0066 §2).
// The ONNX Runtime runner is one implementation, compiled only when
// SONORA_WITH_ONNX found the library; passes are written against this
// interface so their tests run on the fake and the build runs without ORT.
// Offline only: it allocates. Never reached from a `process`.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sonora/analyze/model_manifest.h"

namespace sonora::analyze {

// Float32 only: every model in the manifest takes and returns it.
struct Tensor {
    std::string name;
    std::vector<std::int64_t> shape;
    std::vector<float> data;
};

// The element count a shape implies; -1 for a negative or overflowing
// dimension, so a caller can refuse a tensor whose data does not fit.
inline std::int64_t element_count(const std::vector<std::int64_t>& shape) noexcept {
    std::int64_t n = 1;
    for (const std::int64_t d : shape) {
        if (d < 0) return -1;
        if (d != 0 && n > INT64_MAX / d) return -1;
        n *= d;
    }
    return n;
}

inline bool tensor_consistent(const Tensor& t) noexcept {
    const std::int64_t n = element_count(t.shape);
    return n >= 0 && static_cast<std::uint64_t>(n) == t.data.size();
}

class ModelRunner {
public:
    virtual ~ModelRunner() = default;
    // One forward pass. `outputs` is replaced, named as the manifest's
    // `outputs`. False with `error` on a shape or runtime failure.
    virtual bool run(const std::vector<Tensor>& inputs, std::vector<Tensor>& outputs, std::string& error) = 0;
};

class RunnerFactory {
public:
    virtual ~RunnerFactory() = default;
    // Opens a model that verify_file has already accepted. Null with
    // `error` when it cannot (no ONNX Runtime in this build, a load
    // failure); the pass then fails, it does not try another model.
    virtual std::unique_ptr<ModelRunner> open(const ManifestEntry& entry, const std::string& path,
                                              std::string& error) = 0;
};

}  // namespace sonora::analyze
