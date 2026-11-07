// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/native/state.h"

#include <cstdint>
#include <vector>

#include "sonora/json/json.h"

namespace sonora::native {

namespace {

using json::Value;

const char* unit_name(Unit u) {
    switch (u) {
        case Unit::None: return "none";
        case Unit::Decibel: return "dB";
        case Unit::Hertz: return "Hz";
        case Unit::Milliseconds: return "ms";
        case Unit::Percent: return "%";
        case Unit::Ratio: return "ratio";
        case Unit::Q: return "Q";
        case Unit::Bpm: return "BPM";
        case Unit::Choice: return "choice";
        case Unit::Toggle: return "toggle";
        case Unit::Seconds: return "s";
    }
    return "none";
}

const char* taper_name(Taper t) {
    switch (t) {
        case Taper::Linear: return "linear";
        case Taper::Log: return "log";
        case Taper::Stepped: return "stepped";
    }
    return "linear";
}

const char* widget_name(Widget w) {
    switch (w) {
        case Widget::Auto: return "auto";
        case Widget::Knob: return "knob";
        case Widget::Fader: return "fader";
        case Widget::Switch: return "switch";
        case Widget::Choice: return "choice";
    }
    return "auto";
}

const char* meter_kind_name(MeterKind k) {
    switch (k) {
        case MeterKind::Other: return "other";
        case MeterKind::GainReduction: return "gain_reduction";
        case MeterKind::InputLevel: return "input";
        case MeterKind::OutputLevel: return "output";
    }
    return "other";
}

std::string written(const Value& v) {
    std::string out, err;
    json::write(v, out, err);
    return out;
}

}  // namespace

std::string descriptor_json(const Descriptor& d, const FormatSymbol* symbols,
                            std::uint32_t symbol_count, const FileSlots* files, const UiHints* ui) {
    Value root = Value::object();
    root.set("id", Value::string(d.id));
    root.set("name", Value::string(d.name));
    root.set("category", Value::string(d.category));
    root.set("state_version", Value::integer(d.state_version));
    root.set("min_channels", Value::integer(d.min_channels));
    root.set("max_channels", Value::integer(d.max_channels));
    root.set("sidechain", Value::boolean(d.sidechain));
    Value params = Value::array();
    for (std::uint32_t i = 0; i < d.param_count; ++i) {
        const ParamInfo& p = d.params[i];
        Value o = Value::object();
        o.set("id", Value::string(p.id));
        o.set("index", Value::integer(i));
        o.set("name", Value::string(p.name));
        o.set("unit", Value::string(unit_name(p.unit)));
        o.set("taper", Value::string(taper_name(p.taper)));
        o.set("min", Value::number_float(p.min));
        o.set("max", Value::number_float(p.max));
        o.set("default", Value::number_float(p.def));
        o.set("smoothing_ms", Value::number_float(p.smoothing_ms));
        o.set("automatable", Value::boolean(p.automatable));
        o.set("reconfigures", Value::boolean(p.reconfigures));
        o.set("format", Value::string(p.format));
        if (p.choice_count > 0) {
            Value c = Value::array();
            for (std::uint32_t k = 0; k < p.choice_count; ++k) c.push(Value::string(p.choices[k]));
            o.set("choices", std::move(c));
        }
        Value formats = Value::object();
        bool any = false;
        for (std::uint32_t k = 0; k < symbol_count; ++k) {
            if (symbols[k].index != i) continue;
            Value f = Value::object();
            f.set("symbol", Value::string(symbols[k].symbol));
            f.set("inverted", Value::boolean(symbols[k].inverted));
            formats.set(symbols[k].format, std::move(f));
            any = true;
        }
        if (any) o.set("formats", std::move(formats));
        if (ui != nullptr) {
            for (std::uint32_t k = 0; k < ui->param_count; ++k) {
                const ParamHint& h = ui->params[k];
                if (std::string(h.id) != p.id) continue;
                if (h.section != nullptr) o.set("section", Value::string(h.section));
                if (h.widget != Widget::Auto) o.set("widget", Value::string(widget_name(h.widget)));
                if (h.curve_source != nullptr) o.set("curve_source", Value::string(h.curve_source));
            }
        }
        params.push(std::move(o));
    }
    root.set("params", std::move(params));
    if (ui != nullptr && ui->meter_count > 0) {
        Value meters = Value::array();
        for (std::uint32_t i = 0; i < ui->meter_count; ++i) {
            const MeterDecl& m = ui->meters[i];
            Value o = Value::object();
            o.set("id", Value::string(m.id));
            o.set("name", Value::string(m.name));
            o.set("unit", Value::string(m.unit));
            o.set("kind", Value::string(meter_kind_name(m.kind)));
            o.set("min", Value::number_float(m.min));
            o.set("max", Value::number_float(m.max));
            meters.push(std::move(o));
        }
        root.set("meters", std::move(meters));
    }
    if (ui != nullptr && ui->curve_count > 0) {
        Value curves = Value::array();
        for (std::uint32_t i = 0; i < ui->curve_count; ++i) {
            const CurveDecl& c = ui->curves[i];
            Value o = Value::object();
            o.set("id", Value::string(c.id));
            o.set("name", Value::string(c.name));
            o.set("x_unit", Value::string(c.x_unit));
            o.set("y_unit", Value::string(c.y_unit));
            o.set("points", Value::integer(c.points));
            o.set("x_log", Value::boolean(c.x_log));
            o.set("x_min", Value::number_float(c.x_min));
            o.set("x_max", Value::number_float(c.x_max));
            o.set("y_min", Value::number_float(c.y_min));
            o.set("y_max", Value::number_float(c.y_max));
            curves.push(std::move(o));
        }
        root.set("curves", std::move(curves));
    }
    if (files != nullptr && files->slot_count() > 0) {
        Value slots = Value::array();
        for (std::uint32_t i = 0; i < files->slot_count(); ++i) {
            const FileSlotInfo& s = files->slot(i);
            Value o = Value::object();
            o.set("id", Value::string(s.id));
            o.set("name", Value::string(s.name));
            o.set("extensions", Value::string(s.extensions));
            slots.push(std::move(o));
        }
        root.set("files", std::move(slots));
    }
    return written(root);
}

std::string save_state(const Processor& p) {
    const Descriptor& d = p.descriptor();
    Value root = Value::object();
    root.set("processor", Value::string(d.id));
    root.set("state_version", Value::integer(d.state_version));
    Value params = Value::object();
    for (std::uint32_t i = 0; i < d.param_count; ++i) {
        params.set(d.params[i].id, Value::number_float(p.param(i)));
    }
    root.set("params", std::move(params));
    if (const FileSlots* files = p.files()) {
        Value paths = Value::object();
        bool any = false;
        for (std::uint32_t i = 0; i < files->slot_count(); ++i) {
            const std::string path = files->path(i);
            if (path.empty()) continue;
            paths.set(files->slot(i).id, Value::string(path));
            any = true;
        }
        if (any) root.set("files", std::move(paths));
    }
    return written(root);
}

bool load_state(Processor& p, const std::string& text, std::string& error) {
    const Descriptor& d = p.descriptor();
    Value root;
    json::Error je;
    if (!json::parse(text, root, je)) {
        error = "state is not valid JSON: " + je.message + " at line " + std::to_string(je.line);
        return false;
    }
    const Value* proc = root.find("processor");
    if (proc == nullptr || !proc->is_string() || proc->text() != d.id) {
        error = std::string("state is not for ") + d.id;
        return false;
    }
    const Value* ver = root.find("state_version");
    std::int64_t version = 0;
    if (ver == nullptr || !ver->to_int64(version) || version < 1) {
        error = "state has no valid state_version";
        return false;
    }
    if (version > d.state_version) {
        error = "state version " + std::to_string(version) + " is newer than this processor's " +
                std::to_string(d.state_version);
        return false;
    }
    // Version 1 is the only one yet; a later version adds its migration
    // here, with a test that loads the previous version's golden state.
    const Value* params = root.find("params");
    if (params == nullptr || !params->is_object()) {
        error = "state has no params object";
        return false;
    }
    std::vector<float> values(d.param_count);
    for (std::uint32_t i = 0; i < d.param_count; ++i) values[i] = d.params[i].def;
    for (const auto& kv : params->members()) {
        std::uint32_t idx = d.param_count;
        for (std::uint32_t i = 0; i < d.param_count; ++i) {
            if (kv.first == d.params[i].id) idx = i;
        }
        if (idx == d.param_count) {
            error = "unknown parameter '" + kv.first + "' for " + d.id;
            return false;
        }
        float v = 0.0f;
        if (!kv.second.is_number() || !kv.second.to_float(v)) {
            error = "parameter '" + kv.first + "' is not a number";
            return false;
        }
        values[idx] = sanitize(d.params[idx], v);
    }
    // A state without "files" puts a slot back to its default, as a missing
    // parameter takes its default. Resolved before any value is applied, so
    // a slot this processor does not have, or a file that cannot be read,
    // fails the load with the values untouched.
    FileSlots* files = p.files();
    const std::uint32_t slot_count = files != nullptr ? files->slot_count() : 0;
    std::vector<std::string> wanted(slot_count);
    if (const Value* file_obj = root.find("files")) {
        if (!file_obj->is_object()) {
            error = "state 'files' is not an object";
            return false;
        }
        for (const auto& kv : file_obj->members()) {
            std::uint32_t idx = slot_count;
            for (std::uint32_t i = 0; i < slot_count; ++i) {
                if (kv.first == files->slot(i).id) idx = i;
            }
            if (idx == slot_count) {
                error = "unknown file slot '" + kv.first + "' for " + d.id;
                return false;
            }
            if (!kv.second.is_string()) {
                error = "file slot '" + kv.first + "' is not a path";
                return false;
            }
            wanted[idx] = kv.second.text();
        }
    }
    for (std::uint32_t i = 0; i < slot_count; ++i) {
        std::string why;
        if (!files->load(i, wanted[i], why)) {
            error = std::string("file slot '") + files->slot(i).id + "': " + why;
            return false;
        }
    }
    for (std::uint32_t i = 0; i < d.param_count; ++i) p.set_param(i, values[i]);
    return true;
}

}  // namespace sonora::native
