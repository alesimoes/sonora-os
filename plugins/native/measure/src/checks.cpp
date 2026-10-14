// SPDX-License-Identifier: GPL-3.0-or-later
//
// The checks every processor runs (ADR-0011 §3.2): latency, bypass,
// non-finite input, denormals, zipper, golden files and CPU.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "sonora/dsp/denormals.h"
#include "sonora/measure/measure.h"
#include "sonora/native/param.h"
#include "sonora/rt.h"

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

// ProcessorBase::process() calls on_reset() itself when the output goes
// non-finite, so every processor's on_reset runs on the audio thread. A
// processor cannot be made to blow up from its input (the base class
// sanitises it), so the RTSan job reaches each on_reset this way instead.
void reset_on_audio_thread(native::Processor& p) noexcept SONORA_RT { p.reset(); }

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
    // A finite but absurd input (1e30, a broken upstream processor rather
    // than a signal): output stays finite, and it must not leave the
    // processor deaf or ringing for longer than a block. A limiter would
    // otherwise sit 600 dB down for its release, and a resonant EQ ring at
    // 1e30 for seconds. Asserted as the strictest form of "back to normal
    // within a block": the output is the render with silence in those
    // samples' place, exactly, from the block after the burst on.
    Chans huge = in, hole = in;
    for (std::size_t i = 2000; i < 2010; ++i) {
        huge[0][i] = 1e30f;
        huge[1][i + 3] = -1e30f;
        hole[0][i] = 0.0f;
        hole[1][i + 3] = 0.0f;
    }
    auto h = prepared(s, fs, c);
    auto hz = prepared(s, fs, c);
    if (!h || !hz) return;
    const Chans yh = render(*h, huge);
    const Chans yz = render(*hz, hole);
    bool hf = true;
    double hdiff = 0.0;
    const std::size_t next_block = (2013 / 128 + 1) * 128;
    for (std::size_t ch = 0; ch < 2; ++ch) {
        for (std::size_t i = 0; i < yh[ch].size(); ++i) {
            hf &= std::isfinite(yh[ch][i]);
            if (i >= next_block) hdiff = std::fmax(hdiff, std::fabs(yh[ch][i] - yz[ch][i]));
        }
    }
    s.check("1e30 in: output finite", hf ? 1.0 : 0.0, 1.0, hf, "bool");
    s.check("1e30 in: from the next block on, same as silence in its place", hdiff, 0.0, hdiff == 0.0, "abs",
            "back to normal within a block");
}

void check_reset(Suite& s, const Config& c) {
    const double fs = 48000.0;
    Chans in = noise(fs, -12.0, 0.5, 13u);
    band_limit_and_fade(in, fs);
    auto p = prepared(s, fs, c);
    auto q = prepared(s, fs, c);
    if (!p || !q) return;
    render(*p, in);
    reset_on_audio_thread(*p);
    const Chans y = render(*p, in);
    const Chans r = render(*q, in);
    double diff = 0.0;
    for (std::size_t ch = 0; ch < 2; ++ch) {
        for (std::size_t i = 0; i < y[ch].size(); ++i) diff = std::fmax(diff, std::fabs(y[ch][i] - r[ch][i]));
    }
    s.check("reset on the audio thread: same as freshly prepared", diff, 0.0, diff == 0.0, "abs",
            "on_reset clears all signal state");
}

void check_denormals(Suite& s, const Config& c) {
    // ADR-0011 §3.2: per-block CPU after 10 s of silence following signal,
    // against the active figure. The two are measured side by side, one
    // block of each in turn (the order alternating), rather than one after
    // the other: on a shared machine, or with ctest running tests in
    // parallel, the load changes between the 2nd and the 15th second far
    // more than a 1.2x bar allows, and a sequential ratio read 1.49 on a
    // delay costing 2.4 us a block with nothing wrong in it.
    const double fs = 48000.0;
    auto quiet = prepared(s, fs, c);
    auto active = prepared(s, fs, c);
    if (!quiet || !active) return;
    const Chans q_in = concat({noise(fs, -6.0, 10.0, 9u), silence(fs, 10.0)});
    const Chans a_in = noise(fs, -6.0, 20.0, 9u);
    const std::size_t n = q_in[0].size();
    Chans out(2, std::vector<float>(128, 0.0f));
    float* op[2] = {out[0].data(), out[1].data()};
    std::vector<double> q_us, a_us;
    const std::size_t blocks_per_s = static_cast<std::size_t>(fs / 128.0);
    auto timed = [&](native::Processor& p, const Chans& in, std::size_t pos) {
        const float* ip[2] = {&in[0][pos], &in[1][pos]};
        const native::AudioBuffers io{ip, op, nullptr, 2, 0, 128};
        const auto t0 = std::chrono::steady_clock::now();
        p.process(io, nullptr, 0, native::Transport{});
        const auto t1 = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::micro>(t1 - t0).count();
    };
    for (std::size_t k = 0; (k + 1) * 128 <= n; ++k) {
        const std::size_t pos = k * 128;
        double tq, ta;
        if (k % 2 == 0) {
            tq = timed(*quiet, q_in, pos);
            ta = timed(*active, a_in, pos);
        } else {
            ta = timed(*active, a_in, pos);
            tq = timed(*quiet, q_in, pos);
        }
        // The last 5 s: at least 5 s into the silence.
        if (k >= 15 * blocks_per_s) {
            q_us.push_back(tq);
            a_us.push_back(ta);
        }
    }
    const double ratio = percentile(q_us, 0.5) / percentile(a_us, 0.5);
    s.check_le("denormals: per-block CPU, silence after signal / active (median, interleaved)", ratio,
               s.bar("denormal_ratio"), "ratio",
               dsp::flush_to_zero_supported() ? "FTZ/DAZ on" : "no FTZ on this architecture");
}

