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
// The cases (ADR-0011 N1):
//
// - golden at 44.1, 48 and 96 kHz, and bypass at 48 kHz: the stereo bundle.
// - mono: the mono bundle on a one-channel chain, the golden configuration.
// - key, key-mono: the compressor's and gate's key input. The external key
//   is switched on and a key signal unlike the input is routed to it
//   (host --sidechain), so the output follows the key, not the input.
// - nokey, nokey-mono: the external key switched on and nothing routed to
//   it. A key input the host leaves unconnected (LV2) or inactive (VST3)
//   is no key, and the processor keys from its own input (format/bridge.h,
//   ADR-0011 Amendment 2 item 3), so the reference is rendered with no
//   side-chain at all. The fixture fails unless a silent key would change
//   that reference: the case must tell "no key" from "a silent key".
// - automation: one parameter moved after the first block, on the
//   parameter ring, as the engine's automation will (the driver's
//   --change). The plugin smooths it from the block it arrives in: through
//   the LV2 port, and through VST3 at sample offset 0, which DPF applies
//   before run() (ADR-0011 Amendment 2: only a nonzero offset is late).
// - reconfigure: a value that changes the latency (the EQ's zero-latency
//   mode, a non-default limiter look-ahead), set at load and taken by a
//   host that re-activates the instance (host --reactivate). The reported
//   latency is checked against the reference's, and the preroll is the
//   host's two validations.
//
// With --golden, the LV2 golden reference is also compared with the
// committed golden WAV at the golden bar (1e-6), and a difference, an
// unreadable file or a length mismatch fails the fixture, so a bundle that
// matches its reference also matches its golden file. Only a processor
// whose state moves during the preroll (Target::preroll_free) is exempt,
// and its difference is printed.
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
// The automation case's change goes on the ring before this input block:
// well after the first block (whose values are the initial state), well
// before the end.
constexpr std::uint32_t kChangeBlock = 8;

// A parameter's new plain value, moved mid-render.
struct Change {
    const char* id;
    float value;
};

struct Target {
    const char* name;
    std::function<std::unique_ptr<native::Processor>()> make;
    std::function<Config()> golden;
    // Whether silence before the input leaves the processor exactly where
    // it starts. Then the reference after the host's preroll must equal the
    // committed golden file, and the fixture fails when it does not. The
    // chorus's LFO runs through the preroll, so its reference is a
    // different render and the difference is only printed.
    bool preroll_free;
    // An audible move away from the golden configuration.
    Change automation;
    // The values of the reconfigure case; empty for a processor without a
    // latency-changing parameter.
    Config reconfigure;
};

const Target kTargets[] = {
    {"eq", [] { return native::make_eq(); }, native::suites::eq_golden_config, true,
     {"bell1_gain", -6.0f}, {{"mode", 0.0f}}},
    {"compressor", [] { return native::make_compressor(); }, native::suites::compressor_golden_config, true,
     {"threshold", -40.0f}, {}},
    {"gate", [] { return native::make_gate(); }, native::suites::gate_golden_config, true,
     {"threshold", -10.0f}, {}},
    {"limiter", [] { return native::make_limiter(); }, native::suites::limiter_golden_config, true,
     {"ceiling", -4.0f}, {{"lookahead", 3.0f}}},
    {"delay", [] { return native::make_delay(); }, native::suites::delay_golden_config, true,
     {"mix", 70.0f}, {}},
    {"chorus", [] { return native::make_chorus(); }, native::suites::chorus_golden_config, false,
     {"mix", 20.0f}, {}},
};

struct Case {
    const char* name;
    double fs;
    std::uint32_t channels;  // 2: the stereo bundle; 1: the mono one
    bool key;                // external key on, a key signal routed to it
    bool unrouted_key;       // external key on, nothing routed to it
    bool automation;         // Target::automation, before kChangeBlock
    bool reactivate;         // Target::reconfigure, host --reactivate
    Config extra;            // on top of the golden configuration
};

