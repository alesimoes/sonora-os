// SPDX-License-Identifier: GPL-3.0-or-later
//
// The analysis contract (ADR-0066): the manifest parser's refusals, the
// loader's policy and hash checks, the enum names the reports carry, and
// a round trip through each fake.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "sonora/analyze/analyze_fakes.h"
#include "sonora/analyze/inference.h"
#include "sonora/analyze/key_detector.h"
#include "sonora/analyze/model_manifest.h"
#include "sonora/analyze/timbre_match.h"

using namespace sonora::analyze;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

const char* const kSha = "fc92072f1b9b19552ce3b74a9c8ce0cecb97633a12ae524ac0d93c05800e354e";

// One valid entry; `field` (and `field2`) replaced by raw JSON, or dropped
// when the value is null.
std::string entry(const std::string& field = "", const char* value = "", const std::string& field2 = "",
                  const char* value2 = "") {
    const std::vector<std::pair<std::string, std::string>> fields = {
        {"name", "\"keynet\""},
        {"version", "\"1\""},
        {"file", "\"key/keynet.onnx\""},
        {"url", "\"https://example.org/keynet.onnx\""},
        {"sha256", std::string("\"") + kSha + "\""},
        {"size_bytes", "1852740"},
        {"licence", "\"MIT\""},
        {"licence_url", "\"https://example.org/LICENSE\""},
        {"weights_status", "\"conditional\""},
        {"training_data_note", "\"GiantSteps-MTG, licence not determined\""},
        {"card", "\"brain/models/keynet.md\""},
        {"onnx_opset", "17"},
        {"input", "{\"sample_rate\": 44100, \"channels\": 1, \"layout\": \"cqt\"}"},
        {"outputs", "[\"logits\"]"},
        {"ort_min_api", "21"},
        {"attribution", "\"MusicalKeyCNN, MIT\""},
    };
    std::string s = "{";
    bool first = true;
    for (const auto& f : fields) {
        std::string v = f.second;
        if (f.first == field || f.first == field2) {
            const char* with = f.first == field ? value : value2;
            if (!with) continue;
            v = with;
        }
        if (!first) s += ",";
        first = false;
        s += "\"" + f.first + "\":" + v;
    }
    return s + "}";
}

std::string manifest(const std::string& models) { return "{\"schema\":1,\"models\":[" + models + "]}"; }

bool refused(const std::string& text, const char* expect_in_error) {
    Manifest m;
    std::string error;
    if (parse_manifest(text, m, error)) return false;
    if (error.find(expect_in_error) == std::string::npos) {
        std::fprintf(stderr, "  error was: %s\n", error.c_str());
        return false;
    }
    return true;
}

void test_manifest_accepts() {
    Manifest m;
    std::string error;
    check(parse_manifest(manifest(entry()), m, error), "a valid manifest parses");
    check(m.models.size() == 1, "one model");
    const ManifestEntry* e = find_model(m, "keynet");
    check(e != nullptr, "find_model by name");
    if (!e) return;
    check(e->size_bytes == 1852740 && e->onnx_opset == 17 && e->ort_min_api == 21, "integers");
    check(e->weights_status == WeightsStatus::conditional, "weights_status");
    check(e->input.sample_rate == 44100.0 && e->input.channels == 1 && e->input.layout == "cqt", "input");
    check(e->outputs.size() == 1 && e->outputs[0] == "logits", "outputs");
    check(find_model(m, "other") == nullptr, "an unknown name is not found");
    check(parse_manifest(manifest(""), m, error) && m.models.empty(), "an empty list parses");
}

