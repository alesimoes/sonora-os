// SPDX-License-Identifier: GPL-3.0-or-later
//
// The checks every processor runs (ADR-0011 §3.2): latency, bypass,
// non-finite input, denormals, zipper, golden files and CPU.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "sonora/dsp/denormals.h"
#include "sonora/measure/measure.h"
#include "sonora/native/param.h"

namespace sonora::measure {

namespace {

std::string rate_name(double fs) {
    char b[32];
    std::snprintf(b, sizeof b, "%g kHz", fs / 1000.0);
    return b;
}

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const auto i = static_cast<std::size_t>(std::ceil(p * (v.size() - 1)));
    return v[std::min(i, v.size() - 1)];
}

std::unique_ptr<native::Processor> prepared(Suite& s, double fs, const Config& c,
                                            std::uint32_t block = 128) {
    auto p = s.make();
    apply(*p, c);
    if (!p->prepare(fs, block, 2)) {
        s.fail(s.name() + ": prepare(" + rate_name(fs) + ", stereo) failed");
        return nullptr;
    }
    return p;
}

}  // namespace

void check_latency(Suite& s, const std::vector<std::pair<std::string, Config>>& configs) {
    for (const auto& [label, cfg] : configs) {
        for (double fs : kRates) {
            auto p = prepared(s, fs, cfg);
            if (!p) continue;
            Chans in = noise(fs, -30.0, 0.25, 77u);
            band_limit_and_fade(in, fs);
            in = concat({in, silence(fs, 0.25)});
            const Chans out = render(*p, in);
            const long reported = static_cast<long>(p->latency());
            const long measured = xcorr_lag(in[0], out[0], reported + 2048);
            s.check("latency, " + label + ", " + rate_name(fs) + ": measured - reported",
                    static_cast<double>(measured - reported), 0.0, measured == reported, "samples",
                    "reported " + std::to_string(reported));

            // Bypass: the input, delayed by exactly the reported latency.
            auto b = prepared(s, fs, cfg);
            if (!b) continue;
            b->set_param(0, 1.0f);
            const Chans by = render(*b, in);
            double worst = 0.0;
            for (std::size_t c = 0; c < 2; ++c) {
                for (std::size_t i = 0; i < by[c].size(); ++i) {
                    const float want = i >= static_cast<std::size_t>(reported) ? in[c][i - reported] : 0.0f;
                    worst = std::fmax(worst, std::fabs(by[c][i] - want));
                }
            }
            s.check("bypass = input delayed by latency, " + label + ", " + rate_name(fs),
                    worst, 0.0, worst == 0.0, "abs");
        }
    }
}

void check_nonfinite(Suite& s, const Config& c) {
    const double fs = 48000.0;
    Chans in = noise(fs, -12.0, 1.0, 5u);
    band_limit_and_fade(in, fs);
    Chans bad = in, zeroed = in;
    for (std::size_t at : {1000u, 5003u, 9010u}) {
        bad[0][at] = at == 1000 ? std::nanf("") : (at == 5003 ? INFINITY : -INFINITY);
        bad[1][at + 7] = std::nanf("");
        zeroed[0][at] = 0.0f;
        zeroed[1][at + 7] = 0.0f;
    }
    auto p = prepared(s, fs, c);
    auto q = prepared(s, fs, c);
    if (!p || !q) return;
    const Chans y = render(*p, bad);
    const Chans r = render(*q, zeroed);
    bool finite = true;
    double diff = 0.0;
    for (std::size_t ch = 0; ch < 2; ++ch) {
        for (std::size_t i = 0; i < y[ch].size(); ++i) {
            finite &= std::isfinite(y[ch][i]);
            diff = std::fmax(diff, std::fabs(y[ch][i] - r[ch][i]));
        }
    }
    s.check("NaN/Inf in: output finite", finite ? 1.0 : 0.0, 1.0, finite, "bool");
    s.check("NaN/Inf in: same as silence in their place", diff, 0.0, diff == 0.0, "abs",
            "recovery within the block");
    // A finite but absurd input (1e30): output stays finite, and a block
    // later the processor is back to normal.
    Chans huge = in;
    for (std::size_t i = 2000; i < 2010; ++i) huge[0][i] = 1e30f;
    auto h = prepared(s, fs, c);
    if (!h) return;
    const Chans yh = render(*h, huge);
    bool hf = true;
    for (const auto& ch : yh) {
        for (float v : ch) hf &= std::isfinite(v);
    }
    s.check("1e30 in: output finite", hf ? 1.0 : 0.0, 1.0, hf, "bool");
}

