// SPDX-License-Identifier: GPL-3.0-or-later
//
// sonora-analyze vocal-balance <vocal.wav> [--target-level-db N] [--out report.json]
// (ADR-0104): the whole-track, rule-based vocal analysis and the plan it
// proposes, as a JSON report. Audio is never written. The report is written
// only after the analysis finished, so a failure (exit 1) or a cancel (130)
// leaves no file.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>

#include "commands.h"
#include "sonora/json/json.h"
#include "sonora/model/history.h"
#include "sonora/model/operations.h"
#include "sonora/model/plan.h"
#include "sonora/native/vocal_balance_analysis.h"
#include "sonora/wav_reader.h"

namespace sonora::analyze::app {

namespace {

using json::Value;
namespace nat = sonora::native;

struct Options {
    std::string wav;
    std::string out;
    double target_level_db = -20.0;
};

int usage() {
    std::fputs("usage: sonora-analyze vocal-balance <vocal.wav> [--target-level-db N] "
               "[--out report.json]\n",
               stderr);
    return 2;
}

int fail(const std::string& what) {
    std::fprintf(stderr, "sonora-analyze: %s\n", what.c_str());
    return 1;
}

bool parse_args(const std::vector<std::string>& args, Options& o) {
    if (args.empty() || args[0].rfind("--", 0) == 0) return false;
    o.wav = args[0];
    for (std::size_t i = 1; i < args.size(); i += 2) {
        if (i + 1 >= args.size()) return false;
        const std::string& v = args[i + 1];
        if (args[i] == "--out") {
            o.out = v;
        } else if (args[i] == "--target-level-db") {
            char* end = nullptr;
            o.target_level_db = std::strtod(v.c_str(), &end);
            if (end == v.c_str() || *end != '\0') return false;
        } else {
            return false;
        }
    }
    return true;
}

// Stereo (or more) is summed to mono by the mean of its channels.
bool load_mono(const std::string& path, std::vector<float>& mono, double& rate, std::string& error) {
    WavReader r;
    if (!r.open(path, error)) return false;
    const std::size_t ch = r.channels();
    const std::int64_t total = r.frames();
    rate = r.sample_rate();
    mono.assign(static_cast<std::size_t>(total), 0.0f);
    constexpr std::int64_t kBlock = 16384;
    std::vector<float> buf(static_cast<std::size_t>(kBlock) * ch);
    for (std::int64_t at = 0; at < total;) {
        const std::int64_t got = r.read(at, std::min(kBlock, total - at), buf.data());
        if (got <= 0) {
            error = "read error";
            return false;
        }
        for (std::int64_t i = 0; i < got; ++i) {
            float s = 0.0f;
            for (std::size_t c = 0; c < ch; ++c) s += buf[static_cast<std::size_t>(i) * ch + c];
            mono[static_cast<std::size_t>(at + i)] = s / static_cast<float>(ch);
        }
        at += got;
    }
    return true;
}

Value num(double v) { return Value::number_float(static_cast<float>(v)); }

Value measurements(const nat::VocalAnalysis& a) {
    Value m = Value::object();
    m.set("sample_rate", num(a.sample_rate));
    m.set("duration_s", num(a.duration_s));
    m.set("active_fraction", num(a.active_fraction));
    m.set("level_db", num(a.level_db));
    m.set("level_spread_db", num(a.level_spread_db));
    Value res = Value::array();
    for (const nat::VocalResonance& r : a.resonances) {
        Value o = Value::object();
        o.set("freq_hz", num(r.freq_hz));
        o.set("q", num(r.q));
        o.set("excess_db", num(r.excess_db));
        o.set("cut_db", num(r.cut_db));
        o.set("zone", Value::string(r.zone));
        res.push(std::move(o));
    }
    m.set("resonances", std::move(res));
    Value zones = Value::array();
    for (const nat::VocalZone& z : a.zones) {
        Value o = Value::object();
        o.set("name", Value::string(z.name));
        o.set("lo_hz", num(z.lo_hz));
        o.set("hi_hz", num(z.hi_hz));
        o.set("excess_db", num(z.excess_db));
        zones.push(std::move(o));
    }
    m.set("zones", std::move(zones));
    Value s = Value::object();
    s.set("found", Value::boolean(a.sibilance.found));
    s.set("freq_hz", num(a.sibilance.freq_hz));
    s.set("level_db", num(a.sibilance.level_db));
    s.set("excess_db", num(a.sibilance.excess_db));
    s.set("fraction", num(a.sibilance.fraction));
    m.set("sibilance", std::move(s));
    return m;
}

Value step_json(const model::PlanStep& st) {
    Value o = Value::object();
    o.set("title", Value::string(st.title));
    o.set("reason", Value::string(st.reason));
    if (const auto* ai = model::get_if<model::AddInsert>(&st.op)) {
        o.set("kind", Value::string("add_insert"));
        o.set("plugin", Value::string(ai->plugin.name));
        Value params = Value::object();
        for (const auto& p : ai->params) params.set(p.key, num(p.value));
        o.set("params", std::move(params));
    } else if (const auto* sm = model::get_if<model::SetMix>(&st.op)) {
        o.set("kind", Value::string("set_mix"));
        o.set("gain_db", num(sm->mix.gain_db));
    }
    return o;
}

// The plan needs a track to be about; the command has no session, so it
// proposes against a one-track snapshot. A plan with no steps is not a
// failure: a clean voice is a valid answer, reported as plan.error.
Value plan_json(const nat::VocalAnalysis& a, double target_db) {
    using namespace model;
    History h(new_session(SessionSettings{}));
    AddTrack t;
    t.track = h.current().next_id;
    t.playlist = t.track + 1;
    t.name = "Vocal";
    t.playlist_name = "Vocal.01";
    t.channels = 1;
    t.mix.input.push_back(0);
    std::string error;
    Value plan = Value::object();
    Value steps = Value::array();
    Value notes = Value::array();
    if (!h.apply(t, error)) {
        plan.set("error", Value::string(error));
    } else {
        nat::VocalBalancePlanSource src(t.track, a, target_db);
        Plan p;
        if (src.propose(h.current(), p, error)) {
            plan.set("label", Value::string(p.label));
            for (const PlanStep& st : p.steps) steps.push(step_json(st));
            for (const PlanNote& n : p.notes) {
                Value o = Value::object();
                o.set("subject", Value::string(n.subject));
                o.set("text", Value::string(n.text));
                notes.push(std::move(o));
            }
        } else {
            plan.set("error", Value::string(error));
        }
    }
    plan.set("steps", std::move(steps));
    plan.set("notes", std::move(notes));
    return plan;
}

int emit(const Options& o, const Value& report) {
    std::string text, error;
    if (!json::write(report, text, error)) return fail(error);
    if (o.out.empty()) {
        std::cout << text;
        return 0;
    }
    std::ofstream f(o.out, std::ios::binary);
    f << text;
    f.flush();
    if (!f) return fail("cannot write " + o.out);
    return 0;
}

}  // namespace

int run_vocal_balance_command(const std::vector<std::string>& args, const std::atomic<bool>& cancel) {
    Options o;
    if (!parse_args(args, o)) return usage();
    std::vector<float> mono;
    double rate = 0.0;
    std::string error;
    if (!load_mono(o.wav, mono, rate, error)) return fail(o.wav + ": " + error);
    if (cancel.load()) return 130;
    nat::VocalAnalysis a;
    if (!nat::analyse_vocal(mono.data(), mono.size(), rate, a, error)) return fail(o.wav + ": " + error);
    if (cancel.load()) return 130;

    Value report = Value::object();
    report.set("pass", Value::string("vocal-balance"));
    report.set("target_level_db", num(o.target_level_db));
    report.set("measurements", measurements(a));
    report.set("plan", plan_json(a, o.target_level_db));
    return emit(o, report);
}

}  // namespace sonora::analyze::app
