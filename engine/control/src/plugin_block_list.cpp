// SPDX-License-Identifier: GPL-3.0-or-later
// The operator's plug-in block list (plugin_block_list.h). The scan
// worker's thread only.
#include "sonora/plugin_block_list.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include "sonora/json/json.h"
#include "sonora/plugin_catalog_cache.h"

namespace sonora {

namespace fs = std::filesystem;

BlockListLoad load_plugin_block_list(const std::string& path, PluginBlockList& out, std::string& why) {
    out = PluginBlockList{};
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) {
        if (!fs::exists(path, ec)) return BlockListLoad::Missing;
        why = "the plug-in block list " + path + " cannot be read";
        return BlockListLoad::Unreadable;
    }
    if (size > kMaxPluginBlockListBytes) {
        why = "the plug-in block list " + path + " is " + std::to_string(size) + " bytes, over its " +
              std::to_string(kMaxPluginBlockListBytes >> 20) + " MiB cap";
        return BlockListLoad::Unreadable;
    }
    std::string text;
    {
        std::ifstream f(path, std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        if (!f) {
            why = "the plug-in block list " + path + " cannot be read";
            return BlockListLoad::Unreadable;
        }
        text = ss.str();
    }
    json::Value doc;
    json::Error je;
    json::Limits limits;
    limits.max_input_bytes = kMaxPluginBlockListBytes;
    if (!json::parse(text, doc, je, limits) || !doc.is_object()) {
        why = "the plug-in block list " + path + " is not readable JSON" +
              (je.message.empty() ? "" : " (" + je.message + ", line " + std::to_string(je.line) + ")");
        return BlockListLoad::Unreadable;
    }
    const json::Value* version = doc.find("version");
    int64_t v = 0;
    if (!version || !version->is_number() || !version->to_int64(v) || v != int64_t(kPluginBlockListVersion)) {
        why = "the plug-in block list " + path + " is of another version";
        return BlockListLoad::Unreadable;
    }
    const json::Value* blocked = doc.find("blocked");
    if (!blocked || !blocked->is_array() || blocked->items().size() > kMaxBlockedPlugins) {
        why = "the plug-in block list " + path + " has no list of at most " + std::to_string(kMaxBlockedPlugins) +
              " plug-ins";
        return BlockListLoad::Unreadable;
    }
    // All or nothing: an entry half read would run a plug-in the operator
    // blocked.
    PluginBlockList list;
    for (const json::Value& e : blocked->items()) {
        const json::Value* id = e.is_object() ? e.find("id") : nullptr;
        const json::Value* name = e.is_object() ? e.find("name") : nullptr;
        if (!id || !id->is_string() || id->text().empty() || (name && !name->is_string())) {
            why = "the plug-in block list " + path + " has a malformed entry (line " + std::to_string(e.line()) + ")";
            return BlockListLoad::Unreadable;
        }
        std::string label = name ? name->text() : std::string();
        if (label.size() > kMaxBlockedNameBytes) {
            label.resize(kMaxBlockedNameBytes);
            // Cut back to a character boundary, so the label stays UTF-8.
            while (!label.empty() && !json::valid_utf8(label)) label.pop_back();
        }
        list.plugins[id->text()] = std::move(label);
    }
    out = std::move(list);
    return BlockListLoad::Loaded;
}

std::string encode_plugin_block_list(const PluginBlockList& list) {
    json::Value doc = json::Value::object();
    doc.set("version", json::Value::integer(kPluginBlockListVersion));
    json::Value& blocked = doc.set("blocked", json::Value::array());
    for (const auto& [id, name] : list.plugins) {
        // The reader is strict UTF-8: an id that is not could never be read
        // back, and would make the whole file unreadable. A name that is
        // not is only a label, so it is dropped.
        if (!json::valid_utf8(id)) continue;
        json::Value e = json::Value::object();
        e.set("id", json::Value::string(id));
        e.set("name", json::Value::string(json::valid_utf8(name) ? name : std::string()));
        blocked.push(std::move(e));
    }
    std::string out, error;
    json::write(doc, out, error);
    return out;
}

bool save_plugin_block_list(const std::string& path, const PluginBlockList& list, std::string& why) {
    return write_file_durably(path, encode_plugin_block_list(list), why);
}

}  // namespace sonora