void check_denormals(Suite& s, const Config& c) {
    const double fs = 48000.0;
    auto p = prepared(s, fs, c);
    if (!p) return;
    Chans in = concat({noise(fs, -6.0, 10.0, 9u), silence(fs, 10.0)});
    std::vector<double> us;
    RenderOptions opt;
    opt.block_us = &us;
    render(*p, in, {}, opt);
    const std::size_t blocks_per_s = static_cast<std::size_t>(fs / 128.0);
    const std::vector<double> active(us.begin() + 2 * blocks_per_s, us.begin() + 10 * blocks_per_s);
    const std::vector<double> quiet(us.begin() + 15 * blocks_per_s, us.end());
    const double ratio = percentile(quiet, 0.5) / percentile(active, 0.5);
    s.check_le("denormals: per-block CPU, silence after signal / active (median)", ratio,
               s.bar("denormal_ratio"), "ratio",
               dsp::flush_to_zero_supported() ? "FTZ/DAZ on" : "no FTZ on this architecture");
}

Chans zipper_tone(double fs, double seconds) {
    // A 100 Hz tone at -6 dBFS, faded in over 10 ms so its own start is not
    // a click (a delay would echo one into the measured window).
    Chans x = sine(fs, 100.0, -6.0, seconds);
    const auto f = static_cast<std::size_t>(0.01 * fs);
    for (auto& ch : x) {
        for (std::size_t i = 0; i < f; ++i) ch[i] *= static_cast<float>(0.5 - 0.5 * std::cos(3.14159265358979 * i / f));
    }
    return x;
}

void check_zipper(Suite& s, const Config& c, const std::string& label) {
    const json::Value* list = s.quality("zipper");
    if (list == nullptr || !list->is_array()) {
        s.fail("quality.json has no 'zipper' list");
        return;
    }
    const double bar = s.bar("zipper_dbfs");
    const double fs = 48000.0;
    // A 100 Hz tone at -6 dBFS. Smoothly moving any parameter keeps its
    // energy near 100 Hz; a step or a click puts energy above 5 kHz.
    const Chans in = zipper_tone(fs, 3.25);
    for (const json::Value& item : list->items()) {
        const std::string id = item.text();
        auto p = prepared(s, fs, c);
        if (!p) return;
        const std::uint32_t idx = param_index(*p, id.c_str());
        const native::ParamInfo& info = p->descriptor().params[idx];
        if (!info.automatable) {
            s.fail("zipper list names non-automatable '" + id + "'");
            continue;
        }
        // From the default down to the bottom, up to the top and back, one
        // full range per second, one event per 128-frame block (a host
        // delivering automation at block rate), after 0.25 s.
        std::vector<Event> ev;
        const double n0 = native::to_normalized(info, info.def);
        const std::uint64_t start = static_cast<std::uint64_t>(0.25 * fs);
        for (std::uint64_t f = start; f < in[0].size(); f += 128) {
            const double t = static_cast<double>(f - start) / fs;
            double n;
            if (t < n0) n = n0 - t;                       // down to the bottom
            else if (t < n0 + 1.0) n = t - n0;            // up to the top
            else if (t < 2.0) n = 1.0 - (t - n0 - 1.0);   // back to the default
            else n = n0;
            n = std::fmin(1.0, std::fmax(0.0, n));
            ev.push_back({f, idx, static_cast<float>(native::from_normalized(info, n))});
        }
        const Chans out = render(*p, in, ev);
        double worst = 0.0;
        for (const auto& ch : out) worst = std::fmax(worst, hf_residue_peak(ch, fs, start));
        s.check_le("zipper" + label + ": full-range sweep of '" + id + "', HF residue peak", to_db(worst), bar, "dBFS");
    }
}

