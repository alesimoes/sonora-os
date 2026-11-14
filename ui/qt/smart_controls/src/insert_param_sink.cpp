// SPDX-License-Identifier: GPL-3.0-or-later
#include "insert_param_sink.h"

namespace sonora::ui {

const api::PluginParam* find_param(const InsertController& ctl, model::Id insert,
                                   const std::string& key) {
    const api::PluginInfo* plugin = ctl.plugin_of(insert);
    if (plugin == nullptr) return nullptr;
    for (const api::PluginParam& p : plugin->params) {
        if (p.key == key) return &p;
    }
    return nullptr;
}

bool InsertParamSink::has_param(std::uint64_t insert, const std::string& key) const {
    return find_param(ctl_, insert, key) != nullptr;
}

void InsertParamSink::note(bool ok, const std::string& why) {
    if (!ok && error_.empty()) error_ = why.empty() ? "the insert refused the change" : why;
}

void InsertParamSink::begin(std::uint64_t insert) {
    std::string why;
    note(ctl_.param_begin(insert, why), why);
}

void InsertParamSink::set(std::uint64_t insert, const std::string& key, float value) {
    std::string why;
    note(ctl_.param_set(insert, key, value, why), why);
}

void InsertParamSink::end(std::uint64_t) {
    std::string why;
    note(ctl_.param_end(why), why);
}

}  // namespace sonora::ui
