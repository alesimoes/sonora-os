// SPDX-License-Identifier: GPL-3.0-or-later
//
// native_tuner_eval --data DIR --dataset vocadito|vocalset --out report.json
//                   [--key N --scale NAME]
// Exit 0 ok, 1 failure, 2 data missing (nothing is written then).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "tuner_eval_run.h"

namespace {
// The processor's own scale order (tuner_pitch.h kScaleMasks).
const char* const kScaleNames[] = {"chromatic",        "major",           "minor",
                                   "harmonic-minor",   "major-pentatonic", "minor-pentatonic"};

int scale_index(const char* name) {
    for (int i = 0; i < 6; ++i)
        if (std::strcmp(name, kScaleNames[i]) == 0) return i;
    return -1;
}
}  // namespace

int main(int argc, char** argv) {
    using namespace sonora::native::tuner_eval;
    Options opt;
    std::string out_path;
    opt.scale = 1;
    for (int i = 1; i < argc; i += 2) {
        const std::string a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : nullptr;
        if (!v) {
            std::fprintf(stderr, "missing value for %s\n", a.c_str());
            return 1;
        }
        if (a == "--data") {
            opt.data_dir = v;
        } else if (a == "--dataset") {
            opt.dataset = v;
        } else if (a == "--out") {
            out_path = v;
        } else if (a == "--key") {
            opt.key = std::atoi(v);
        } else if (a == "--scale") {
            opt.scale = scale_index(v);
            if (opt.scale < 0) opt.scale = std::atoi(v);  // an index also works
        } else {
            std::fprintf(stderr, "unknown argument %s\n", a.c_str());
            return 1;
        }
    }
    if (out_path.empty() || opt.dataset.empty()) {
        std::fprintf(stderr,
                     "usage: native_tuner_eval --data DIR --dataset vocadito|vocalset --out "
                     "report.json [--key N --scale NAME]\n");
        return 1;
    }
    const RunResult r = run_dataset(opt);
    if (r.exit_code != 0) {
        std::fprintf(stderr, "native_tuner_eval: %s\n", r.message.c_str());
        return r.exit_code;
    }
    std::ofstream f(out_path, std::ios::binary);
    if (!f || !(f << r.json)) {
        std::fprintf(stderr, "native_tuner_eval: cannot write %s\n", out_path.c_str());
        return 1;
    }
    return 0;
}