void test_manifest_refusals() {
    check(refused("not json", "manifest:"), "invalid JSON");
    check(refused("[]", "not an object"), "a root that is not an object");
    check(refused("{\"schema\":2,\"models\":[]}", "schema: unsupported"), "schema 2");
    check(refused("{\"models\":[]}", "schema: missing"), "no schema");
    check(refused("{\"schema\":1,\"models\":[],\"extra\":1}", "extra: unknown field"), "unknown root field");
    check(refused(manifest("1"), "models[0]: not an object"), "an entry that is not an object");
    check(refused(manifest(entry("name", nullptr)), "models[0].name: missing"), "missing name");
    check(refused(manifest(entry("name", "\"Key Net\"")), "name: not [a-z0-9_-]+"), "a name with capitals");
    check(refused(manifest(entry() + "," + entry()), "models[1].name: duplicate"), "a duplicate name");
    check(refused(manifest(entry("file", "\"/abs/keynet.onnx\"")), "file: not a safe"), "an absolute file");
    check(refused(manifest(entry("file", "\"../keynet.onnx\"")), "file: not a safe"), "a file with ..");
    check(refused(manifest(entry("file", "\"key\\\\keynet.onnx\"")), "file: not a safe"), "a backslash");
    check(refused(manifest(entry("url", "\"http://example.org/k\"")), "url: not https"), "a plain http url");
    check(refused(manifest(entry("sha256", "\"FC92\"")), "sha256: not 64"), "a short sha256");
    check(refused(manifest(entry("sha256", "\"FC92072F1B9B19552CE3B74A9C8CE0CECB97633A12AE524AC0D93C05800E354E\"")),
                  "sha256: not 64"),
          "an upper-case sha256");
    check(refused(manifest(entry("size_bytes", "0")), "size_bytes: out of range"), "size 0");
    check(refused(manifest(entry("size_bytes", "1.5")), "size_bytes: not an integer"), "a fractional size");
    check(refused(manifest(entry("weights_status", "\"maybe\"")), "weights_status: not cleared|conditional"),
          "an unknown weights_status");
    check(refused(manifest(entry("training_data_note", "\"\"")), "training_data_note: empty for a conditional"),
          "a conditional model without its caveat");
    check(refused(manifest(entry("onnx_opset", "0")), "onnx_opset: out of range"), "opset 0");
    check(refused(manifest(entry("input", "{\"sample_rate\":0,\"channels\":1,\"layout\":\"x\"}")),
                  "input.sample_rate: out of range"),
          "sample rate 0");
    check(refused(manifest(entry("input", "{\"sample_rate\":16000,\"channels\":1}")), "input.layout: missing"),
          "input without layout");
    check(refused(manifest(entry("outputs", "[]")), "outputs: empty"), "no outputs");
    check(refused(manifest(entry("outputs", "[1]")), "outputs: not a non-empty string"), "a numeric output");
    check(refused(manifest(entry("card", nullptr)), "card: missing"), "missing card");
    check(refused(manifest(entry("licence", "\"\"")), "licence: empty"), "an empty licence");

    // A cleared model may have an empty note.
    Manifest m;
    std::string error;
    check(parse_manifest(manifest(entry("weights_status", "\"cleared\"", "training_data_note", "\"\"")), m, error),
          "a cleared model with an empty training_data_note");
}

ManifestEntry parsed_entry(const char* status) {
    Manifest m;
    std::string error;
    parse_manifest(manifest(entry("weights_status", status)), m, error);
    return m.models.empty() ? ManifestEntry{} : m.models[0];
}

void test_verify_file() {
    const ManifestEntry conditional = parsed_entry("\"conditional\"");
    const ManifestEntry cleared = parsed_entry("\"cleared\"");
    FakeFileDigest digest;
    digest.files["/models/key/keynet.onnx"] = {1852740, kSha};
    std::string error;

    ModelPolicy strict;
    check(!verify_file(conditional, "/models", strict, digest, error), "conditional refused by default");
    check(error.find("allow_conditional_models") != std::string::npos, "the refusal names the switch");
    check(digest.asked.empty(), "a refused model is not hashed");

    ModelPolicy open;
    open.allow_conditional_models = true;
    check(verify_file(conditional, "/models", open, digest, error), "conditional accepted when allowed");
    check(verify_file(cleared, "/models", strict, digest, error), "cleared accepted by default");

    digest.files["/models/key/keynet.onnx"].sha256[0] = '0';
    check(!verify_file(cleared, "/models", strict, digest, error), "sha256 mismatch refused");
    check(error.find("sha256 mismatch") != std::string::npos, "the refusal says sha256");

    digest.files["/models/key/keynet.onnx"] = {1, kSha};
    check(!verify_file(cleared, "/models", strict, digest, error), "size mismatch refused");

    check(!verify_file(cleared, "/elsewhere", strict, digest, error), "a missing file refused");
    check(error.find("no such file") != std::string::npos, "the refusal says why");
}

void test_enum_names() {
    check(std::string(key_mode_name(KeyMode::major)) == "major", "major");
    check(std::string(key_mode_name(KeyMode::minor)) == "minor", "minor");
    check(std::string(key_method_name(KeyMethod::krumhansl)) == "krumhansl", "krumhansl");
    check(std::string(key_method_name(KeyMethod::keynet)) == "keynet", "keynet");
    check(std::string(weights_status_name(WeightsStatus::cleared)) == "cleared", "cleared");
    check(std::string(weights_status_name(WeightsStatus::conditional)) == "conditional", "conditional");
    KeyMode mode;
    KeyMethod method;
    WeightsStatus status;
    check(parse_key_mode("minor", mode) && mode == KeyMode::minor, "parse minor");
    check(!parse_key_mode("dorian", mode), "an unknown mode");
    check(parse_key_method("keynet", method) && method == KeyMethod::keynet, "parse keynet");
    check(!parse_key_method("skey", method), "an unknown method");
    check(parse_weights_status("cleared", status) && status == WeightsStatus::cleared, "parse cleared");
    check(!parse_weights_status("Cleared", status), "names are case-sensitive");
    check(std::string(tonic_name(0)) == "C" && std::string(tonic_name(1)) == "C#" &&
              std::string(tonic_name(11)) == "B",
          "tonic names");
    check(tonic_name(12) == nullptr && tonic_name(-1) == nullptr, "no tonic outside 0..11");
    check(kThirdOctaveCentreHz[0] == 20.0 && kThirdOctaveCentreHz[17] == 1000.0 &&
              kThirdOctaveCentreHz[kTimbreBands - 1] == 20000.0,
          "the 31 ISO 266 centres");
}

