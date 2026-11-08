// SPDX-License-Identifier: GPL-3.0-or-later
//
// Loading a manifest model (ADR-0066 §3): bounds, then verify_file, then the
// runner. Offline only, it allocates. Every refusal is a message and a null
// runner; nothing here substitutes another model or method.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "sonora/analyze/inference.h"
#include "sonora/analyze/model_manifest.h"

namespace sonora::analyze {

// ADR-0011 §4.6: what a model may weigh, per file and in the whole manifest,
// so a bad entry cannot grow the image or the analyzer's memory.
inline constexpr std::int64_t kMaxModelBytes = 25LL * 1000 * 1000;
inline constexpr std::int64_t kMaxTotalModelBytes = 100LL * 1000 * 1000;

inline constexpr const char* kNoOnnxMessage = "model unavailable: built without ONNX Runtime";

// False with `error` when an entry or the sum exceeds the bounds.
bool check_model_bounds(const Manifest& manifest, std::string& error);

// Streams the file through SHA-256 (never held whole). Stops reading and
// refuses once the file exceeds `max_bytes`.
class FsFileDigest final : public FileDigest {
public:
    explicit FsFileDigest(std::int64_t max_bytes = kMaxModelBytes) : max_bytes_(max_bytes) {}
    bool digest(const std::string& path, std::int64_t& size_bytes, std::string& sha256_hex,
                std::string& error) override;

private:
    std::int64_t max_bytes_;
};

// Reads and parses a manifest file, then applies check_model_bounds.
bool load_manifest_file(const std::string& path, Manifest& out, std::string& error);

// The runner for `name`, or null with `error`. Order: known name, bounds,
// verify_file (policy, size, sha256), factory.open.
std::unique_ptr<ModelRunner> load_model(const Manifest& manifest, std::string_view name,
                                        const std::string& models_dir, const ModelPolicy& policy,
                                        FileDigest& digest, RunnerFactory& factory, std::string& error);

// ONNX Runtime when this build has it, else the stub whose open() says
// kNoOnnxMessage. `intra_op_threads` defaults to 2 so a bounce analysis
// leaves cores to a recording.
std::unique_ptr<RunnerFactory> make_runner_factory(int intra_op_threads = 2);

// True when make_runner_factory returns the real runner.
bool onnx_runtime_built() noexcept;

}  // namespace sonora::analyze
