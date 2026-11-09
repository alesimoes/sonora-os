// SPDX-License-Identifier: GPL-3.0-or-later
//
// The LSTM contract (ADR-0111 §2, §3): which specs the kernel takes, the
// exact weight count a `.nam` file must carry, and the refusals. Links the
// spec functions only, so it runs before the kernel exists.
//
// The counts are worked by hand from the trainer's export (per layer
// W [4H][in + H], bias [4H], h [H], c [H]; head [H] + bias), so a slip in
// lstm_weight_count's formula cannot hide behind the same formula here.
#include <cstdio>
#include <initializer_list>
#include <string>

#include "check.h"
#include "sonora/dsp/neural/lstm.h"

namespace {

using sonora::dsp::neural::kLstmMaxHidden;
using sonora::dsp::neural::kLstmMaxLayers;
using sonora::dsp::neural::kLstmMaxWeights;
using sonora::dsp::neural::lstm_check;
using sonora::dsp::neural::lstm_weight_count;
using sonora::dsp::neural::LstmSpec;

LstmSpec spec(int layers, int hidden) {
    LstmSpec s;
    s.num_layers = layers;
    s.hidden_size = hidden;
    return s;
}

bool contains(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }

int test_weight_counts() {
    // 1 x 16: 64 x 17 + 64 + 16 + 16 = 1184; head 17.
    CHECK(lstm_weight_count(spec(1, 16)) == 1201);
    // 1 x 24: 96 x 25 + 96 + 24 + 24 = 2544; head 25.
    CHECK(lstm_weight_count(spec(1, 24)) == 2569);
    // 2 x 8: 32 x 9 + 32 + 16 = 336; 32 x 16 + 32 + 16 = 560; head 9.
    CHECK(lstm_weight_count(spec(2, 8)) == 905);
    // The smallest: 4 x 2 + 4 + 2 = 14; head 2.
    CHECK(lstm_weight_count(spec(1, 1)) == 16);
    CHECK(lstm_weight_count(spec(0, 16)) == 0);
    CHECK(lstm_weight_count(spec(1, 0)) == 0);
    return 0;
}

int test_accepted() {
    CHECK(lstm_check(spec(1, 16)).empty());
    CHECK(lstm_check(spec(1, 24)).empty());
    CHECK(lstm_check(spec(2, 8)).empty());
    CHECK(lstm_check(spec(1, 1)).empty());
    CHECK(lstm_check(spec(1, kLstmMaxHidden)).empty());
    CHECK(lstm_check(spec(kLstmMaxLayers, 64)).empty());
    CHECK(lstm_weight_count(spec(kLstmMaxLayers, 64)) <= kLstmMaxWeights);
    return 0;
}

int test_refusals() {
    const std::string no_layers = lstm_check(spec(0, 16));
    CHECK(contains(no_layers, "layers"));
    CHECK(contains(lstm_check(spec(kLstmMaxLayers + 1, 8)), "layers"));
    CHECK(contains(lstm_check(spec(-1, 8)), "layers"));
    CHECK(contains(lstm_check(spec(1, 0)), "hidden_size"));
    CHECK(contains(lstm_check(spec(1, kLstmMaxHidden + 1)), "hidden_size"));
    LstmSpec stereo = spec(1, 16);
    stereo.input_size = 2;
    CHECK(contains(lstm_check(stereo), "input_size"));
    // Each dimension inside its cap, the total over: 2 x 128 carries
    // 66 816 + 131 840 + 129 weights.
    CHECK(lstm_weight_count(spec(2, kLstmMaxHidden)) > kLstmMaxWeights);
    CHECK(contains(lstm_check(spec(2, kLstmMaxHidden)), "weights"));
    // One sentence: no line break, no trailing full stop to double up when
    // the processor puts it in its message.
    for (const LstmSpec& s : {spec(0, 16), spec(1, 0), stereo, spec(2, kLstmMaxHidden)}) {
        const std::string why = lstm_check(s);
        CHECK(!why.empty());
        CHECK(why.find('\n') == std::string::npos);
        CHECK(why.back() != '.');
    }
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += test_weight_counts();
    failed += test_accepted();
    failed += test_refusals();
    if (failed != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", failed);
        return 1;
    }
    std::printf("dsp lstm contract: all passed\n");
    return 0;
}