void test_key_fake() {
    MemorySource src({std::vector<float>(480, 0.0f)}, 48000.0);
    FakeKeyDetector det;
    det.result = {9, KeyMode::minor, 0.8, KeyMethod::krumhansl};
    KeyResult r;
    std::string error;
    RunOptions run;
    check(det.detect(src, run, r, error), "fake detect");
    check(r.tonic == 9 && r.mode == KeyMode::minor && r.confidence == 0.8 && key_result_valid(r), "round trip");

    std::atomic<bool> cancel{true};
    run.cancel = &cancel;
    check(!det.detect(src, run, r, error) && error == "cancelled", "cancel honoured");

    run.cancel = nullptr;
    det.fail_with = "keynet: model unavailable";
    check(!det.detect(src, run, r, error) && error == "keynet: model unavailable", "a failure is reported");
    check(!key_result_valid(KeyResult{12, KeyMode::major, 0.5, KeyMethod::krumhansl}), "tonic 12 invalid");
    check(!key_result_valid(KeyResult{0, KeyMode::major, 1.5, KeyMethod::krumhansl}), "confidence 1.5 invalid");
}

void test_timbre_fake() {
    TimbreCurve ref, tgt;
    for (int b = 0; b < kTimbreBands; ++b) {
        ref.valid[b] = tgt.valid[b] = true;
        ref.gain_db[b] = b == 20 ? 6.0 : 0.0;
        tgt.gain_db[b] = b == 5 ? 30.0 : 0.0;
    }
    tgt.valid[0] = false;
    ref.gain_db[0] = 9.0;
    FakeTimbreMatcher matcher;
    matcher.bands = {{2000.0, 3.0, 1.0}, {63.0, -12.0, 0.7}};
    TimbreMatchParams params;
    params.amount = 0.5;
    params.max_bands = 1;
    TimbreMatchResult r;
    std::string error;
    check(matcher.match(ref, tgt, params, r, error), "fake match");
    check(r.curve_db[20] == 3.0, "difference scaled by amount");
    check(r.curve_db[5] == -12.0, "clamped to max_gain_db");
    check(r.curve_db[0] == 0.0, "an invalid band contributes nothing");
    check(r.eq_bands.size() == 1 && r.eq_bands[0].freq_hz == 2000.0, "at most max_bands");
    params.amount = 2.0;
    check(!matcher.match(ref, tgt, params, r, error), "amount out of range refused");
    check(timbre_params_valid(TimbreMatchParams{}), "default params valid");

    FakeTimbreMeasurer measurer;
    measurer.curve = ref;
    MemorySource src({std::vector<float>(480, 0.0f)}, 48000.0);
    TimbreCurve got;
    check(measurer.measure(src, RunOptions{}, got, error) && got.gain_db[20] == 6.0, "fake measure");
}

void test_inference_fake() {
    check(element_count({1, 105, 50}) == 5250, "element count");
    check(element_count({}) == 1, "a scalar");
    check(element_count({2, -1}) == -1, "a negative dimension");
    check(element_count({INT64_MAX, 2}) == -1, "an overflow");

    Manifest m;
    std::string error;
    parse_manifest(manifest(entry()), m, error);
    if (m.models.empty()) {
        check(false, "manifest for the inference test");
        return;
    }
    FakeRunnerFactory factory;
    auto runner = factory.open(m.models[0], "/models/key/keynet.onnx", error);
    check(runner != nullptr, "the factory opens a runner");
    if (!runner) return;
    Tensor in{"cqt", {1, 2, 3}, {1, 2, 3, 4, 5, 6}};
    std::vector<Tensor> out;
    check(runner->run({in}, out, error), "run");
    check(out.size() == 1 && out[0].name == "logits" && out[0].data == in.data, "round trip, named as the manifest");
    Tensor bad{"cqt", {1, 4}, {1, 2}};
    check(!runner->run({bad}, out, error), "a tensor whose data does not fit its shape is refused");

    factory.available = false;
    check(factory.open(m.models[0], "/x", error) == nullptr, "no runner without ONNX Runtime");
    check(error.find("built without ONNX Runtime") != std::string::npos, "and it says so");
}

}  // namespace

int main() {
    test_manifest_accepts();
    test_manifest_refusals();
    test_verify_file();
    test_enum_names();
    test_key_fake();
    test_timbre_fake();
    test_inference_fake();
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("analyze contract: ok");
    return 0;
}