void check_golden(Suite& s, const Config& c) {
    const double seconds = 0.5;
    const double tol = s.bar("golden_abs");
    for (double fs : kRates) {
        auto p = prepared(s, fs, c);
        if (!p) return;
        const Chans out = render(*p, golden_input(fs, seconds));
        char file[64];
        std::snprintf(file, sizeof file, "/%s-%d.wav", s.name().c_str(), static_cast<int>(fs));
        const std::string path = s.args().golden_dir + file;
        if (s.args().regenerate_golden) {
            if (!write_wav_f32(path, out, fs)) s.fail("cannot write " + path);
            else s.info("golden regenerated, " + rate_name(fs), 0.0, "", path);
            continue;
        }
        Chans ref;
        double rfs = 0.0;
        std::string err;
        if (!read_wav_f32(path, ref, rfs, err)) {
            s.fail("golden: " + err);
            continue;
        }
        double worst = 0.0;
        bool shape = ref.size() == out.size() && rfs == fs;
        for (std::size_t ch = 0; shape && ch < out.size(); ++ch) {
            shape = ref[ch].size() == out[ch].size();
            for (std::size_t i = 0; shape && i < out[ch].size(); ++i) {
                worst = std::fmax(worst, std::fabs(static_cast<double>(out[ch][i]) - ref[ch][i]));
            }
        }
        if (!shape) {
            s.fail("golden " + path + ": rate, channels or length differ");
            continue;
        }
        s.check_le("golden render, " + rate_name(fs) + ": max |difference|", worst, tol, "abs");
    }
}

void check_cpu(Suite& s, const Config& c, const std::string& label) {
    if (!s.args().cpu) return;
    const double fs = 48000.0;
    // Programme-like load: noise with a sweep under it.
    const double secs = s.args().cpu_seconds;
    Chans in = noise(fs, -12.0, secs, 31u);
    const Chans sw = log_sweep(fs, 20.0, 20000.0, -12.0, secs, 2);
    for (std::size_t ch = 0; ch < 2; ++ch) {
        for (std::size_t i = 0; i < in[ch].size(); ++i) in[ch][i] = 0.5f * (in[ch][i] + sw[ch][i]);
    }
    // Five identical renders from a fresh processor. The first run's
    // figures are the raw ones, preemption and all. The per-block minimum
    // over the five is the cost of the code path itself: a deterministic
    // render does the same work on block k every time, and a host's
    // scheduling noise rarely hits the same block five times.
    constexpr int kRuns = 5;
    std::vector<double> raw, best;
    for (int r = 0; r < kRuns; ++r) {
        auto p = prepared(s, fs, c);
        if (!p) return;
        std::vector<double> us;
        RenderOptions opt;
        opt.block_us = &us;
        render(*p, in, {}, opt);
        // The first second warms caches and branch predictors.
        us.erase(us.begin(), us.begin() + static_cast<long>(fs / 128.0));
        if (r == 0) {
            raw = us;
            best = us;
        } else {
            for (std::size_t k = 0; k < us.size(); ++k) best[k] = std::min(best[k], us[k]);
        }
    }
    const std::string what = "CPU per block, q128 48 kHz stereo" + label + ": ";
    s.info(what + "p50", percentile(best, 0.5), "us", "per-block minimum of 5 runs");
    s.check_le(what + "p99.9", percentile(best, 0.999), s.bar("cpu.p999_us"), "us", "per-block minimum of 5 runs");
    s.check_le(what + "max", percentile(best, 1.0), s.bar("cpu.max_us"), "us", "per-block minimum of 5 runs");
    s.info(what + "p99.9, single run", percentile(raw, 0.999), "us", "includes the host's scheduling");
    s.info(what + "max, single run", percentile(raw, 1.0), "us", "includes the host's scheduling");
}

void check_common(Suite& s, const Config& golden,
                  const std::vector<std::pair<std::string, Config>>& latency_configs,
                  const Config* zipper) {
    check_latency(s, latency_configs);
    check_nonfinite(s, golden);
    check_denormals(s, golden);
    check_zipper(s, zipper != nullptr ? *zipper : golden, "");
    check_golden(s, golden);
    check_cpu(s, golden, "");
}

}  // namespace sonora::measure
