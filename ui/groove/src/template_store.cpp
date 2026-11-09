// SPDX-License-Identifier: GPL-3.0-or-later
//
// groove-templates.json, version 1:
//   { "format": "sonora.groove-templates", "version": 1,
//     "templates": [ { "name": "Pocket", "slots_per_beat": 4,
//                      "offsets": [0.0, 0.12, 0.0, 0.1], "gains": [1.0, ...] } ] }
// Written whole to a temporary file and renamed over the old one, so a crash
// mid-save leaves the previous file, never half of one.
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "sonora/json/json.h"
#include "sonora/ui_groove/template_store.h"

namespace sonora::ui {

namespace fs = std::filesystem;
using json::Value;

namespace {

constexpr const char* kFormat = "sonora.groove-templates";

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : std::string();
}

bool name_problem(const std::string& name, std::string& why) {
    if (name.empty() || name.size() > kMaxGrooveNameBytes || !json::valid_utf8(name)) {
        why = "a groove's name is 1 to 64 characters of text";
        return true;
    }
    return false;
}

bool number_list(const Value* v, std::size_t n, std::vector<double>& out) {
    if (v == nullptr || !v->is_array() || v->items().size() != n) return false;
    out.clear();
    for (const Value& x : v->items()) {
        double d = 0.0;
        if (!x.is_number() || !x.to_double(d)) return false;
        out.push_back(d);
    }
    return true;
}

// One entry of "templates", checked as groove_moves() would check it, so a
// hand-edited file is refused here rather than at Apply.
bool read_entry(const Value& e, NamedGroove& out, std::string& why) {
    const Value* name = e.is_object() ? e.find("name") : nullptr;
    const Value* spb = e.is_object() ? e.find("slots_per_beat") : nullptr;
    std::int64_t slots = 0;
    if (name == nullptr || !name->is_string() || spb == nullptr || !spb->to_int64(slots) ||
        slots < 1 || slots > static_cast<std::int64_t>(groove::kMaxGrooveSlots)) {
        why = "a template needs a name and 1 to 64 slots_per_beat";
        return false;
    }
    if (name_problem(name->text(), why)) return false;
    std::vector<double> offsets, gains;
    const auto n = static_cast<std::size_t>(slots);
    if (!number_list(e.find("offsets"), n, offsets) || !number_list(e.find("gains"), n, gains)) {
        why = "\"" + name->text() + "\" needs one offset and one gain per slot";
        return false;
    }
    out.name = name->text();
    out.groove.slots_per_beat = n;
    out.groove.slots.assign(n, groove::GrooveSlot{});
    for (std::size_t i = 0; i < n; ++i) {
        if (!(std::abs(offsets[i]) <= groove::kMaxSlotOffset) || !(gains[i] >= 0.0)) {
            why = "\"" + name->text() + "\" has an offset past half a slot or a negative gain";
            return false;
        }
        out.groove.slots[i] = groove::GrooveSlot{offsets[i], gains[i]};
    }
    return true;
}

bool read_document(const Value& doc, std::vector<NamedGroove>& out, std::string& why) {
    const Value* format = doc.is_object() ? doc.find("format") : nullptr;
    const Value* version = doc.is_object() ? doc.find("version") : nullptr;
    std::int64_t v = 0;
    if (format == nullptr || !format->is_string() || format->text() != kFormat ||
        version == nullptr || !version->to_int64(v)) {
        why = "it is not a groove template file";
        return false;
    }
    if (v != kGrooveTemplatesVersion) {
        why = "it is version " + std::to_string(v) + "; this Sonora reads version " +
              std::to_string(kGrooveTemplatesVersion);
        return false;
    }
    const Value* list = doc.find("templates");
    if (list == nullptr || !list->is_array() || list->items().size() > kMaxSavedGrooves) {
        why = "\"templates\" must be a list of at most 256";
        return false;
    }
    std::vector<NamedGroove> got;
    for (const Value& e : list->items()) {
        NamedGroove g;
        if (!read_entry(e, g, why)) return false;
        for (const NamedGroove& seen : got) {
            if (seen.name == g.name) {
                why = "\"" + g.name + "\" is there twice";
                return false;
            }
        }
        got.push_back(std::move(g));
    }
    out = std::move(got);
    return true;
}

Value to_json(const std::vector<NamedGroove>& all) {
    Value doc = Value::object();
    doc.set("format", Value::string(kFormat));
    doc.set("version", Value::integer(kGrooveTemplatesVersion));
    Value list = Value::array();
    for (const NamedGroove& g : all) {
        Value e = Value::object();
        e.set("name", Value::string(g.name));
        e.set("slots_per_beat", Value::integer(static_cast<std::int64_t>(g.groove.slots_per_beat)));
        Value offsets = Value::array(), gains = Value::array();
        for (const auto& s : g.groove.slots) {
            offsets.push(Value::number_float(static_cast<float>(s.offset)));
            gains.push(Value::number_float(static_cast<float>(s.gain)));
        }
        e.set("offsets", std::move(offsets));
        e.set("gains", std::move(gains));
        list.push(std::move(e));
    }
    doc.set("templates", std::move(list));
    return doc;
}

}  // namespace

