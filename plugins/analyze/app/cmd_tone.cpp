// SPDX-License-Identifier: GPL-3.0-or-later
//
// sonora-analyze tone --reference FILE [--captures DIR] [--manifest M]
//                     [--cards DIR] [--max-distance DB] [--json]   (ADR-0072)
//
// Suggests the installed captures whose spectrum is closest to a reference
// the user supplies, plus the EQ that closes the gap. The reference is read
// and closed; only its derived profile is reported, labelled by its file
// name as given. Matching is by spectrum only: nothing here holds or
// prints an artist, band or product name. Exit: 0 report written (also the
// two empty answers), 1 failed (unreadable reference, stale or missing
// profile), 2 usage, 130 cancelled (ADR-0046).
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "commands.h"
#include "sonora/ampcab/capture_card.h"
#include "sonora/analyze/tone_match.h"
#include "sonora/json/json.h"
#include "sonora/sha256.h"
#include "sonora/wav_reader.h"
#include "tone_matcher.h"
#include "tone_measure.h"

#ifndef SONORA_TONE_DEFAULT_CAPTURES
#define SONORA_TONE_DEFAULT_CAPTURES "plugins/native/ampcab/captures"
#endif
#ifndef SONORA_TONE_DEFAULT_CARDS
#define SONORA_TONE_DEFAULT_CARDS "brain/models"
#endif

namespace {

using sonora::json::Value;
using namespace sonora::analyze;
namespace fs = std::filesystem;

struct Options {
    std::string reference;
    std::string captures = SONORA_TONE_DEFAULT_CAPTURES;
    std::string manifest;  // default: <captures>/manifest.json
    std::string cards = SONORA_TONE_DEFAULT_CARDS;
    double max_distance_db = ToneMatchParams{}.max_distance_db;
    bool json = false;
};

class WavSource final : public AudioSource {
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

Value num(double v) { return Value::number_float(static_cast<float>(v)); }

int usage() {
    std::fputs(
        "usage: sonora-analyze tone --reference FILE [--captures DIR] [--manifest M] [--cards DIR]"
        " [--max-distance DB] [--json]\n",
        stderr);
    return 2;
}

int fail(const std::string& what) {
    std::fprintf(stderr, "sonora-analyze: %s\n", what.c_str());
    return 1;
}

bool parse_args(const std::vector<std::string>& args, Options& o) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--json") {
            o.json = true;
            continue;
        }
        if (i + 1 >= args.size()) return false;
        const std::string& v = args[++i];
        if (a == "--reference") o.reference = v;
        else if (a == "--captures") o.captures = v;
        else if (a == "--manifest") o.manifest = v;
        else if (a == "--cards") o.cards = v;
        else if (a == "--max-distance") o.max_distance_db = std::atof(v.c_str());
        else return false;
    }
    if (o.manifest.empty()) o.manifest = (fs::path(o.captures) / "manifest.json").string();
    return !o.reference.empty() && o.max_distance_db > 0.0;
}

