// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/analyze/mastering_rules.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace sonora::analyze {

using namespace native::mastering;

const double kTargetDb[kBands] = {-3.0, 4.0, 5.0, 3.0, 1.5, 0.0, -3.0, -6.5, -10.5, -16.0};

namespace {

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

double round_to(double v, double step) { return std::round(v / step) * step; }

const char* kBandNames[kBands] = {"31.5 Hz", "63 Hz", "125 Hz", "250 Hz", "500 Hz",
                                  "1 kHz",   "2 kHz", "4 kHz",  "8 kHz",  "16 kHz"};

class Builder {
public:
    Builder(const MixAnalysis& a, const MasteringConfig& c, MasteringSuggestion& out) : a_(a), c_(c), out_(out) {
        const native::Descriptor& d = native::mastering_descriptor();
        out_.values.assign(d.param_count, 0.0f);
        for (std::uint32_t i = 0; i < d.param_count; ++i) out_.values[i] = d.params[i].def;
    }

    void set(Finding& f, std::uint32_t index, double value) {
        const native::ParamInfo& p = native::mastering_descriptor().params[index];
        const float v = native::sanitize(p, static_cast<float>(value));
        out_.values[index] = v;
        f.settings.push_back({index, v});
    }
    void set_quiet(std::uint32_t index, double value) {
        Finding scratch;
        set(scratch, index, value);
    }

    void balance() {
        double dev[kBands] = {};
        bool ok[kBands] = {};
        double anchor_mix = 0.0, anchor_target = 0.0;
        int n = 0;
        for (int b = 4; b <= 6; ++b) {
            if (!a_.band_valid[b]) continue;
            anchor_mix += a_.band_db[b];
            anchor_target += kTargetDb[b];
            ++n;
        }
        if (n < 3) {
            out_.observations.push_back("Spectral balance not judged: the file's sample rate leaves the 500 Hz - 2 kHz anchor bands incomplete.");
            return;
        }
        anchor_mix /= n;
        anchor_target /= n;
        std::string line = "Balance against the target curve (dB, + is more than the curve):";
        for (int b = 0; b < kBands; ++b) {
            ok[b] = a_.band_valid[b];
            if (!ok[b]) continue;
            dev[b] = (a_.band_db[b] - anchor_mix) - (kTargetDb[b] - anchor_target);
            line += fmt(" %s %+.1f;", kBandNames[b], dev[b]);
        }
        out_.observations.push_back(line);

        struct Group {
            const char* rule;
            int bands[2];
            const char* what;     // "63 and 125 Hz"
            const char* title;    // "Low shelf"
            std::uint32_t gain;
            std::uint32_t freq;   // 0: fixed
            double freq_hz;
            const char* at;       // "at 100 Hz"
        };
        const Group groups[] = {
            {"balance.low", {1, 2}, "63 and 125 Hz", "Low shelf", kLowGain, 0, 0.0, "at 100 Hz"},
            {"balance.lowmid", {3, -1}, "250 Hz", "Low-mid bell", kLowMidGain, kLowMidFreq, 250.0, "at 250 Hz"},
            {"balance.highmid", {7, -1}, "4 kHz", "High-mid bell", kHighMidGain, kHighMidFreq, 4000.0, "at 4 kHz"},
            {"balance.high", {8, 9}, "8 and 16 kHz", "High shelf", kHighGain, 0, 0.0, "at 10 kHz"},
        };
        for (const Group& g : groups) {
            double sum = 0.0;
            int cnt = 0;
            for (int b : g.bands) {
                if (b >= 0 && ok[b]) {
                    sum += dev[b];
                    ++cnt;
                }
            }
            if (cnt == 0) continue;
            const double d = sum / cnt;
            if (std::fabs(d) <= c_.band_tolerance_db) continue;
            const double raw = -std::copysign(std::fabs(d) - c_.band_tolerance_db, d) * c_.eq_fraction;
            const double corr = round_to(std::fmax(-c_.eq_max_db, std::fmin(c_.eq_max_db, raw)), 0.5);
            if (corr == 0.0) continue;
            Finding f;
            f.rule = g.rule;
            f.title = fmt("%s %+.1f dB %s", g.title, corr, g.at);
            f.reason = fmt("%s %s %.1f dB %s the target curve (tolerance %.1f dB, balance judged against 500 Hz - 2 kHz); "
                           "%.0f%% of the excess beyond the tolerance is %s, at most %.0f dB.",
                           g.what, cnt > 1 ? "sit" : "sits", std::fabs(d), d > 0 ? "above" : "below",
                           c_.band_tolerance_db, c_.eq_fraction * 100.0, d > 0 ? "taken out" : "put back", c_.eq_max_db);
            if (g.freq != 0) set(f, g.freq, g.freq_hz);
            set(f, g.gain, corr);
            out_.findings.push_back(std::move(f));
        }
        if (ok[0] && std::fabs(dev[0]) > c_.band_tolerance_db + 3.0) {
            out_.observations.push_back(fmt("31.5 Hz is %.1f dB %s the curve: sub-bass is reported, not adjusted (a high-pass or a sub cut belongs to the mix).",
                                            std::fabs(dev[0]), dev[0] > 0 ? "above" : "below"));
        }
    }

