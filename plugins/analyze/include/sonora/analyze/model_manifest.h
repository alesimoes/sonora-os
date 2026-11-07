// SPDX-License-Identifier: GPL-3.0-or-later
//
// plugins/native/models/manifest.json (ADR-0066 §3): which pretrained
// models exist, where their files sit after install, their pinned sha256
// and their licence state. The schema is the contract, so the parser is
// here, inline. The loader never downloads: the image build fetches the
// files by this manifest, and verify_file refuses a file whose size or
// sha256 differs and a `conditional` model unless the operator opted in.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sonora/json/json.h"

namespace sonora::analyze {

enum class WeightsStatus { cleared, conditional };

struct ModelInput {
    double sample_rate = 0.0;
    std::int64_t channels = 0;
    std::string layout;
};

struct ManifestEntry {
    std::string name;
    std::string version;
    std::string file;  // relative to the models directory
    std::string url;   // for the image build only
    std::string sha256;
    std::int64_t size_bytes = 0;
    std::string licence;
    std::string licence_url;
    WeightsStatus weights_status = WeightsStatus::conditional;
    std::string training_data_note;
    std::string card;
    std::int64_t onnx_opset = 0;
    ModelInput input;
    std::vector<std::string> outputs;
    std::int64_t ort_min_api = 0;
    std::string attribution;
};

struct Manifest {
    std::int64_t schema = 1;
    std::vector<ManifestEntry> models;
};

// The operator's settings; refusing a conditional model is the default.
struct ModelPolicy {
    bool allow_conditional_models = false;
};

// Size and sha256 of a file, injected so tests need no file. False with
// `error` when the file is missing or unreadable.
class FileDigest {
public:
    virtual ~FileDigest() = default;
    virtual bool digest(const std::string& path, std::int64_t& size_bytes, std::string& sha256_hex,
                        std::string& error) = 0;
};

inline const char* weights_status_name(WeightsStatus s) noexcept {
    return s == WeightsStatus::cleared ? "cleared" : "conditional";
}

inline bool parse_weights_status(std::string_view s, WeightsStatus& out) noexcept {
    if (s == "cleared") { out = WeightsStatus::cleared; return true; }
    if (s == "conditional") { out = WeightsStatus::conditional; return true; }
    return false;
}

namespace manifest_detail {

using json::Value;

inline bool is_sha256_hex(std::string_view s) noexcept {
    if (s.size() != 64) return false;
    for (const char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

inline bool is_model_name(std::string_view s) noexcept {
    if (s.empty()) return false;
    for (const char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}

// A relative path that cannot leave the models directory.
inline bool is_safe_relative(std::string_view s) noexcept {
    if (s.empty() || s.front() == '/' || s.find('\\') != std::string_view::npos ||
        s.find(':') != std::string_view::npos)
        return false;
    std::size_t start = 0;
    while (start <= s.size()) {
        const std::size_t end = std::min(s.find('/', start), s.size());
        const std::string_view part = s.substr(start, end - start);
        if (part.empty() || part == "..") return false;
        start = end + 1;
    }
    return true;
}

class Reader {
public:
    Reader(const Value& obj, std::string where, std::string& error) : obj_(obj), where_(std::move(where)), error_(error) {}

    bool only(std::initializer_list<std::string_view> known) {
        for (const auto& m : obj_.members()) {
            bool ok = false;
            for (const std::string_view k : known) ok = ok || m.first == k;
            if (!ok) return fail(m.first, "unknown field");
        }
        return true;
    }
    bool string(std::string_view key, std::string& out, bool allow_empty = false) {
        const Value* v = obj_.find(key);
        if (!v) return fail(key, "missing");
        if (!v->is_string()) return fail(key, "not a string");
        if (!allow_empty && v->text().empty()) return fail(key, "empty");
        out = v->text();
        return true;
    }
    bool integer(std::string_view key, std::int64_t min, std::int64_t& out) {
        const Value* v = obj_.find(key);
        if (!v) return fail(key, "missing");
        if (!v->is_number() || !v->is_integer_lexeme() || !v->to_int64(out)) return fail(key, "not an integer");
        if (out < min) return fail(key, "out of range");
        return true;
    }
    bool positive(std::string_view key, double& out) {
        const Value* v = obj_.find(key);
        if (!v) return fail(key, "missing");
        if (!v->is_number() || !v->to_double(out)) return fail(key, "not a number");
        if (!(out > 0.0)) return fail(key, "out of range");
        return true;
    }
    const Value* member(std::string_view key, Value::Type type) {
        const Value* v = obj_.find(key);
        if (!v) { fail(key, "missing"); return nullptr; }
        if (v->type() != type) { fail(key, "wrong type"); return nullptr; }
        return v;
    }
    bool fail(std::string_view key, const char* why) {
        error_ = where_ + "." + std::string(key) + ": " + why;
        return false;
    }

private:
    const Value& obj_;
    std::string where_;
    std::string& error_;
};

inline bool parse_input(const Value& v, const std::string& where, ModelInput& out, std::string& error) {
    Reader r(v, where, error);
    return r.only({"sample_rate", "channels", "layout"}) && r.positive("sample_rate", out.sample_rate) &&
           r.integer("channels", 1, out.channels) && r.string("layout", out.layout);
}

inline bool parse_outputs(const Value& v, Reader& r, std::vector<std::string>& out) {
    if (v.items().empty()) return r.fail("outputs", "empty");
    for (const Value& o : v.items()) {
        if (!o.is_string() || o.text().empty()) return r.fail("outputs", "not a non-empty string");
        out.push_back(o.text());
    }
    return true;
}

inline bool parse_entry(const Value& v, const std::string& where, ManifestEntry& e, std::string& error) {
    if (!v.is_object()) {
        error = where + ": not an object";
        return false;
    }
    Reader r(v, where, error);
    std::string status;
    const bool fields =
        r.only({"name", "version", "file", "url", "sha256", "size_bytes", "licence", "licence_url",
                "weights_status", "training_data_note", "card", "onnx_opset", "input", "outputs", "ort_min_api",
                "attribution"}) &&
        r.string("name", e.name) && r.string("version", e.version) && r.string("file", e.file) &&
        r.string("url", e.url) && r.string("sha256", e.sha256) && r.integer("size_bytes", 1, e.size_bytes) &&
        r.string("licence", e.licence) && r.string("licence_url", e.licence_url) &&
        r.string("weights_status", status) && r.string("training_data_note", e.training_data_note, true) &&
        r.string("card", e.card) && r.integer("onnx_opset", 1, e.onnx_opset) &&
        r.integer("ort_min_api", 1, e.ort_min_api) && r.string("attribution", e.attribution);
    if (!fields) return false;
    if (!is_model_name(e.name)) return r.fail("name", "not [a-z0-9_-]+");
    if (!is_safe_relative(e.file)) return r.fail("file", "not a safe relative path");
    if (e.url.compare(0, 8, "https://") != 0) return r.fail("url", "not https");
    if (!is_sha256_hex(e.sha256)) return r.fail("sha256", "not 64 lowercase hex digits");
    if (!parse_weights_status(status, e.weights_status)) return r.fail("weights_status", "not cleared|conditional");
    if (e.weights_status == WeightsStatus::conditional && e.training_data_note.empty())
        return r.fail("training_data_note", "empty for a conditional model");
    const Value* input = r.member("input", Value::Type::Object);
    if (!input || !parse_input(*input, where + ".input", e.input, error)) return false;
    const Value* outputs = r.member("outputs", Value::Type::Array);
    return outputs && parse_outputs(*outputs, r, e.outputs);
}

}  // namespace manifest_detail

// The whole file or nothing: false with `error` naming the entry and the
// field of the first violation (ADR-0066 §3).
inline bool parse_manifest(std::string_view text, Manifest& out, std::string& error) {
    using json::Value;
    Value root;
    json::Error jerr;
    if (!json::parse(text, root, jerr)) {
        error = "manifest: " + jerr.message + " at line " + std::to_string(jerr.line);
        return false;
    }
    if (!root.is_object()) {
        error = "manifest: not an object";
        return false;
    }
    manifest_detail::Reader r(root, "manifest", error);
    Manifest m;
    if (!r.only({"schema", "models"}) || !r.integer("schema", 1, m.schema)) return false;
    if (m.schema != 1) return r.fail("schema", "unsupported");
    const Value* models = r.member("models", Value::Type::Array);
    if (!models) return false;
    for (std::size_t i = 0; i < models->items().size(); ++i) {
        ManifestEntry e;
        const std::string where = "manifest.models[" + std::to_string(i) + "]";
        if (!manifest_detail::parse_entry(models->items()[i], where, e, error)) return false;
        for (const ManifestEntry& prev : m.models)
            if (prev.name == e.name) {
                error = where + ".name: duplicate " + e.name;
                return false;
            }
        m.models.push_back(std::move(e));
    }
    out = std::move(m);
    return true;
}

inline const ManifestEntry* find_model(const Manifest& m, std::string_view name) noexcept {
    for (const ManifestEntry& e : m.models)
        if (e.name == name) return &e;
    return nullptr;
}

// Policy first (a refused model is not even hashed), then size, then
// sha256. False with `error` naming the model and the cause.
inline bool verify_file(const ManifestEntry& entry, const std::string& models_dir, const ModelPolicy& policy,
                        FileDigest& digest, std::string& error) {
    if (entry.weights_status == WeightsStatus::conditional && !policy.allow_conditional_models) {
        error = entry.name + ": conditional weights refused (allow_conditional_models is off)";
        return false;
    }
    const std::string path = models_dir.empty() ? entry.file : models_dir + "/" + entry.file;
    std::int64_t size = 0;
    std::string hex;
    std::string why;
    if (!digest.digest(path, size, hex, why)) {
        error = entry.name + ": " + path + ": " + why;
        return false;
    }
    if (size != entry.size_bytes) {
        error = entry.name + ": size " + std::to_string(size) + " differs from the manifest's " +
                std::to_string(entry.size_bytes);
        return false;
    }
    if (hex != entry.sha256) {
        error = entry.name + ": sha256 mismatch";
        return false;
    }
    return true;
}

}  // namespace sonora::analyze
