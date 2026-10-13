// SPDX-License-Identifier: GPL-3.0-or-later
//
// --host-fixture: what the bundles must produce when Sonora's own LV2 and
// VST3 hosts run them (ADR-0011 §1.2, the N1 checkpoint: "the golden tests
// run through the adapter, not only through the core").
//
// For each case it writes the input, the expected output and the
// plugin_driver arguments (plugins/host/tests/plugin_driver.cpp, scenario
// "render"). The expected output is the N0 reference: the Processor
// rendered offline through the harness, with the golden configuration of
// its suite, under the conditions the host creates and the plugin cannot
// see past:
//
// - Preroll. The host validates a chain with blocks of silence before the
//   first real block (plugins/host/src/chain.cpp, Chain::validate). A
//   processor with free-running state (the chorus's LFO) is that many
//   samples further on when the input starts, so the reference is rendered
//   after the same silence.
// - Parameter delivery. The driver's --set values reach the plugin in its
//   first block, where the adapter applies them as the initial state
//   (format/bridge.h); the reference sets them after prepare(), the same
//   way. VST3 carries them normalised (0..1, linear, as DPF maps them), so
//   the reference uses the value that comes back out of that round trip,
//   computed with DPF's own arithmetic.
// - Transport. The VST3 host sends a tempo (120 BPM, the delay follows
//   it); the LV2 host sends none, so the delay uses its own tempo
//   parameter.
//
// A difference from the committed golden WAV is printed for information:
// with no preroll and no host tempo, the two are the same render.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "sonora/measure/measure.h"
#include "sonora/native/chorus.h"
#include "sonora/native/compressor.h"
#include "sonora/native/delay.h"
#include "sonora/native/eq.h"
#include "sonora/native/gate.h"
#include "sonora/native/limiter.h"

namespace sonora::native::suites {
measure::Config eq_golden_config();
measure::Config compressor_golden_config();
measure::Config gate_golden_config();
measure::Config limiter_golden_config();
measure::Config delay_golden_config();
measure::Config chorus_golden_config();
}  // namespace sonora::native::suites