    // Returns whether the mix is dense, for the loudness rule.
    bool dynamics() {
        const double plr = a_.plr_db;
        if (plr < c_.dense_plr_db) {
            Finding f;
            f.rule = "dynamics.dense";
            f.title = "Compressor and saturation left off";
            f.reason = fmt("Peak-to-loudness ratio is %.1f dB (true peak %.1f dBTP, integrated %.1f LUFS), under %.0f dB: the mix is already "
                           "dense, and more compression or saturation would take what dynamics are left. The loudness gain is held "
                           "to the headroom the peaks leave.",
                           plr, a_.true_peak_dbtp, a_.integrated_lufs, c_.dense_plr_db);
            out_.findings.push_back(std::move(f));
            return true;
        }
        if (plr > c_.dynamic_plr_db) {
            const double ratio = plr > 18.0 ? 2.0 : 1.5;
            const double thr = std::fmax(-40.0, std::fmin(0.0, round_to(a_.integrated_lufs + 4.0, 0.5)));
            Finding f;
            f.rule = "dynamics.glue";
            f.title = fmt("Compressor %.1f:1 from %.1f dB", ratio, thr);
            f.reason = fmt("Peak-to-loudness ratio is %.1f dB, over %.0f dB: peaks stand well above the body of the mix. A gentle %.1f:1 "
                           "compressor from %.1f dB (4 dB above the programme loudness of %.1f LUFS), 30 ms attack, 200 ms release, "
                           "brings them nearer.",
                           plr, c_.dynamic_plr_db, ratio, thr, a_.integrated_lufs);
            set(f, kCompThreshold, thr);
            set(f, kCompRatio, ratio);
            out_.findings.push_back(std::move(f));
            if (plr > 16.0) {
                Finding s;
                s.rule = "dynamics.saturation";
                s.title = "Saturation 15% at 6 dB drive";
                s.reason = fmt("Peak-to-loudness ratio is %.1f dB, over 16 dB: 15%% of a soft-clipped copy at 6 dB drive rounds the "
                               "highest peaks and adds density before the limiter.", plr);
                set(s, kSatDrive, 6.0);
                set(s, kSatMix, 15.0);
                out_.findings.push_back(std::move(s));
            }
        } else {
            out_.observations.push_back(fmt("Peak-to-loudness ratio %.1f dB is in the normal range (%.0f - %.0f dB): no compression or saturation proposed.",
                                            plr, c_.dense_plr_db, c_.dynamic_plr_db));
        }
        return false;
    }

    void stereo() {
        if (a_.channels < 2) {
            out_.observations.push_back("Mono file: width left at 100%.");
            return;
        }
        if (a_.correlation < 0.0) {
            Finding f;
            f.rule = "stereo.phase";
            f.title = "Width 80%";
            f.reason = fmt("L/R correlation is %.2f: more of the mix is out of phase than in phase, which cancels when summed to mono. "
                           "Narrowing to 80%% brings the sides in.", a_.correlation);
            set(f, kWidth, 80.0);
            out_.findings.push_back(std::move(f));
        } else if (a_.side_mid_db > -3.0) {
            Finding f;
            f.rule = "stereo.wide";
            f.title = "Width 90%";
            f.reason = fmt("Side level is %.1f dB against the mid (within 3 dB): a very wide mix. Narrowing to 90%% keeps the centre "
                           "from thinning in mono.", a_.side_mid_db);
            set(f, kWidth, 90.0);
            out_.findings.push_back(std::move(f));
        } else if (a_.side_mid_db < -24.0) {
            out_.observations.push_back(fmt("Side level is %.1f dB against the mid: a near-mono mix. Width left at 100%%; widening it would change the balance.",
                                            a_.side_mid_db));
        } else {
            out_.observations.push_back(fmt("Stereo image is healthy (correlation %.2f, side %.1f dB against the mid): width left at 100%%.",
                                            a_.correlation, a_.side_mid_db));
        }
    }

