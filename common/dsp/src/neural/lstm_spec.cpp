// SPDX-License-Identifier: GPL-3.0-or-later
//
// What an LSTM spec may be and how many weights it carries (ADR-0111 §2,
// §3). Kept apart from the kernel (src/neural/lstm.cpp) so the `.nam`
// reader and the contract test can use it without the kernel.
#include "sonora/dsp/neural/lstm.h"

namespace sonora::dsp::neural {

namespace {

// One layer's share: W [4H][in + H], bias [4H], initial h [H], initial c [H].
std::size_t layer_weights(std::size_t in, std::size_t h) {
    return 4 * h * (in + h) + 4 * h + h + h;
}

}  // namespace

std::string lstm_check(const LstmSpec& spec) {
    if (spec.num_layers < 1 || spec.num_layers > kLstmMaxLayers)
        return "an LSTM needs 1 to " + std::to_string(kLstmMaxLayers) + " layers";
    if (spec.input_size != 1) return "input_size must be 1 (one input channel)";
    if (spec.hidden_size < 1 || spec.hidden_size > kLstmMaxHidden)
        return "hidden_size must be 1 to " + std::to_string(kLstmMaxHidden);
    if (lstm_weight_count(spec) > kLstmMaxWeights)
        return "over " + std::to_string(kLstmMaxWeights) + " weights";
    return {};
}

std::size_t lstm_weight_count(const LstmSpec& spec) {
    if (spec.num_layers < 1 || spec.hidden_size < 1 || spec.input_size < 0) return 0;
    const std::size_t h = static_cast<std::size_t>(spec.hidden_size);
    const std::size_t deeper = static_cast<std::size_t>(spec.num_layers - 1);
    const std::size_t head = h + 1;
    return layer_weights(static_cast<std::size_t>(spec.input_size), h) + deeper * layer_weights(h, h) + head;
}

}  // namespace sonora::dsp::neural
