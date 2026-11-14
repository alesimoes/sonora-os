// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/analyze/mastering_rules.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace sonora::analyze {

using namespace native::mastering;

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

class Builder {
public:
    Builder(const MixAnalysis& a, const MasteringConfig& c, MasteringSuggestion& out) : a_(a), c_(c), t_(*c.table), out_(out) {
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
        for (const int b : t_.anchor_bands) {
            if (!a_.band_valid[b]) continue;
            anchor_mix += a_.band_db[b];
            anchor_target += t_.target_db[b];
            ++n;
        }
        if (n < static_cast<int>(t_.anchor_bands.size())) {
            out_.observations.push_back("Spectral balance not judged: the file's sample rate leaves the 500 Hz - 2 kHz anchor bands incomplete.");
            return;
        }
        anchor_mix /= n;
        anchor_target /= n;
        std::string line = "Balance against the target curve (dB, + is more than the curve):";
        for (int b = 0; b < kBands; ++b) {
            ok[b] = a_.band_valid[b];
            if (!ok[b]) continue;
            dev[b] = (a_.band_db[b] - anchor_mix) - (t_.target_db[b] - anchor_target);
            line += fmt(" %s %+.1f;", t_.band_names[b].c_str(), dev[b]);
        }
        out_.observations.push_back(line);

        for (const BalanceGroup& g : t_.groups) {
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
            f.title = fmt("%s %+.1f dB %s", g.title.c_str(), corr, g.at.c_str());
            f.reason = fmt("%s %s %.1f dB %s the target curve (tolerance %.1f dB, balance judged against 500 Hz - 2 kHz); "
                           "%.0f%% of the excess beyond the tolerance is %s, at most %.0f dB.",
                           g.what.c_str(), cnt > 1 ? "sit" : "sits", std::fabs(d), d > 0 ? "above" : "below",
                           c_.band_tolerance_db, c_.eq_fraction * 100.0, d > 0 ? "taken out" : "put back", c_.eq_max_db);
            if (g.freq != 0) set(f, g.freq, g.freq_hz);
            set(f, g.gain, corr);
            out_.findings.push_back(std::move(f));
        }
        if (ok[0] && std::fabs(dev[0]) > c_.band_tolerance_db + t_.sub_bass_margin_db) {
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
            const double ratio = plr > t_.ratio_step_plr_db ? t_.ratio_high : t_.ratio_normal;
            const double thr = std::fmax(-40.0, std::fmin(0.0, round_to(a_.integrated_lufs + t_.threshold_above_loudness_db, 0.5)));
            Finding f;
            f.rule = "dynamics.glue";
            f.title = fmt("Compressor %.1f:1 from %.1f dB", ratio, thr);
            f.reason = fmt("Peak-to-loudness ratio is %.1f dB, over %.0f dB: peaks stand well above the body of the mix. A gentle %.1f:1 "
                           "compressor from %.1f dB (%.0f dB above the programme loudness of %.1f LUFS), 30 ms attack, 200 ms release, "
                           "brings them nearer.",
                           plr, c_.dynamic_plr_db, ratio, thr, t_.threshold_above_loudness_db, a_.integrated_lufs);
            set(f, kCompThreshold, thr);
            set(f, kCompRatio, ratio);
            out_.findings.push_back(std::move(f));
            if (plr > t_.saturation_plr_db) {
                Finding s;
                s.rule = "dynamics.saturation";
                s.title = fmt("Saturation %.0f%% at %.0f dB drive", t_.saturation_mix_pct, t_.saturation_drive_db);
                s.reason = fmt("Peak-to-loudness ratio is %.1f dB, over %.0f dB: %.0f%% of a soft-clipped copy at %.0f dB drive rounds the "
                               "highest peaks and adds density before the limiter.", plr, t_.saturation_plr_db, t_.saturation_mix_pct, t_.saturation_drive_db);
                set(s, kSatDrive, t_.saturation_drive_db);
                set(s, kSatMix, t_.saturation_mix_pct);
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
        if (a_.correlation < t_.phase_correlation) {
            Finding f;
            f.rule = "stereo.phase";
            f.title = fmt("Width %.0f%%", t_.phase_width_pct);
            f.reason = fmt("L/R correlation is %.2f: more of the mix is out of phase than in phase, which cancels when summed to mono. "
                           "Narrowing to %.0f%% brings the sides in.", a_.correlation, t_.phase_width_pct);
            set(f, kWidth, t_.phase_width_pct);
            out_.findings.push_back(std::move(f));
        } else if (a_.side_mid_db > t_.wide_side_mid_db) {
            Finding f;
            f.rule = "stereo.wide";
            f.title = fmt("Width %.0f%%", t_.wide_width_pct);
            f.reason = fmt("Side level is %.1f dB against the mid (within %.0f dB): a very wide mix. Narrowing to %.0f%% keeps the centre "
                           "from thinning in mono.", a_.side_mid_db, -t_.wide_side_mid_db, t_.wide_width_pct);
            set(f, kWidth, t_.wide_width_pct);
            out_.findings.push_back(std::move(f));
        } else if (a_.side_mid_db < t_.near_mono_side_mid_db) {
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
        const double limit = dense ? std::fmin(c_.max_limiting_db, t_.dense_limit_db) : c_.max_limiting_db;
        double gain = 0.0;
        std::string cap_note;
        if (std::fabs(needed) < t_.no_change_lu) {
            out_.observations.push_back(fmt("Integrated loudness %.1f LUFS is within %.1f LU of the %.1f LUFS target: no gain change.",
                                            a_.integrated_lufs, t_.no_change_lu, c_.target_lufs));
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
        } else if (needed > t_.no_change_lu) {
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
    const MasteringRules& t_;
    MasteringSuggestion& out_;
};

}  // namespace

bool suggest_mastering(const MixAnalysis& a, const MasteringConfig& c, MasteringSuggestion& out, std::string& error) {
    out = MasteringSuggestion{};
    if (c.table == nullptr) {
        error = "the mastering rule table is not loaded";
        return false;
    }
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
