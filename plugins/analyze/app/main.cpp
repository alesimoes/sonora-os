// SPDX-License-Identifier: GPL-3.0-or-later
//
// sonora-analyze (ADR-0011 §3.4, ADR-0046): the offline analysis process.
// A short-lived, non-real-time program that engine/control starts on a
// bounce of the mix and reads the result of. Subcommands (ADR-0066 §5):
//
//   sonora-analyze mastering <bounce.wav> [--target-lufs N] [--ceiling N]
//                  [--out report.json]
//   sonora-analyze key ...       (app/cmd_key.cpp, or a stub: exit 1)
//   sonora-analyze timbre ...    (app/cmd_timbre.cpp, or a stub: exit 1)
//   sonora-analyze tone ...      (app/cmd_tone.cpp, or a stub: exit 1; ADR-0072)
//   sonora-analyze vocal-balance ... (app/cmd_vocal.cpp, or a stub; ADR-0104)
//   sonora-analyze beat <file.wav> [--out report.json]
//                  (app/cmd_beat.cpp, or a stub: exit 1; ADR-0114)
//
// The mastering pass measures the bounce (mix_analysis.h), applies the mastering rules
// (mastering_rules.h) and writes a JSON report: the measurements, the
// console's settings, and each decision with its reason. The report is
// settings, never audio; the session plan that applies them is built by
// whoever holds the session (MasteringPlanner).
//
// SIGINT and SIGTERM cancel the analysis between blocks (exit 130). Exit
// 0: report written. 1: the analysis failed (the message is on stderr).
// 2: usage.
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

#include "commands.h"
#include "sonora/analyze/mastering_rules.h"
#include "sonora/json/json.h"
#include "sonora/wav_reader.h"

namespace {

std::atomic<bool> g_cancel{false};
extern "C" void on_signal(int) { g_cancel.store(true); }

class WavSource final : public sonora::analyze::AudioSource {
public:
    bool open(const std::string& path, std::string& error) { return reader_.open(path, error); }
    std::uint32_t channels() const override { return reader_.channels(); }
    double sample_rate() const override { return reader_.sample_rate(); }
    std::int64_t frames() const override { return reader_.frames(); }
    std::int64_t read(std::int64_t start, std::int64_t count, float* out) override {
        return reader_.read(start, count, out);
    }

private:
    sonora::WavReader reader_;
};

using sonora::json::Value;

Value num(double v) { return Value::number_float(static_cast<float>(v)); }

Value settings_json(const std::vector<float>& values) {
    const auto& d = sonora::native::mastering_descriptor();
    Value o = Value::object();
    for (std::uint32_t i = 1; i < d.param_count; ++i) o.set(d.params[i].id, Value::number_float(values[i]));
    return o;
}

int usage() {
    std::fputs("usage: sonora-analyze mastering <bounce.wav> [--target-lufs N] [--ceiling N] [--out report.json]\n"
               "       sonora-analyze key ...\n"
               "       sonora-analyze timbre ...\n"
               "       sonora-analyze tone ...\n"
               "       sonora-analyze beat <file.wav> [--out report.json]\n",
               stderr);
    return 2;
}

// Where the rule table is read from (ADR-0172): SONORA_ANALYZE_RULES if set
// else next to the binary (the build tree: data/) or in the installed layout
// (../share/sonora/analyze/). No built-in copy: a file that is not found is an
// error naming every path tried.
bool load_rules(sonora::analyze::MasteringRules& rules, std::string& error) {
    namespace fs = std::filesystem;
    std::vector<fs::path> tried;
    if (const char* env = std::getenv("SONORA_ANALYZE_RULES"); env != nullptr && *env != '\0') {
        return sonora::analyze::load_mastering_rules(env, rules, error);  // an explicit path is not searched for
    } else {
        std::error_code ec;
        const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
        if (!ec) {
            tried.push_back(exe.parent_path() / "data" / "mastering_rules.json");
            tried.push_back(exe.parent_path().parent_path() / "share" / "sonora" / "analyze" / "mastering_rules.json");
        }
    }
    for (const fs::path& p : tried) {
        std::error_code ec;
        if (fs::exists(p, ec)) return sonora::analyze::load_mastering_rules(p.string(), rules, error);
    }
    error = "mastering rules: mastering_rules.json not found (looked in:";
    for (const fs::path& p : tried) error += " " + p.string();
    error += "); set SONORA_ANALYZE_RULES or reinstall";
    return false;
}

}  // namespace