bool read_file(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// The ids in the capture manifest, in file order. An absent `entries` is
// an error, an empty one is the shipped state. A missing manifest in a
// captures directory with nothing in it is the same shipped state.
bool manifest_ids(const Options& o, std::vector<std::string>& ids, std::string& error) {
    std::string text;
    if (!read_file(o.manifest, text)) {
        error = o.manifest + ": cannot read the capture manifest";
        return false;
    }
    Value root;
    sonora::json::Error jerr;
    if (!sonora::json::parse(text, root, jerr)) {
        error = o.manifest + ": " + jerr.message;
        return false;
    }
    const Value* entries = root.is_object() ? root.find("entries") : nullptr;
    if (!entries || !entries->is_array()) {
        error = o.manifest + ": no 'entries' array";
        return false;
    }
    for (const Value& e : entries->items()) {
        const Value* id = e.is_object() ? e.find("id") : nullptr;
        if (!id || !id->is_string()) {
            error = o.manifest + ": an entry has no string 'id'";
            return false;
        }
        ids.push_back(id->text());
    }
    return true;
}

bool card_digest(const Options& o, const std::string& id, std::string& sha) {
    std::string bytes;
    if (!tone_detail::is_card_id(id) || !read_file(fs::path(o.cards) / (id + ".md"), bytes)) return false;
    sha = sonora::sha256_hex(bytes);
    return true;
}

// One capture's profile, with the card's settings. A capture without a
// profile fails: profiles are committed derived artefacts, so a missing
// one is a build gap, not "no captures". `skip` is set for an IR card,
// which has no profile of its own.
bool load_capture(const Options& o, const std::string& id, CaptureProfile& p, bool& skip, std::string& error) {
    std::string md;
    skip = false;
    if (!tone_detail::is_card_id(id) || !read_file(fs::path(o.cards) / (id + ".md"), md)) {
        error = "capture " + id + ": its card is missing";
        return false;
    }
    sonora::ampcab::CaptureCard card;
    const auto refusals = sonora::ampcab::parse_card(md, card);
    if (!refusals.empty()) {
        error = "capture " + id + ": " + refusals.front();
        return false;
    }
    if (card.kind == sonora::ampcab::CardKind::ir) {
        skip = true;
        return true;
    }
    std::string text;
    if (!read_file(fs::path(o.captures) / "profiles" / (id + ".json"), text)) {
        error = "capture " + id + ": its profile is missing (run ampcab-tone-profile)";
        return false;
    }
    if (!parse_profile(text, p, error)) return false;
    if (p.id != id) {
        error = "profile " + id + ": names another capture";
        return false;
    }
    p.amp_settings = card.settings;
    return true;
}

bool load_library(const Options& o, std::vector<CaptureProfile>& library, std::string& error) {
    std::vector<std::string> ids;
    if (!manifest_ids(o, ids, error)) return false;
    for (const std::string& id : ids) {
        CaptureProfile p;
        bool skip = false;
        if (!load_capture(o, id, p, skip, error)) return false;
        if (!skip) library.push_back(std::move(p));
    }
    CardDigestLookup digest = [&](const std::string& id, std::string& sha) { return card_digest(o, id, sha); };
    return check_library_fresh(library, digest, error);
}

Value eq_band_json(const EqBand& e) {
    Value o = Value::object();
    o.set("freq_hz", num(e.freq_hz));
    o.set("gain_db", num(e.gain_db));
    o.set("q", num(e.q));
    return o;
}

Value suggestion_json(const ToneSuggestion& s) {
    Value o = Value::object();
    o.set("capture_id", Value::string(s.capture_id));
    o.set("ir_id", Value::string(s.ir_id));
    Value settings = Value::object();
    for (const auto& kv : s.amp_settings) settings.set(kv.first, Value::string(kv.second));
    o.set("amp_settings", std::move(settings));
    o.set("distance_db", num(s.distance_db));
    o.set("crest_delta_db", num(s.crest_delta_db));
    o.set("flatness_delta", num(s.flatness_delta));
    Value curve = Value::array();
    for (double d : s.eq_curve_db) curve.push(num(d));
    o.set("eq_curve_db", std::move(curve));
    Value bands = Value::array();
    for (const EqBand& e : s.eq_bands) bands.push(eq_band_json(e));
    o.set("eq_bands", std::move(bands));
    return o;
}

Value report_json(const std::string& name, const ToneProfile* ref, double max_distance,
                  const std::vector<ToneSuggestion>& found, const char* message) {
    Value r = Value::object();
    r.set("pass", Value::string("tone"));
    r.set("method", Value::string("third_octave_l2"));
    r.set("reference", Value::string(name));
    Value centres = Value::array();
    for (double f : kThirdOctaveCentreHz) centres.push(num(f));
    r.set("bands_hz", std::move(centres));
    Value db = Value::array();
    for (int b = 0; b < kToneBands; ++b) db.push(ref && ref->valid[b] ? num(ref->bands_db[b]) : Value::null());
    r.set("reference_db", std::move(db));
    if (ref) {
        r.set("reference_crest_db", num(ref->crest_db));
        r.set("reference_flatness", num(ref->flatness));
    }
    r.set("max_distance_db", num(max_distance));
    Value cands = Value::array();
    for (const ToneSuggestion& s : found) cands.push(suggestion_json(s));
    r.set("candidates", std::move(cands));
    if (message) r.set("message", Value::string(message));
    return r;
}

// Ids and numbers only, plus the reference's own file name.
void print_text(const std::string& name, const std::vector<ToneSuggestion>& found, const char* message) {
    std::printf("reference %s\n", name.c_str());
    if (found.empty()) {
        std::printf("%s\n", message);
        return;
    }
    int rank = 1;
    for (const ToneSuggestion& s : found)
        std::printf("%d %s %s %.2f dB\n", rank++, s.capture_id.c_str(), s.ir_id.empty() ? "-" : s.ir_id.c_str(),
                    s.distance_db);
}

bool measure_reference(const Options& o, const RunOptions& run, ToneProfile& ref, std::string& error) {
    WavSource src;
    if (!src.open(o.reference, error)) {
        error = o.reference + ": " + error;
        return false;
    }
    return measure_tone_profile(src, run, ref, error);
}

}  // namespace

namespace sonora::analyze::app {

int run_tone_command(const std::vector<std::string>& args, const std::atomic<bool>& cancel) {
    Options o;
    if (!parse_args(args, o)) return usage();
    const std::string name = fs::path(o.reference).filename().string();

    std::string error;
    std::vector<CaptureProfile> library;
    if (!load_library(o, library, error)) return fail(error);

    ToneProfile ref;
    std::vector<ToneSuggestion> found;
    const char* message = nullptr;
    const bool have_ref = !library.empty();
    if (library.empty()) {
        // Nothing to match against: the answer does not depend on the
        // reference, so it is not read.
        message = kToneNoCapturesInstalled;
    } else {
        RunOptions run;
        run.cancel = &cancel;
        if (!measure_reference(o, run, ref, error)) return error == "cancelled" ? 130 : fail(error);
        ToneMatchParams params;
        params.max_distance_db = o.max_distance_db;
        ClassicalToneMatcher matcher(params);
        if (!matcher.match(ref, library, found, error)) return fail(error);
        if (found.empty()) message = kToneNoCloseCapture;
    }

    if (!o.json) {
        print_text(name, found, message);
        return 0;
    }
    std::string text;
    if (!sonora::json::write(report_json(name, have_ref ? &ref : nullptr, o.max_distance_db, found, message), text,
                             error))
        return fail(error);
    std::cout << text;
    return 0;
}

}  // namespace sonora::analyze::app
