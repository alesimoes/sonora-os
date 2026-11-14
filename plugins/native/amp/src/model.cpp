// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/native/amp_model.h"

#include "sonora/ampcab/nam_file.h"
#include "sonora/dsp/neural/lstm.h"
#include "sonora/dsp/neural/wavenet.h"

namespace sonora::native::amp {

namespace {

class WaveNetModel final : public NeuralModel {
public:
    bool load(const ampcab::NamModel& m, std::string* error) { return net_.load(m.wavenet, m.weights, error); }
    // The WaveNet's prewarm is its receptive field: the rate does not enter.
    void prepare(int max_frames, double) override { net_.prepare(max_frames); }
    void reset() noexcept override { net_.reset(); }
    void process(const float* in, float* out, int frames) noexcept override { net_.process(in, out, frames); }

private:
    dsp::neural::WaveNet net_;
};

class LstmModel final : public NeuralModel {
public:
    bool load(const ampcab::NamModel& m, std::string* error) { return net_.load(m.lstm, m.weights, error); }
    void prepare(int max_frames, double rate) override { net_.prepare(max_frames, rate); }
    void reset() noexcept override { net_.reset(); }
    void process(const float* in, float* out, int frames) noexcept override { net_.process(in, out, frames); }

private:
    dsp::neural::Lstm net_;
};

template <class T>
std::unique_ptr<NeuralModel> loaded(const ampcab::NamModel& m, std::string* error) {
    auto k = std::make_unique<T>();
    if (!k->load(m, error)) return nullptr;
    return k;
}

}  // namespace

std::unique_ptr<NeuralModel> make_neural_model(const ampcab::NamModel& model, std::string* error) {
    if (model.architecture == "WaveNet") return loaded<WaveNetModel>(model, error);
    if (model.architecture == "LSTM") return loaded<LstmModel>(model, error);
    if (error) *error = "the amp runs WaveNet and LSTM captures, not \"" + model.architecture + "\"";
    return nullptr;
}

}  // namespace sonora::native::amp
