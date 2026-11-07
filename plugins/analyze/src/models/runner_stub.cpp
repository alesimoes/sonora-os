// SPDX-License-Identifier: GPL-3.0-or-later
//
// Compiled when ONNX Runtime is not linked: every open() refuses, so a pass
// asked for a model says why and exits 1 (ADR-0066 §1), never falls back.
#include "model_loader.h"

namespace sonora::analyze {
namespace {

class StubRunnerFactory final : public RunnerFactory {
public:
    std::unique_ptr<ModelRunner> open(const ManifestEntry& entry, const std::string&, std::string& error) override {
        error = entry.name + ": " + kNoOnnxMessage;
        return nullptr;
    }
};

}  // namespace

std::unique_ptr<RunnerFactory> make_runner_factory(int) { return std::make_unique<StubRunnerFactory>(); }
bool onnx_runtime_built() noexcept { return false; }

}  // namespace sonora::analyze