namespace sonora::measure {

namespace {

// ADR-0011 §3.4: golden files compare at a maximum absolute error of 1e-6.
// The bundles run the same compiled objects, so in practice the match is
// exact; the bar is the golden bar, not a looser one.
constexpr double kTolerance = 1e-6;
constexpr std::uint32_t kBlock = 128;  // plugin_driver's block
constexpr double kSeconds = 0.5;       // the golden input's length

struct Target {
    const char* name;
    std::function<std::unique_ptr<native::Processor>()> make;
    std::function<Config()> golden;
};

const Target kTargets[] = {
    {"eq", [] { return native::make_eq(); }, native::suites::eq_golden_config},
    {"compressor", [] { return native::make_compressor(); }, native::suites::compressor_golden_config},
    {"gate", [] { return native::make_gate(); }, native::suites::gate_golden_config},
    {"limiter", [] { return native::make_limiter(); }, native::suites::limiter_golden_config},
    {"delay", [] { return native::make_delay(); }, native::suites::delay_golden_config},
    {"chorus", [] { return native::make_chorus(); }, native::suites::chorus_golden_config},
};

// What DPF's VST3 wrapper makes of a normalised value
// (third_party/dpf/distrho/src/DistrhoPluginVST3.cpp,
// _setNormalizedPluginParameterValue, and ParameterRanges in
// DistrhoDetails.hpp), operation for operation, so the reference sees the
// float the plugin sees.
float dpf_vst3_plain(const native::ParamInfo& p, bool boolean, bool integer, float normalized) {
    const double n = normalized;
    float v;
    if (n <= 0.0) v = p.min;
    else if (n >= 1.0) v = p.max;
    else v = static_cast<float>(n * (p.max - p.min) + p.min);
    if (boolean) {
        const float mid = p.min + (p.max - p.min) / 2.f;
        v = v > mid ? p.max : p.min;
    } else if (integer) {
        v = static_cast<float>(static_cast<int>(v >= 0.0f ? v + 0.5f : v - 0.5f));
    } else {
        // A value within 1e-7 (normalised) of the one DPF already holds,
        // the default here, is dropped as unchanged: the plugin keeps the
        // exact default, not the round-tripped neighbour.
        double held = (static_cast<double>(p.def) - p.min) / (p.max - p.min);
        held = held <= 0.0 ? 0.0 : (held >= 1.0 ? 1.0 : held);
        if (std::fabs(held - n) < 0.0000001) v = p.def;
    }
    return v;
}

// One host --set argument, and the value the plugin ends up with.
struct Set {
    std::string arg;
    std::uint32_t index;
    float value;
};

Set make_set(const native::Descriptor& d, std::uint32_t index, float plain, bool vst3) {
    const native::ParamInfo& p = d.params[index];
    // How the glue declares it (format/src/dpf/plugin.cpp): index 0 is the
    // bypass designation, toggles are boolean, stepped values integer.
    const bool boolean = index == 0 || p.unit == native::Unit::Toggle;
    const bool integer = boolean || p.taper == native::Taper::Stepped;
    char buf[256];
    if (!vst3) {
        // LV2: the plain value on the port. The bypass is lv2:enabled, which
        // is 1 while processing.
        if (index == 0) std::snprintf(buf, sizeof buf, "0:lv2_enabled=%.9g", 1.0f - plain);
        else std::snprintf(buf, sizeof buf, "0:%s=%.9g", p.id, plain);
        return {buf, index, plain};
    }
    // VST3: by title, normalised. The host parses it with strtof, so the
    // float printed here is the float the plugin receives.
    const float n = static_cast<float>((static_cast<double>(plain) - p.min) /
                                       (static_cast<double>(p.max) - p.min));
    std::snprintf(buf, sizeof buf, "0:%s=%.9g", index == 0 ? "Bypass" : p.name, n);
    return {buf, index, dpf_vst3_plain(p, boolean, integer, n)};
}

bool write_f32(const std::string& path, const Chans& x, std::size_t from) {
    // Planar: every sample of channel 0, then channel 1. The driver reads
    // the file length to know the frame count.
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    bool ok = true;
    for (const auto& c : x) {
        const std::size_t n = c.size() - from;
        ok = ok && std::fwrite(c.data() + from, sizeof(float), n, f) == n;
    }
    return std::fclose(f) == 0 && ok;
}

int write_case(const Target& t, const std::string& dir, const std::string& golden_dir,
               const char* case_name, const Config& cfg, double fs, bool vst3,
               std::uint32_t preroll, double vst3_tempo) {
    const char* fmt = vst3 ? "vst3" : "lv2";
    char stem[160];
    std::snprintf(stem, sizeof stem, "%s/%s-%s-%d", dir.c_str(), t.name, case_name, static_cast<int>(fs));
    const std::string in_path = std::string(stem) + ".in.f32";
    const std::string out_path = std::string(stem) + "-" + fmt + ".expected.f32";
    const std::string args_path = std::string(stem) + "-" + fmt + ".args";

    auto p = t.make();
    const native::Descriptor& d = p->descriptor();
    std::vector<Set> sets;
    for (const auto& kv : cfg) {
        const std::uint32_t i = param_index(*p, kv.first);
        if (d.params[i].reconfigures) {
            // Through a host it would arrive in the first block and wait for
            // the next activation; the fixture would then test something
            // else than it says.
            std::fprintf(stderr, "host fixture %s: '%s' reconfigures; not settable from --set\n",
                         t.name, kv.first);
            return 1;
        }
        sets.push_back(make_set(d, i, kv.second, vst3));
    }

    // The host's max_frames is plugin_driver's 1024; blocks are 128.
    if (!p->prepare(fs, 1024, 2)) return 1;
    for (const Set& s : sets) p->set_param(s.index, s.value);
    const std::uint32_t latency = p->latency();

    const Chans input = golden_input(fs, kSeconds);
    Chans padded = silence(fs, 0.0, 2);
    for (std::size_t c = 0; c < 2; ++c) {
        padded[c].assign(preroll, 0.0f);
        padded[c].insert(padded[c].end(), input[c].begin(), input[c].end());
    }
    RenderOptions opt;
    opt.block = kBlock;
    if (vst3 && vst3_tempo > 0.0) {
        opt.transport.tempo_bpm = vst3_tempo;
        opt.transport.tempo_valid = true;
    }
    const Chans out = render(*p, padded, {}, opt);

    if (!write_f32(in_path, input, 0) || !write_f32(out_path, out, preroll)) {
        std::fprintf(stderr, "host fixture: cannot write %s\n", stem);
        return 1;
    }
    std::FILE* a = std::fopen(args_path.c_str(), "w");
    if (a == nullptr) return 1;
    // One argument per line: VST3 titles have spaces.
    std::fprintf(a, "--rate\n%d\n--input\n%s\n--expected\n%s\n--tolerance\n%.3g\n--expect-latency\n%u\n",
                 static_cast<int>(fs), in_path.c_str(), out_path.c_str(), kTolerance, latency);
    for (const Set& s : sets) std::fprintf(a, "--set\n%s\n", s.arg.c_str());
    std::fclose(a);

    if (!golden_dir.empty() && !vst3 && std::strcmp(case_name, "golden") == 0) {
        char file[64];
        std::snprintf(file, sizeof file, "/%s-%d.wav", t.name, static_cast<int>(fs));
        Chans ref;
        double rfs = 0.0;
        std::string err;
        if (read_wav_f32(golden_dir + file, ref, rfs, err) && ref.size() == 2 &&
            ref[0].size() + preroll == out[0].size()) {
            double worst = 0.0;
            for (std::size_t c = 0; c < 2; ++c) {
                for (std::size_t i = 0; i < ref[c].size(); ++i)
                    worst = std::fmax(worst, std::fabs(static_cast<double>(out[c][i + preroll]) - ref[c][i]));
            }
            std::printf("  %s %d Hz: reference after %u samples of preroll vs the N0 golden file: "
                        "max |difference| %.3g\n",
                        t.name, static_cast<int>(fs), preroll, worst);
        }
    }
    std::printf("  %s\n", args_path.c_str());
    return 0;
}

}  // namespace

int host_fixture(const std::string& which, const std::string& dir, const std::string& golden_dir,
                 std::uint32_t preroll, double vst3_tempo) {
    for (const Target& t : kTargets) {
        if (which != t.name) continue;
        Config bypassed = t.golden();
        bypassed.push_back({"bypass", 1.0f});
        int bad = 0;
        for (const bool vst3 : {false, true}) {
            for (double fs : kRates) {
                bad += write_case(t, dir, golden_dir, "golden", t.golden(), fs, vst3, preroll, vst3_tempo);
            }
            // Bypass delays by the reported latency through the format as
            // well (ADR-0011 §1.5); one rate is enough, latency per rate is
            // covered by the golden cases.
            bad += write_case(t, dir, golden_dir, "bypass", bypassed, 48000.0, vst3, preroll, vst3_tempo);
        }
        return bad == 0 ? 0 : 1;
    }
    std::fprintf(stderr, "host fixture: unknown processor '%s'\n", which.c_str());
    return 2;
}

}  // namespace sonora::measure