std::string default_groove_templates_file() {
    fs::path dir;
    if (const std::string prefs = env("SONORA_PREFERENCES_FILE"); !prefs.empty()) {
        dir = fs::path(prefs).parent_path();
    } else if (const std::string xdg = env("XDG_CONFIG_HOME"); !xdg.empty()) {
        dir = fs::path(xdg) / "sonora";
    } else if (const std::string home = env("HOME"); !home.empty()) {
        dir = fs::path(home) / ".config" / "sonora";
    } else {
        return {};
    }
    return (dir / "groove-templates.json").string();
}

bool GrooveTemplateStore::load(std::vector<NamedGroove>& out, std::string& why) const {
    std::error_code ec;
    if (!fs::exists(file_, ec)) {
        out.clear();
        return true;
    }
    std::ifstream in(file_, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    if (!in.good() && !in.eof()) {
        why = "cannot read the saved grooves in " + file_;
        return false;
    }
    Value doc;
    json::Error err;
    std::string problem;
    if (!json::parse(text.str(), doc, err)) {
        problem = err.message + " at line " + std::to_string(err.line);
    } else if (!read_document(doc, out, problem)) {
        out.clear();
    } else {
        return true;
    }
    why = "the saved grooves in " + file_ + " were not read: " + problem +
          ". Move the file aside to start a new one.";
    return false;
}

bool GrooveTemplateStore::find(const std::string& name, groove::GrooveTemplate& out,
                               std::string& why) const {
    std::vector<NamedGroove> all;
    if (!load(all, why)) return false;
    for (const NamedGroove& g : all) {
        if (g.name == name) {
            out = g.groove;
            return true;
        }
    }
    why = all.empty() ? "no groove is saved yet; save one first"
                      : "no saved groove is called \"" + name + "\"";
    return false;
}

bool GrooveTemplateStore::save(const std::string& name, const groove::GrooveTemplate& tmpl,
                               std::string& why) const {
    if (name_problem(name, why)) return false;
    std::vector<NamedGroove> all;
    if (!load(all, why)) return false;
    bool replaced = false;
    for (NamedGroove& g : all) {
        if (g.name == name) {
            g.groove = tmpl;
            replaced = true;
        }
    }
    if (!replaced) {
        if (all.size() >= kMaxSavedGrooves) {
            why = "256 grooves are saved already; replace one by saving under its name";
            return false;
        }
        all.push_back(NamedGroove{name, tmpl});
    }
    std::string text, error;
    if (!json::write(to_json(all), text, error)) {
        why = "the groove could not be written: " + error;
        return false;
    }
    std::error_code ec;
    const fs::path path(file_);
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream outf(tmp, std::ios::binary | std::ios::trunc);
        outf << text;
        outf.flush();
        if (!outf.good()) {
            why = "cannot write " + tmp.string();
            return false;
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        why = "cannot replace " + file_ + ": " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

}  // namespace sonora::ui
