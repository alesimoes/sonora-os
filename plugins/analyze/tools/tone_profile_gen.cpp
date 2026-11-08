// SPDX-License-Identifier: GPL-3.0-or-later
//
// ampcab-tone-profile [--manifest M] [--cards DIR] [--captures DIR] [--out DIR]
//
// Renders each installed amp capture through the WaveNet kernel with the
// fixed in-code stimulus (kToneStimulusId) and writes its tone profile,
// <out>/<id>.json, stamped with the SHA-256 of its card (ADR-0072 §3, §7).
// A capture whose file is not in the captures directory is skipped, and
// with none at all nothing is written and the exit is 0: that is the
// shipped state until the owner names hardware. The same input gives the
// same bytes (fixed-seed stimulus, no clock, no threads, no random).
//
// The cabinet IR is not rendered: no cabinet convolution exists yet, so a
// profile stands for the capture alone (ir_id empty) until it does.
// Cards and the manifest are read, never written (ADR-0072 §7).
// Offline: allocates and reads files.
//
// Exit: 0 done (also nothing to do), 1 failed, 2 usage.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "sonora/ampcab/capture_card.h"
#include "sonora/ampcab/nam_file.h"
#include "sonora/analyze/mix_analysis.h"
#include "sonora/analyze/tone_profile.h"
#include "sonora/json/json.h"
#include "sonora/sha256.h"
#include "tone_measure.h"

#ifndef SONORA_TONE_DEFAULT_CAPTURES
#define SONORA_TONE_DEFAULT_CAPTURES "plugins/native/ampcab/captures"
#endif
#ifndef SONORA_TONE_DEFAULT_CARDS
#define SONORA_TONE_DEFAULT_CARDS "brain/models"
#endif

namespace {

namespace fs = std::filesystem;
using namespace sonora::analyze;

constexpr int kBlock = 1024;  // the kernel's block size cap

struct Options {
    std::string captures = SONORA_TONE_DEFAULT_CAPTURES;
    std::string manifest;
    std::string cards = SONORA_TONE_DEFAULT_CARDS;
    std::string out;
};

int usage() {
    std::fputs("usage: ampcab-tone-profile [--manifest M] [--cards DIR] [--captures DIR] [--out DIR]\n", stderr);
    return 2;
}

bool parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        if (i + 1 >= argc) return false;
        const std::string a = argv[i];
        const std::string v = argv[++i];
        if (a == "--captures") o.captures = v;
        else if (a == "--manifest") o.manifest = v;
        else if (a == "--cards") o.cards = v;
        else if (a == "--out") o.out = v;
        else return false;
    }
    if (o.manifest.empty()) o.manifest = (fs::path(o.captures) / "manifest.json").string();
    if (o.out.empty()) o.out = (fs::path(o.captures) / "profiles").string();
    return true;
}

bool read_file(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool manifest_ids(const Options& o, std::vector<std::string>& ids, std::string& error) {
    std::string text;
    if (!read_file(o.manifest, text)) {
        error = o.manifest + ": cannot read the capture manifest";
        return false;
    }
    sonora::json::Value root;
    sonora::json::Error jerr;
    if (!sonora::json::parse(text, root, jerr)) {
        error = o.manifest + ": " + jerr.message;
        return false;
    }
    const auto* entries = root.is_object() ? root.find("entries") : nullptr;
    if (!entries || !entries->is_array()) {
        error = o.manifest + ": no 'entries' array";
        return false;
    }
    for (const auto& e : entries->items()) {
        const auto* id = e.is_object() ? e.find("id") : nullptr;
        if (!id || !id->is_string()) {
            error = o.manifest + ": an entry has no string 'id'";
            return false;
        }
        ids.push_back(id->text());
    }
    return true;
}

// The stimulus through the capture, block by block as the processor would
// run it; the network is prewarmed by prepare()/reset().
bool render(const sonora::ampcab::NamModel& model, std::vector<float>& signal, std::string& error) {
    sonora::dsp::neural::WaveNet net;
    if (!net.load(model.wavenet, model.weights, &error)) return false;
    net.prepare(kBlock);
    net.reset();
    for (std::size_t at = 0; at < signal.size(); at += kBlock) {
        const int n = static_cast<int>(std::min<std::size_t>(kBlock, signal.size() - at));
        net.process(signal.data() + at, signal.data() + at, n);
    }
    for (float v : signal)
        if (!std::isfinite(v)) {
            error = "the capture's output is not finite";
            return false;
        }
    return true;
}

bool make_profile(const std::string& id, const std::string& card_md, const std::string& nam_text,
                  CaptureProfile& p, std::string& error) {
    sonora::ampcab::NamModel model;
    if (!sonora::ampcab::parse_nam(nam_text, &model, &error)) return false;
    if (std::fabs(model.sample_rate - kToneStimulusRate) > 0.5) {
        error = "the capture runs at " + std::to_string(model.sample_rate) + " Hz; the stimulus is " +
                std::to_string(kToneStimulusRate) + " Hz";
        return false;
    }
    std::vector<float> signal = tone_stimulus();
    if (!render(model, signal, error)) return false;
    std::vector<std::vector<float>> planar{std::move(signal)};
    MemorySource src(planar, kToneStimulusRate);
    p = CaptureProfile{};
    p.id = id;
    p.card_sha256 = sonora::sha256_hex(card_md);
    p.stimulus = kToneStimulusId;
    return measure_tone_profile(src, RunOptions{}, p.profile, error);
}

// 0 written, 1 failed, 2 skipped (no capture file).
int process_capture(const Options& o, const std::string& id, std::string& error) {
    std::string md;
    if (!tone_detail::is_card_id(id) || !read_file(fs::path(o.cards) / (id + ".md"), md)) {
        error = "capture " + id + ": its card is missing";
        return 1;
    }
    sonora::ampcab::CaptureCard card;
    const auto refusals = sonora::ampcab::parse_card(md, card);
    if (!refusals.empty()) {
        error = "capture " + id + ": " + refusals.front();
        return 1;
    }
    if (card.kind != sonora::ampcab::CardKind::capture) return 2;
    std::string nam;
    if (!read_file(fs::path(o.captures) / card.file.name, nam)) return 2;
    const auto bad = sonora::ampcab::check_capture_file(card, nam);
    if (!bad.empty()) {
        error = "capture " + id + ": " + bad.front();
        return 1;
    }
    CaptureProfile p;
    std::string text;
    if (!make_profile(id, md, nam, p, error) || !profile_to_json(p, text, error)) {
        error = "capture " + id + ": " + error;
        return 1;
    }
    std::error_code ec;
    fs::create_directories(o.out, ec);
    std::ofstream f(fs::path(o.out) / (id + ".json"), std::ios::binary);
    f << text;
    if (!f) {
        error = "capture " + id + ": cannot write the profile";
        return 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parse_args(argc, argv, o)) return usage();
    std::string error;
    std::vector<std::string> ids;
    if (!manifest_ids(o, ids, error)) {
        std::fprintf(stderr, "ampcab-tone-profile: %s\n", error.c_str());
        return 1;
    }
    int written = 0;
    for (const std::string& id : ids) {
        const int r = process_capture(o, id, error);
        if (r == 1) {
            std::fprintf(stderr, "ampcab-tone-profile: %s\n", error.c_str());
            return 1;
        }
        if (r == 0) {
            std::printf("wrote %s\n", id.c_str());
            ++written;
        }
    }
    if (written == 0) std::puts("no captures installed");
    return 0;
}
