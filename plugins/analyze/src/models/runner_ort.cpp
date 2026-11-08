// SPDX-License-Identifier: GPL-3.0-or-later
//
// ONNX Runtime runner, compiled only with SONORA_WITH_ONNX and the library
// found. Offline: it allocates and spawns intra-op threads (2 by default, so
// a bounce analysis does not starve a recording). Never reached from process.
#include <onnxruntime_cxx_api.h>

#include <exception>

#include "model_loader.h"

namespace sonora::analyze {
namespace {

class OrtModelRunner final : public ModelRunner {
public:
    OrtModelRunner(Ort::Env& env, const Ort::SessionOptions& opts, const std::string& path,
                   std::vector<std::string> outputs)
        : session_(env, path.c_str(), opts), outputs_(std::move(outputs)) {
        Ort::AllocatorWithDefaultOptions alloc;
        for (std::size_t i = 0; i < session_.GetInputCount(); ++i)
            input_names_.push_back(session_.GetInputNameAllocated(i, alloc).get());
    }

    bool run(const std::vector<Tensor>& inputs, std::vector<Tensor>& outputs, std::string& error) override {
        try {
            for (const Tensor& t : inputs)
                if (!tensor_consistent(t)) {
                    error = "tensor " + t.name + ": data does not match its shape";
                    return false;
                }
            if (inputs.size() != input_names_.size()) {
                error = "model takes " + std::to_string(input_names_.size()) + " input(s), got " +
                        std::to_string(inputs.size());
                return false;
            }
            Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            std::vector<Ort::Value> values;
            std::vector<const char*> in_names;
            for (std::size_t i = 0; i < inputs.size(); ++i) {
                // ORT's API takes a non-const pointer but does not write to it.
                auto* data = const_cast<float*>(inputs[i].data.data());
                values.push_back(Ort::Value::CreateTensor<float>(mem, data, inputs[i].data.size(),
                                                                 inputs[i].shape.data(), inputs[i].shape.size()));
                in_names.push_back(input_names_[i].c_str());
            }
            std::vector<const char*> out_names;
            for (const std::string& n : outputs_) out_names.push_back(n.c_str());
            auto result = session_.Run(Ort::RunOptions{nullptr}, in_names.data(), values.data(), values.size(),
                                       out_names.data(), out_names.size());
            std::vector<Tensor> out;
            for (std::size_t i = 0; i < result.size(); ++i) {
                if (!result[i].IsTensor()) {
                    error = outputs_[i] + ": output is not a tensor";
                    return false;
                }
                const auto info = result[i].GetTensorTypeAndShapeInfo();
                if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
                    error = outputs_[i] + ": output is not float32";
                    return false;
                }
                Tensor t;
                t.name = outputs_[i];
                t.shape = info.GetShape();
                const float* p = result[i].GetTensorData<float>();
                t.data.assign(p, p + info.GetElementCount());
                out.push_back(std::move(t));
            }
            outputs = std::move(out);
            return true;
        } catch (const std::exception& ex) {
            error = std::string("onnx runtime: ") + ex.what();
            return false;
        }
    }

private:
    Ort::Session session_;
    std::vector<std::string> input_names_;
    std::vector<std::string> outputs_;
};

class OrtRunnerFactory final : public RunnerFactory {
public:
    explicit OrtRunnerFactory(int threads) : env_(ORT_LOGGING_LEVEL_WARNING, "sonora-analyze") {
        opts_.SetIntraOpNumThreads(threads > 0 ? threads : 2);
        opts_.SetInterOpNumThreads(1);
    }

    std::unique_ptr<ModelRunner> open(const ManifestEntry& entry, const std::string& path,
                                      std::string& error) override {
        if (static_cast<std::int64_t>(ORT_API_VERSION) < entry.ort_min_api) {
            error = entry.name + ": needs ONNX Runtime API " + std::to_string(entry.ort_min_api) + ", built with " +
                    std::to_string(ORT_API_VERSION);
            return nullptr;
        }
        try {
            return std::make_unique<OrtModelRunner>(env_, opts_, path, entry.outputs);
        } catch (const std::exception& ex) {
            error = entry.name + ": " + ex.what();
            return nullptr;
        }
    }

private:
    Ort::Env env_;
    Ort::SessionOptions opts_;
};

}  // namespace

std::unique_ptr<RunnerFactory> make_runner_factory(int intra_op_threads) {
    return std::make_unique<OrtRunnerFactory>(intra_op_threads);
}
bool onnx_runtime_built() noexcept { return true; }

}  // namespace sonora::analyze
