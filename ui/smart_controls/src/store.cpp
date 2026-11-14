#include <fstream>
#include <sstream>

#include "sonora/json/json.h"
#include "sonora/smart_controls/smart_controls.h"

namespace sonora::smart_controls {
namespace json = sonora::json;

std::string to_json(const SmartControlSet& set) {
    json::Value root = json::Value::object();
    root.set("schema", json::Value::integer(kSchema));
    json::Value macros = json::Value::array();
    for (const auto& m : set.macros) {
        json::Value jm = json::Value::object();
        jm.set("name", json::Value::string(m.name));
        json::Value maps = json::Value::array();
        for (const auto& p : m.mappings) {
            json::Value jp = json::Value::object();
            jp.set("insert", json::Value::integer(static_cast<std::int64_t>(p.insert)));
            jp.set("key", json::Value::string(p.key));
            jp.set("lo", json::Value::number_float(p.lo));
            jp.set("hi", json::Value::number_float(p.hi));
            jp.set("invert", json::Value::boolean(p.invert));
            maps.push(std::move(jp));
        }
        jm.set("mappings", std::move(maps));
        macros.push(std::move(jm));
    }
    root.set("macros", std::move(macros));
    std::string out, err;
    if (!json::write(root, out, err)) return {};
    return out;
}

static bool read_mapping(const json::Value& jp, Mapping& p) {
    if (!jp.is_object()) return false;
    const json::Value* in = jp.find("insert");
    const json::Value* key = jp.find("key");
    const json::Value* lo = jp.find("lo");
    const json::Value* hi = jp.find("hi");
    const json::Value* inv = jp.find("invert");
    std::int64_t id = 0;
    if (!in || !key || !lo || !hi || !inv || !key->is_string() || !inv->is_bool()) return false;
    if (!in->to_int64(id) || id < 0) return false;
    if (!lo->to_float(p.lo) || !hi->to_float(p.hi)) return false;
    p.insert = static_cast<std::uint64_t>(id);
    p.key = key->text();
    p.invert = inv->as_bool();
    return true;
}

bool from_json(const std::string& text, SmartControlSet& out, std::string& why) {
    json::Value root;
    json::Error e;
    if (!json::parse(text, root, e)) {
        why = "corrupt smart controls file: " + e.message;
        return false;
    }
    const json::Value* sv = root.is_object() ? root.find("schema") : nullptr;
    std::int64_t schema = 0;
    if (!sv || !sv->to_int64(schema)) {
        why = "corrupt smart controls file: no schema";
        return false;
    }
    if (schema != kSchema) {
        why = "smart controls schema " + std::to_string(schema) + " is not supported";
        return false;
    }
    const json::Value* ms = root.find("macros");
    if (!ms || !ms->is_array()) {
        why = "corrupt smart controls file: no macros";
        return false;
    }
    SmartControlSet set;
    for (const auto& jm : ms->items()) {
        const json::Value* name = jm.is_object() ? jm.find("name") : nullptr;
        const json::Value* maps = jm.is_object() ? jm.find("mappings") : nullptr;
        if (!name || !name->is_string() || !maps || !maps->is_array()) {
            why = "corrupt smart controls file: bad macro";
            return false;
        }
        Macro m;
        m.name = name->text();
        for (const auto& jp : maps->items()) {
            Mapping p;
            if (!read_mapping(jp, p)) {
                why = "corrupt smart controls file: bad mapping";
                return false;
            }
            m.mappings.push_back(std::move(p));
        }
        set.macros.push_back(std::move(m));
    }
    why = validate(set);
    if (!why.empty()) {
        why = "invalid smart controls file: " + why;
        return false;
    }
    out = std::move(set);
    return true;
}

bool JsonFileStore::load(SmartControlSet& out, std::string& why) {
    std::ifstream f(path_, std::ios::binary);
    if (!f) {
        why = "cannot open " + path_;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return from_json(ss.str(), out, why);
}

bool JsonFileStore::save(const SmartControlSet& set, std::string& why) {
    why = validate(set);
    if (!why.empty()) return false;
    std::ofstream f(path_, std::ios::binary | std::ios::trunc);
    f << to_json(set);
    if (!f) {
        why = "cannot write " + path_;
        return false;
    }
    return true;
}

bool MemoryStore::load(SmartControlSet& out, std::string& why) {
    if (text.empty()) {
        why = "nothing stored";
        return false;
    }
    return from_json(text, out, why);
}

bool MemoryStore::save(const SmartControlSet& set, std::string& why) {
    why = validate(set);
    if (!why.empty()) return false;
    text = to_json(set);
    return true;
}

}  // namespace sonora::smart_controls