    void loudness(bool dense) {
        const double needed = c_.target_lufs - a_.integrated_lufs;
        const double headroom = c_.ceiling_dbtp - a_.true_peak_dbtp;
        const double limit = dense ? std::fmin(c_.max_limiting_db, 2.0) : c_.max_limiting_db;
        double gain = 0.0;
        std::string cap_note;
        if (std::fabs(needed) < 0.5) {
            out_.observations.push_back(fmt("Integrated loudness %.1f LUFS is within 0.5 LU of the %.1f LUFS target: no gain change.",
                                            a_.integrated_lufs, c_.target_lufs));
        } else if (needed > 0.0) {
            const double allowed = std::fmax(headroom, 0.0) + limit;
            gain = std::fmin(std::fmin(needed, kTrimRangeDb), allowed);
            gain = std::floor(gain * 10.0) / 10.0;
            if (gain < needed - 0.05) {
                cap_note = fmt(" Capped: the target needs +%.1f dB, but that would ask the limiter for more than %.1f dB; the mix reaches "
                               "%.1f LUFS, not %.1f.",
                               needed, limit, a_.integrated_lufs + gain, c_.target_lufs);
            }
        } else {
            gain = std::fmax(needed, -kTrimRangeDb);
            gain = std::ceil(gain * 10.0) / 10.0;
        }
        if (gain != 0.0) {
            Finding f;
            f.rule = "loudness.gain";
            f.title = fmt("Gain %+.1f dB ahead of the limiter", gain);
            if (gain > 0.0) {
                const double lim_need = std::fmax(0.0, gain - std::fmax(headroom, 0.0));
                f.reason = fmt("Integrated loudness is %.1f LUFS, the target %.1f LUFS: %+.1f dB. The true peak of %.1f dBTP leaves %.1f dB "
                               "under the %.1f dBTP ceiling, so the limiter takes about %.1f dB (cap %.1f dB).%s",
                               a_.integrated_lufs, c_.target_lufs, needed, a_.true_peak_dbtp, std::fmax(headroom, 0.0),
                               c_.ceiling_dbtp, lim_need, limit, cap_note.c_str());
            } else {
                f.reason = fmt("Integrated loudness is %.1f LUFS, above the %.1f LUFS target by %.1f dB: the gain ahead of the limiter "
                               "turns it down.",
                               a_.integrated_lufs, c_.target_lufs, -needed);
            }
            set(f, kGain, gain);
            out_.findings.push_back(std::move(f));
        } else if (needed > 0.5) {
            out_.observations.push_back(fmt("Target %.1f LUFS is %.1f dB above the mix, but the peaks leave no room: no gain proposed.",
                                            c_.target_lufs, needed));
        }
        if (!cap_note.empty() && gain != 0.0) out_.observations.push_back(cap_note.substr(1));
        const double beyond = needed - gain;
        if (needed < 0.0 && gain <= -kTrimRangeDb + 0.05 && beyond < -0.05) {
            out_.fader_trim_db = std::floor(beyond * 10.0) / 10.0;
            out_.fader_reason = fmt("The mix is %.1f dB above the %.1f LUFS target; the console's gain stops at -%.0f dB, so the strip's "
                                    "fader takes the remaining %.1f dB after the limiter.",
                                    -needed, c_.target_lufs, kTrimRangeDb, -beyond);
        }
        out_.aimed_lufs = a_.integrated_lufs + gain + out_.fader_trim_db;
        if (headroom < 0.0 && gain <= 0.0) {
            out_.observations.push_back(fmt("True peak %.1f dBTP is %.1f dB over the %.1f dBTP ceiling: the limiter takes it.", a_.true_peak_dbtp,
                                            -headroom, c_.ceiling_dbtp));
        }
        set_quiet(kCeiling, c_.ceiling_dbtp);
        set_quiet(kTargetLufs, c_.target_lufs);
        out_.observations.push_back(fmt("Ceiling %.1f dBTP and target %.1f LUFS are set on the console; auto-level stays off, so the gain "
                                        "above is fixed and can be checked.",
                                        c_.ceiling_dbtp, c_.target_lufs));
    }

private:
    const MixAnalysis& a_;
    const MasteringConfig& c_;
    MasteringSuggestion& out_;
};

}  // namespace

bool suggest_mastering(const MixAnalysis& a, const MasteringConfig& c, MasteringSuggestion& out, std::string& error) {
    out = MasteringSuggestion{};
    if (a.spectrum_frames == 0 || !(a.integrated_lufs > -70.0) || a.channels == 0) {
        error = "the analysis is empty or silent: nothing to base settings on";
        return false;
    }
    if (!(c.target_lufs <= -6.0 && c.target_lufs >= -24.0) || !(c.ceiling_dbtp <= 0.0 && c.ceiling_dbtp >= -6.0)) {
        error = "the target must be -24 to -6 LUFS and the ceiling -6 to 0 dBTP";
        return false;
    }
    Builder b(a, c, out);
    b.balance();
    const bool dense = b.dynamics();
    b.stereo();
    b.loudness(dense);
    return true;
}

}  // namespace sonora::analyze