namespace sonora::analyze::app {

int run_mastering_command(const std::vector<std::string>& args, const std::atomic<bool>& cancel) {
    if (args.empty()) return usage();
    const std::string& input = args[0];
    std::string out_path;
    MasteringConfig cfg;
    sonora::analyze::MasteringRules rules;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (i + 1 >= args.size()) return usage();
        if (a == "--target-lufs") {
            cfg.target_lufs = std::strtod(args[++i].c_str(), nullptr);
        } else if (a == "--ceiling") {
            cfg.ceiling_dbtp = std::strtod(args[++i].c_str(), nullptr);
        } else if (a == "--out") {
            out_path = args[++i];
        } else {
            return usage();
        }
    }

    std::string error;
    if (!load_rules(rules, error)) {
        std::fprintf(stderr, "sonora-analyze: %s\n", error.c_str());
        return 1;
    }
    cfg.table = &rules;
    WavSource src;
    if (!src.open(input, error)) {
        std::fprintf(stderr, "sonora-analyze: %s: %s\n", input.c_str(), error.c_str());
        return 1;
    }
    RunOptions run;
    run.cancel = &cancel;
    MixAnalysis a;
    if (!analyse_mix(src, run, a, error)) {
        std::fprintf(stderr, "sonora-analyze: %s\n", error.c_str());
        return error == "cancelled" ? 130 : 1;
    }
    MasteringSuggestion s;
    if (!suggest_mastering(a, cfg, s, error)) {
        std::fprintf(stderr, "sonora-analyze: %s\n", error.c_str());
        return 1;
    }

    Value report = Value::object();
    Value m = Value::object();
    m.set("integrated_lufs", num(a.integrated_lufs));
    m.set("max_short_term_lufs", num(a.max_short_term_lufs));
    m.set("loudness_range_lu", num(a.loudness_range_lu));
    m.set("true_peak_dbtp", num(a.true_peak_dbtp));
    m.set("crest_db", num(a.crest_db));
    m.set("plr_db", num(a.plr_db));
    m.set("correlation", num(a.correlation));
    m.set("side_mid_db", num(a.side_mid_db));
    Value bands = Value::array();
    for (int b = 0; b < kBands; ++b) bands.push(a.band_valid[b] ? num(a.band_db[b]) : Value::null());
    m.set("band_db", std::move(bands));
    report.set("measurements", std::move(m));
    report.set("settings", settings_json(s.values));
    Value findings = Value::array();
    for (const Finding& f : s.findings) {
        Value o = Value::object();
        o.set("rule", Value::string(f.rule));
        o.set("title", Value::string(f.title));
        o.set("reason", Value::string(f.reason));
        findings.push(std::move(o));
    }
    report.set("findings", std::move(findings));
    Value obs = Value::array();
    for (const std::string& o : s.observations) obs.push(Value::string(o));
    report.set("observations", std::move(obs));
    report.set("fader_trim_db", num(s.fader_trim_db));
    report.set("aimed_lufs", num(s.aimed_lufs));

    std::string text;
    if (!sonora::json::write(report, text, error)) {
        std::fprintf(stderr, "sonora-analyze: %s\n", error.c_str());
        return 1;
    }
    if (out_path.empty()) {
        std::cout << text;
    } else {
        std::ofstream f(out_path, std::ios::binary);
        f << text;
        if (!f) {
            std::fprintf(stderr, "sonora-analyze: cannot write %s\n", out_path.c_str());
            return 1;
        }
    }
    return 0;
}

}  // namespace sonora::analyze::app

namespace {

struct Command {
    const char* name;
    sonora::analyze::app::CommandFn run;
};

// The one list of subcommands; a pass brings its own app/cmd_<pass>.cpp.
const Command kCommands[] = {
    {"mastering", sonora::analyze::app::run_mastering_command},
    {"key", sonora::analyze::app::run_key_command},
    {"timbre", sonora::analyze::app::run_timbre_command},
    {"tone", sonora::analyze::app::run_tone_command},
    {"vocal-balance", sonora::analyze::app::run_vocal_balance_command},
    {"beat", sonora::analyze::app::run_beat_command},
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    const std::string name = argv[1];
    for (const Command& c : kCommands) {
        if (name != c.name) continue;
        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);
        return c.run(std::vector<std::string>(argv + 2, argv + argc), g_cancel);
    }
    return usage();
}