// What DPF's VST3 wrapper makes of a normalised value
// (third_party/dpf/distrho/src/DistrhoPluginVST3.cpp,
// _setNormalizedPluginParameterValue, and ParameterRanges in
// DistrhoDetails.hpp), operation for operation, so the reference sees the
// float the plugin sees. `held` is the value the plugin holds when it
// arrives: DPF drops a value it reads as unchanged.
float dpf_vst3_plain(const native::ParamInfo& p, bool boolean, bool integer, float normalized, float held) {
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
        // A value within 1e-7 (normalised) of the one DPF already holds is
        // dropped as unchanged: the plugin keeps the exact value it had,
        // not the round-tripped neighbour.
        double h = (static_cast<double>(held) - p.min) / (p.max - p.min);
        h = h <= 0.0 ? 0.0 : (h >= 1.0 ? 1.0 : h);
        if (std::fabs(h - n) < 0.0000001) v = held;
    }
    return v;
}

// One value as a host argument, and the value the plugin ends up with.
struct Set {
    std::string arg;  // INDEX:SYMBOL=VALUE without the instance for a change
    std::uint32_t index;
    float value;
};

Set make_set(const native::Descriptor& d, std::uint32_t index, float plain, float held, bool vst3,
             bool with_instance) {
    const native::ParamInfo& p = d.params[index];
    // How the glue declares it (format/src/dpf/plugin.cpp): index 0 is the
    // bypass designation, toggles are boolean, stepped values integer.
    const bool boolean = index == 0 || p.unit == native::Unit::Toggle;
    const bool integer = boolean || p.taper == native::Taper::Stepped;
    const char* instance = with_instance ? "0:" : "";
    char buf[256];
    if (!vst3) {
        // LV2: the plain value on the port. The bypass is lv2:enabled, which
        // is 1 while processing.
        if (index == 0) std::snprintf(buf, sizeof buf, "%slv2_enabled=%.9g", instance, 1.0f - plain);
        else std::snprintf(buf, sizeof buf, "%s%s=%.9g", instance, p.id, plain);
        return {buf, index, plain};
    }
    // VST3: by title, normalised. The host parses it with strtof, so the
    // float printed here is the float the plugin receives.
    const float n = static_cast<float>((static_cast<double>(plain) - p.min) /
                                       (static_cast<double>(p.max) - p.min));
    std::snprintf(buf, sizeof buf, "%s%s=%.9g", instance, index == 0 ? "Bypass" : p.name, n);
    return {buf, index, dpf_vst3_plain(p, boolean, integer, n, held)};
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

// The key routed to the side-chain: noise in two bursts with a gap, so a
// compressor keyed by it pumps and a gate keyed by it opens and closes on a
// schedule the input does not have.
Chans key_signal(double fs, std::uint32_t channels) {
    Chans k = noise(fs, -6.0, kSeconds, 7, channels);
    for (auto& c : k) {
        for (std::size_t i = 0; i < c.size(); ++i) {
            const double t = static_cast<double>(i) / fs;
            if (t >= 0.15 && t < 0.3) c[i] = 0.0f;
        }
    }
    return k;
}

Chans with_preroll(const Chans& x, std::uint32_t preroll) {
    Chans padded(x.size());
    for (std::size_t c = 0; c < x.size(); ++c) {
        padded[c].assign(preroll, 0.0f);
        padded[c].insert(padded[c].end(), x[c].begin(), x[c].end());
    }
    return padded;
}

int write_case(const Target& t, const std::string& dir, const std::string& golden_dir, const Case& k,
               bool vst3, std::uint32_t preroll, double vst3_tempo) {
    const char* fmt = vst3 ? "vst3" : "lv2";
    char stem[192];
    std::snprintf(stem, sizeof stem, "%s/%s-%s-%d", dir.c_str(), t.name, k.name, static_cast<int>(k.fs));
    const std::string in_path = std::string(stem) + ".in.f32";
    const std::string key_path = std::string(stem) + ".key.f32";
    const std::string out_path = std::string(stem) + "-" + fmt + ".expected.f32";
    const std::string args_path = std::string(stem) + "-" + fmt + ".args";

    auto p = t.make();
    const native::Descriptor& d = p->descriptor();
    Config cfg = t.golden();
    cfg.insert(cfg.end(), k.extra.begin(), k.extra.end());
    if (k.key || k.unrouted_key) cfg.push_back({"external_sc", 1.0f});
    if (k.reactivate) cfg.insert(cfg.end(), t.reconfigure.begin(), t.reconfigure.end());
    std::vector<Set> sets;
    for (const auto& kv : cfg) {
        const std::uint32_t i = param_index(*p, kv.first);
        if (d.params[i].reconfigures && !k.reactivate) {
            // Through a host it would arrive in the first block and wait for
            // the next activation; without --reactivate the case would test
            // something else than it says.
            std::fprintf(stderr, "host fixture %s: '%s' reconfigures; only the reconfigure case sets it\n",
                         t.name, kv.first);
            return 1;
        }
        // A fresh instance holds the defaults when the first block brings
        // these.
        sets.push_back(make_set(d, i, kv.second, d.params[i].def, vst3, true));
    }

    // The host's max_frames is plugin_driver's 1024; blocks are 128. Every
    // value is set before prepare(): through a host the reconfiguring ones
    // are taken at the re-activation, the others are where the instance
    // starts either way.
    const auto configure = [&](native::Processor& q) {
        for (const Set& s : sets) q.set_param(s.index, s.value);
        if (!q.prepare(k.fs, 1024, k.channels)) return false;
        for (const Set& s : sets) q.set_param(s.index, s.value);
        return true;
    };
    if (!configure(*p)) return 1;
    const std::uint32_t latency = p->latency();

    // The host validates twice when it re-activates (host --reactivate).
    const std::uint32_t pre = k.reactivate ? 2 * preroll : preroll;
    Chans input = golden_input(k.fs, kSeconds);
    input.resize(k.channels);
    const Chans padded = with_preroll(input, pre);
    Chans key, padded_key;
    if (k.key) {
        key = key_signal(k.fs, k.channels);
        padded_key = with_preroll(key, pre);
    }

    std::vector<Event> events;
    Set change{};
    if (k.automation) {
        const std::uint32_t i = param_index(*p, t.automation.id);
        change = make_set(d, i, t.automation.value, p->param(i), vst3, false);
        events.push_back({pre + static_cast<std::uint64_t>(kChangeBlock) * kBlock, i, change.value});
    }

    RenderOptions opt;
    opt.block = kBlock;
    if (vst3 && vst3_tempo > 0.0) {
        opt.transport.tempo_bpm = vst3_tempo;
        opt.transport.tempo_valid = true;
    }
    if (k.key) opt.sidechain = &padded_key;
    const Chans out = render(*p, padded, events, opt);

    if (k.unrouted_key) {
        // What a host that fed the key input zeros would get: if it equals
        // the reference, this case cannot catch a silent key.
        auto q = t.make();
        if (!configure(*q)) return 1;
        Chans silent(k.channels, std::vector<float>(padded[0].size(), 0.0f));
        RenderOptions sopt = opt;
        sopt.sidechain = &silent;
        const Chans keyed = render(*q, padded, events, sopt);
        double apart = 0.0;
        for (std::size_t c = 0; c < out.size(); ++c) {
            for (std::size_t i = pre; i < out[c].size(); ++i)
                apart = std::fmax(apart, std::fabs(static_cast<double>(out[c][i]) - keyed[c][i]));
        }
        std::printf("  %s %s: no key against a silent key: max |difference| %.3g\n", t.name, k.name, apart);
        if (!(apart > 1e-3)) {
            std::fprintf(stderr, "host fixture %s %s: a silent key renders the same as no key; the case "
                         "proves nothing\n", t.name, k.name);
            return 1;
        }
    }

    if (!write_f32(in_path, input, 0) || !write_f32(out_path, out, pre) ||
        (k.key && !write_f32(key_path, key, 0))) {
        std::fprintf(stderr, "host fixture: cannot write %s\n", stem);
        return 1;
    }
    std::FILE* a = std::fopen(args_path.c_str(), "w");
    if (a == nullptr) return 1;
    // One argument per line: VST3 titles have spaces.
    std::fprintf(a, "--rate\n%d\n--channels\n%u\n--input\n%s\n--expected\n%s\n--tolerance\n%.3g\n"
                    "--expect-latency\n%u\n",
                 static_cast<int>(k.fs), k.channels, in_path.c_str(), out_path.c_str(), kTolerance, latency);
    for (const Set& s : sets) std::fprintf(a, "--set\n%s\n", s.arg.c_str());
    if (k.key) std::fprintf(a, "--key\n%s\n--key-channels\n%u\n", key_path.c_str(), k.channels);
    if (k.automation) std::fprintf(a, "--change\n%u:%s\n", kChangeBlock, change.arg.c_str());
    if (k.reactivate) std::fprintf(a, "--reactivate\n1\n");
    std::fclose(a);

    // The LV2 golden case has no host tempo, so it is the golden render
    // again, after the preroll: the expected output is tied to the
    // committed golden file, not only to whatever the Processor does today.
    if (!golden_dir.empty() && !vst3 && std::strcmp(k.name, "golden") == 0) {
        char file[64];
        std::snprintf(file, sizeof file, "/%s-%d.wav", t.name, static_cast<int>(k.fs));
        Chans ref;
        double rfs = 0.0;
        std::string err;
        if (!read_wav_f32(golden_dir + file, ref, rfs, err)) {
            std::fprintf(stderr, "host fixture %s: cannot read golden file %s%s: %s\n", t.name,
                         golden_dir.c_str(), file, err.c_str());
            return 1;
        }
        if (ref.size() != 2 || ref[0].size() + pre != out[0].size() || rfs != k.fs) {
            std::fprintf(stderr, "host fixture %s: golden file %s%s is %zu channels of %zu frames at %g Hz; "
                         "the reference is 2 of %zu at %g Hz\n",
                         t.name, golden_dir.c_str(), file, ref.size(), ref.empty() ? 0 : ref[0].size(), rfs,
                         out[0].size() - pre, k.fs);
            return 1;
        }
        double worst = 0.0;
        for (std::size_t c = 0; c < 2; ++c) {
            for (std::size_t i = 0; i < ref[c].size(); ++i)
                worst = std::fmax(worst, std::fabs(static_cast<double>(out[c][i + pre]) - ref[c][i]));
        }
        std::printf("  %s %d Hz: reference after %u samples of preroll vs the N0 golden file: "
                    "max |difference| %.3g%s\n",
                    t.name, static_cast<int>(k.fs), pre, worst,
                    t.preroll_free ? "" : " (preroll-dependent, not asserted)");
        if (t.preroll_free && !(worst <= kTolerance)) {
            std::fprintf(stderr, "host fixture %s %d Hz: reference differs from the golden file by %.3g "
                         "(bar %.3g)\n", t.name, static_cast<int>(k.fs), worst, kTolerance);
            return 1;
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
        std::vector<Case> cases;
        for (double fs : kRates) cases.push_back({"golden", fs, 2, false, false, false, false, {}});
        // Bypass delays by the reported latency through the format as
        // well (ADR-0011 §1.5); one rate is enough, latency per rate is
        // covered by the golden cases.
        cases.push_back({"bypass", 48000.0, 2, false, false, false, false, {{"bypass", 1.0f}}});
        cases.push_back({"mono", 48000.0, 1, false, false, false, false, {}});
        cases.push_back({"automation", 48000.0, 2, false, false, true, false, {}});
        if (t.make()->descriptor().sidechain) {
            cases.push_back({"key", 48000.0, 2, true, false, false, false, {}});
            cases.push_back({"key-mono", 48000.0, 1, true, false, false, false, {}});
            cases.push_back({"nokey", 48000.0, 2, false, true, false, false, {}});
            cases.push_back({"nokey-mono", 48000.0, 1, false, true, false, false, {}});
        }
        if (!t.reconfigure.empty()) cases.push_back({"reconfigure", 48000.0, 2, false, false, false, true, {}});
        int bad = 0;
        for (const bool vst3 : {false, true}) {
            for (const Case& k : cases) bad += write_case(t, dir, golden_dir, k, vst3, preroll, vst3_tempo);
        }
        return bad == 0 ? 0 : 1;
    }
    std::fprintf(stderr, "host fixture: unknown processor '%s'\n", which.c_str());
    return 2;
}

}  // namespace sonora::measure
