// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADR-0077's dataset side: vocadito's f0 CSVs, WAV loading through
// common/audiofile, the per-set evaluation and the JSON report. Offline.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "sonora/wav_reader.h"
#include "tuner_eval_run.h"

namespace sonora::native::tuner_eval {

namespace fs = std::filesystem;

namespace {
constexpr int kProcessorRange = 1;  // medium: covers the voices of both sets

bool read_text(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

std::vector<fs::path> sorted_files(const fs::path& root, const std::string& ext) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && it->path().extension() == ext) out.push_back(it->path());
    std::sort(out.begin(), out.end());
    return out;
}

// vocadito_N.wav pairs with vocadito_N_f0.csv somewhere under the root.
fs::path find_annotation(const std::vector<fs::path>& csvs, const std::string& stem) {
    for (const fs::path& c : csvs)
        if (c.stem().string() == stem + "_f0") return c;
    return {};
}

std::string json_range(const char* name, const EvalReport& r) {
    std::string s = "    \"";
    s += name;
    s += "\": {\n";
    detail::append_number(s, "rpa", r.rpa);
    detail::append_number(s, "rca", r.rca);
    detail::append_number(s, "voicing_recall", r.voicing_recall);
    detail::append_number(s, "voicing_false_alarm", r.voicing_false_alarm);
    detail::append_number(s, "frames", static_cast<double>(r.frames), true);
    s += "    }";
    return s;
}
}  // namespace

std::vector<PitchFrame> parse_f0_csv(const std::string& text) {
    std::vector<PitchFrame> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t comma = line.find_first_of(",;\t");
        if (comma == std::string::npos) continue;
        char* e1 = nullptr;
        char* e2 = nullptr;
        const double t = std::strtod(line.c_str(), &e1);
        if (e1 == line.c_str()) continue;  // header or junk
        const double hz = std::strtod(line.c_str() + comma + 1, &e2);
        if (e2 == line.c_str() + comma + 1 || !std::isfinite(t) || !std::isfinite(hz)) continue;
        out.push_back({t, hz > 0.0 ? hz : 0.0, hz > 0.0});
    }
    return out;
}

bool load_wav_mono(const std::string& path, Clip& out, std::string& error) {
    WavReader wav;
    if (!wav.open(path, error)) return false;
    const int64_t frames = wav.frames();
    const std::size_t ch = wav.channels();
    std::vector<float> raw(static_cast<std::size_t>(frames) * ch);
    if (frames > 0 && wav.read(0, frames, raw.data()) != frames) {
        error = "short read";
        return false;
    }
    out.fs = wav.sample_rate();
    out.samples.assign(static_cast<std::size_t>(frames), 0.0f);
    for (std::size_t i = 0; i < out.samples.size(); ++i) {
        float s = 0.0f;
        for (std::size_t c = 0; c < ch; ++c) s += raw[i * ch + c];
        out.samples[i] = s / static_cast<float>(ch);
    }
    return true;
}

SetReport evaluate(const TrackSet& set, const Options& opt) {
    SetReport rep;
    for (int r = 0; r < kRangeCount; ++r) {
        std::vector<PitchFrame> ref_all, est_all;
        for (std::size_t i = 0; i < set.references.size() && i < set.clips.size(); ++i) {
            if (set.references[i].empty()) continue;
            const auto est = resample_to(detect_pitch(set.clips[i], r), set.references[i]);
            ref_all.insert(ref_all.end(), set.references[i].begin(), set.references[i].end());
            est_all.insert(est_all.end(), est.begin(), est.end());
        }
        if (!ref_all.empty()) rep.detector[r] = score_detector(ref_all, est_all, opt.tolerance_cents);
    }
    // The processor figure: mean over clips of each clip's in-scale fraction.
    double sum = 0.0;
    std::size_t n = 0;
    for (const Clip& c : set.clips) {
        const auto out = run_tuner(c, opt.key, opt.scale, kProcessorRange);
        const double f = in_scale_fraction(out, c.fs, opt.key, opt.scale, opt.scale_tol_cents);
        if (std::isfinite(f)) {
            sum += f;
            ++n;
        }
        rep.processor.frames += out.size();
    }
    if (n > 0) rep.processor.in_scale_fraction = sum / static_cast<double>(n);
    return rep;
}

RunResult run_dataset(const Options& opt) {
    RunResult res;
    std::error_code ec;
    if (opt.dataset != "vocadito" && opt.dataset != "vocalset") {
        res.exit_code = 1;
        res.message = "unknown dataset '" + opt.dataset + "' (vocadito or vocalset)";
        return res;
    }
    if (opt.scale < 0 || opt.scale >= 6 || opt.key < 0 || opt.key > 11) {
        res.exit_code = 1;
        res.message = "key must be 0-11 and scale 0-5";
        return res;
    }
    if (opt.data_dir.empty() || !fs::is_directory(opt.data_dir, ec)) {
        res.exit_code = 2;
        res.message = "data directory not found: " + opt.data_dir;
        return res;
    }
    const bool labelled = opt.dataset == "vocadito";
    const std::vector<fs::path> csvs = labelled ? sorted_files(opt.data_dir, ".csv") : std::vector<fs::path>{};
    TrackSet set;
    for (const fs::path& wav : sorted_files(opt.data_dir, ".wav")) {
        std::vector<PitchFrame> ref;
        if (labelled) {
            std::string text;
            const fs::path csv = find_annotation(csvs, wav.stem().string());
            if (csv.empty() || !read_text(csv, text)) continue;  // unlabelled: not scored
            ref = parse_f0_csv(text);
            if (ref.empty()) continue;
        }
        Clip clip;
        std::string err;
        if (!load_wav_mono(wav.string(), clip, err)) {
            res.exit_code = 1;
            res.message = wav.string() + ": " + err;
            return res;
        }
        set.clips.push_back(std::move(clip));
        set.references.push_back(std::move(ref));
    }
    if (set.clips.empty()) {
        res.exit_code = 2;
        res.message = "no usable audio under " + opt.data_dir;
        return res;
    }
    const SetReport rep = evaluate(set, opt);
    std::string j = "{\n";
    detail::append_string(j, "dataset", opt.dataset);
    detail::append_string(j, "licence", "CC BY 4.0 (see the dataset's own card)");
    detail::append_string(j, "version", "as downloaded; not recorded by the tool");
    detail::append_number(j, "clips", static_cast<double>(set.clips.size()));
    detail::append_number(j, "key", opt.key);
    detail::append_number(j, "scale", opt.scale);
    if (labelled) {
        j += "\"detector\": {\n";
        for (int r = 0; r < kRangeCount; ++r)
            j += json_range(kRangeNames[r], rep.detector[r]) + (r + 1 < kRangeCount ? ",\n" : "\n");
        j += "},\n";
    }
    j += "\"processor\": {\n";
    detail::append_number(j, "in_scale_fraction", rep.processor.in_scale_fraction, true);
    j += "}\n}\n";
    res.json = std::move(j);
    return res;
}

}  // namespace sonora::native::tuner_eval
