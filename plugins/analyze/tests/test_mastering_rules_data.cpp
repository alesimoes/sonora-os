// SPDX-License-Identifier: GPL-3.0-or-later
//
// Golden test for the mastering rule table (ADR-0172 settles where it lives):
// the suggestion report for a fixed set of analyses must stay byte-equal
// whether the thresholds are compiled in or loaded from
// data/mastering_rules.json. The golden file was captured from the compiled-in
// table before it moved. `--print` writes the report to stdout (to regenerate
// it, a deliberate commit that says why the sound changed).
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "sonora/analyze/mastering_rules.h"
#include "mastering_rules_fixture.h"

using namespace sonora::analyze;

namespace {

// Independent of the library's table on purpose: the fixtures sit on this
// curve whatever the table says.
const double kCurve[kBands] = {-3.0, 4.0, 5.0, 3.0, 1.5, 0.0, -3.0, -6.5, -10.5, -16.0};

MixAnalysis base() {
    MixAnalysis a;
    a.sample_rate = 48000.0;
    a.frames = 48000 * 30;
    a.channels = 2;
    a.duration_s = 30.0;
    a.integrated_lufs = -14.0;
    a.max_short_term_lufs = -12.0;
    a.loudness_range_lu = 6.0;
    a.true_peak_dbtp = -2.0;
    a.sample_peak_dbfs = -2.0;
    a.rms_dbfs = -17.0;
    a.crest_db = 15.0;
    a.plr_db = 12.0;
    a.correlation = 0.9;
    a.side_mid_db = -12.0;
    for (int b = 0; b < kBands; ++b) {
        a.band_db[b] = -40.0 + kCurve[b];
        a.band_valid[b] = true;
    }
    a.spectrum_frames = 100;
    return a;
}

void plr(MixAnalysis& a, double v) {
    a.plr_db = v;
    a.true_peak_dbtp = a.integrated_lufs + v;
}

struct Case {
    std::string name;
    MixAnalysis a;
    MasteringConfig c;
};

std::vector<Case> cases() {
    std::vector<Case> v;
    v.push_back({"neutral", base(), {}});
    {
        MixAnalysis a = base();
        a.band_db[8] -= 8.0; a.band_db[9] -= 8.0;
        a.band_db[1] += 9.0; a.band_db[2] += 9.0;
        a.band_db[7] += 20.0;
        a.band_db[3] -= 6.0;
        v.push_back({"balance_all_groups", a, {}});
    }
    {
        MixAnalysis a = base();
        a.band_db[0] += 12.0;
        a.band_db[9] = 0.0; a.band_valid[9] = false;
        v.push_back({"sub_bass_and_missing_band", a, {}});
    }
    {
        MixAnalysis a = base();
        a.band_valid[5] = false;
        v.push_back({"anchor_incomplete", a, {}});
    }
    { MixAnalysis a = base(); plr(a, 6.0); v.push_back({"dense", a, {}}); }
    { MixAnalysis a = base(); plr(a, 15.0); v.push_back({"glue_15", a, {}}); }
    { MixAnalysis a = base(); plr(a, 17.0); v.push_back({"glue_saturation_17", a, {}}); }
    { MixAnalysis a = base(); plr(a, 19.5); a.integrated_lufs = -30.0; plr(a, 19.5); v.push_back({"glue_ratio2_quiet", a, {}}); }
    { MixAnalysis a = base(); a.correlation = -0.2; v.push_back({"phase", a, {}}); }
    { MixAnalysis a = base(); a.side_mid_db = -2.0; v.push_back({"wide", a, {}}); }
    { MixAnalysis a = base(); a.side_mid_db = -30.0; v.push_back({"near_mono", a, {}}); }
    { MixAnalysis a = base(); a.channels = 1; v.push_back({"mono", a, {}}); }
    { MixAnalysis a = base(); a.integrated_lufs = -20.0; plr(a, 12.0); v.push_back({"quiet_gain", a, {}}); }
    { MixAnalysis a = base(); a.integrated_lufs = -30.0; plr(a, 12.0); v.push_back({"gain_capped_at_12", a, {}}); }
    { MixAnalysis a = base(); a.integrated_lufs = -22.0; a.true_peak_dbtp = -0.5; v.push_back({"no_room", a, {}}); }
    { MixAnalysis a = base(); a.integrated_lufs = -6.0; plr(a, 5.0); v.push_back({"loud_dense", a, {}}); }
    { MixAnalysis a = base(); a.integrated_lufs = -4.0; plr(a, 12.0); MasteringConfig c; c.target_lufs = -23.0; c.ceiling_dbtp = -2.0; v.push_back({"fader_trim_r128", a, c}); }
    { MixAnalysis a = base(); a.true_peak_dbtp = 1.5; v.push_back({"over_ceiling", a, {}}); }
    { MixAnalysis a = base(); a.band_db[8] -= 7.0; plr(a, 16.0); MasteringConfig c; c.band_tolerance_db = 1.0; c.eq_fraction = 0.8; c.eq_max_db = 4.0; c.dense_plr_db = 9.0; c.dynamic_plr_db = 13.0; v.push_back({"custom_config", a, c}); }
    return v;
}

std::string render(const std::string& name, const MasteringSuggestion& s) {
    std::ostringstream o;
    char buf[64];
    o << "== " << name << "\n";
    for (const Finding& f : s.findings) {
        o << "finding " << f.rule << " | " << f.title << " | " << f.reason << "\n";
        for (const Setting& st : f.settings) {
            std::snprintf(buf, sizeof buf, "%.9g", static_cast<double>(st.value));
            o << "  set " << st.index << " " << buf << "\n";
        }
    }
    for (const std::string& ob : s.observations) o << "obs " << ob << "\n";
    for (std::size_t i = 0; i < s.values.size(); ++i) {
        std::snprintf(buf, sizeof buf, "%.9g", static_cast<double>(s.values[i]));
        o << "value " << i << " " << buf << "\n";
    }
    std::snprintf(buf, sizeof buf, "%.17g", s.fader_trim_db);
    o << "fader " << buf << " | " << s.fader_reason << "\n";
    std::snprintf(buf, sizeof buf, "%.17g", s.aimed_lufs);
    o << "aimed " << buf << "\n";
    return o.str();
}

}  // namespace

int main(int argc, char** argv) {
    std::string report;
    for (Case& c : cases()) {
        MasteringSuggestion s;
        std::string err;
        if (!run_suggest(c.a, c.c, s, err)) {
            std::fprintf(stderr, "%s: %s\n", c.name.c_str(), err.c_str());
            return 1;
        }
        report += render(c.name, s);
    }
    if (argc > 1 && std::string(argv[1]) == "--print") {
        std::fwrite(report.data(), 1, report.size(), stdout);
        return 0;
    }
    std::ifstream f(GOLDEN_PATH, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    if (!f || ss.str() != report) {
        if (std::getenv("SONORA_GOLDEN_DUMP")) std::fwrite(report.data(), 1, report.size(), stderr);
        std::fprintf(stderr, "suggestion report differs from %s\n", GOLDEN_PATH);
        return 1;
    }
    std::puts("golden: byte-equal");
    return 0;
}