Chans zipper_tone(double fs, double seconds, double dbfs) {
    // A 100 Hz tone, faded in over 10 ms so its own start is not a click
    // (a delay would echo one into the measured window).
    Chans x = sine(fs, 100.0, dbfs, seconds);
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
    // A processor whose sound is distortion (the tube) turns a loud 100 Hz
    // tone at full drive into harmonics far above 5 kHz, which is the
    // effect and not a step. Its quality.json sets a quieter tone
    // ("zipper_tone_dbfs"), and the bar moves down with it, so a step
    // must still stay as far under the tone as under the -6 dBFS one.
    double tone_db = -6.0;
    if (const json::Value* t = s.quality("zipper_tone_dbfs")) t->to_double(tone_db);
    const double bar = s.bar("zipper_dbfs") + (tone_db + 6.0);
    const double fs = 48000.0;
    // ADR-0011 §3.2: every automatable parameter. A list that leaves one
    // out is a bar that silently does not apply, so it fails here. Bypass
    // is the base class's crossfade, tested in plugins/native/sdk, and is
    // not a processor parameter.
    {
        auto p = prepared(s, fs, c);
        if (!p) return;
        const native::Descriptor& d = p->descriptor();
        for (std::uint32_t i = 1; i < d.param_count; ++i) {
            if (!d.params[i].automatable) continue;
            bool listed = false;
            for (const json::Value& item : list->items()) listed |= item.text() == d.params[i].id;
            if (!listed) s.fail("quality.json 'zipper' leaves out automatable '" + std::string(d.params[i].id) + "'");
        }
    }
    // A 100 Hz tone at -6 dBFS. Smoothly moving any parameter keeps its
    // energy near 100 Hz; a step or a click puts energy above 5 kHz.
    const Chans in = zipper_tone(fs, 3.25, tone_db);
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

Chans cpu_programme(double fs, double seconds, double gain_db) {
    // Programme-like load: noise with a sweep under it.
    Chans in = noise(fs, -12.0, seconds, 31u);
    const Chans sw = log_sweep(fs, 20.0, 20000.0, -12.0, seconds, 2);
    const float g = static_cast<float>(0.5 * std::pow(10.0, gain_db / 20.0));
    for (std::size_t ch = 0; ch < 2; ++ch) {
        for (std::size_t i = 0; i < in[ch].size(); ++i) in[ch][i] = g * (in[ch][i] + sw[ch][i]);
    }
    return in;
}

void check_cpu(Suite& s, const Config& c, const std::string& label, bool assert_bar) {
    if (!s.args().cpu) return;
    check_cpu_on(s, c, cpu_programme(48000.0, s.args().cpu_seconds, 0.0), label, assert_bar);
}

void check_cpu_on(Suite& s, const Config& c, const Chans& in, const std::string& label, bool assert_bar) {
    if (!s.args().cpu) return;
    const double fs = 48000.0;
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
    if (assert_bar) {
        s.check_le(what + "p99.9", percentile(best, 0.999), s.bar("cpu.p999_us"), "us", "per-block minimum of 5 runs");
        s.check_le(what + "max", percentile(best, 1.0), s.bar("cpu.max_us"), "us", "per-block minimum of 5 runs");
    } else {
        const std::string over = "per-block minimum of 5 runs; reported, not held to the class bar (" +
                                 std::to_string(static_cast<int>(s.bar("cpu.p999_us"))) + " us)";
        s.info(what + "p99.9", percentile(best, 0.999), "us", over);
        s.info(what + "max", percentile(best, 1.0), "us", over);
    }
    s.info(what + "p99.9, single run", percentile(raw, 0.999), "us", "includes the host's scheduling");
    s.info(what + "max, single run", percentile(raw, 1.0), "us", "includes the host's scheduling");
}

void check_common(Suite& s, const Config& golden,
                  const std::vector<std::pair<std::string, Config>>& latency_configs,
                  const Config* zipper) {
    check_latency(s, latency_configs);
    check_nonfinite(s, golden);
    check_reset(s, golden);
    check_denormals(s, golden);
    check_zipper(s, zipper != nullptr ? *zipper : golden, "");
    check_golden(s, golden);
    check_cpu(s, golden, "");
}

}  // namespace sonora::measure
