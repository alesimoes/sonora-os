// SPDX-License-Identifier: GPL-3.0-or-later
//
// The session, the knobs and the store. Learn and the mapping edits are in
// smart_controls_learn.cpp.
#include "smart_controls_model.h"

#include <QDir>
#include <QFileInfo>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <set>

namespace sonora::ui {

namespace sc = smart_controls;

SmartControlsModel::SmartControlsModel(InsertController& ctl, QObject* parent)
    : QObject(parent), ctl_(ctl), sink_(ctl) {
    pad_macros();
}

SmartControlsModel::~SmartControlsModel() = default;

void SmartControlsModel::reset_session() {
    set_ = sc::SmartControlSet{};
    values_.fill(0.0f);
    selected_ = 0;
    learning_ = -1;
    capturing_ = false;
    capture_insert_ = 0;
    capture_key_.clear();
    store_broken_ = false;
    notice_.clear();
}

void SmartControlsModel::open(std::unique_ptr<sc::SmartControlsStore> store, bool exists) {
    reset_session();
    store_ = std::move(store);
    if (store_ != nullptr && exists) {
        sc::SmartControlSet loaded;
        std::string why;
        if (store_->load(loaded, why)) {
            set_ = std::move(loaded);
        } else {
            // ADR-0117 §5: the file stays as it is, so a hand fix or another
            // version of the application still finds what was there.
            store_broken_ = true;
            notice_ = QStringLiteral("Smart Controls could not be read: %1. The macros below are "
                                     "empty and nothing is saved over the file.")
                          .arg(QString::fromStdString(why));
        }
    }
    pad_macros();
    emit changed();
}

void SmartControlsModel::setSessionFolder(const QString& folder) {
    if (folder.isEmpty()) {
        open(nullptr, false);
        return;
    }
    const QString path = QDir(folder).filePath(QString::fromLatin1(kFileName));
    open(std::make_unique<sc::JsonFileStore>(path.toStdString()), QFileInfo::exists(path));
}

// Always eight knobs on the panel; a file with fewer keeps its own and the
// rest are offered under names it does not use.
void SmartControlsModel::pad_macros() {
    std::set<std::string> used;
    for (const sc::Macro& m : set_.macros) used.insert(m.name);
    for (int n = 1; set_.macros.size() < sc::kMaxMacros; ++n) {
        std::string name = "Macro " + std::to_string(n);
        if (used.insert(name).second) set_.macros.push_back(sc::Macro{name, {}});
    }
}

void SmartControlsModel::save() {
    if (store_ == nullptr || store_broken_) return;
    std::string why;
    if (!store_->save(set_, why)) refuse("Smart Controls were not saved: " + why);
}

void SmartControlsModel::refuse(const std::string& why) {
    notice_ = QString::fromStdString(why);
    emit changed();
}

void SmartControlsModel::dismissNotice() {
    if (notice_.isEmpty()) return;
    notice_.clear();
    emit changed();
}

float SmartControlsModel::document_value(model::Id insert, const api::PluginParam& p) const {
    if (const model::InsertMix* m = ctl_.snapshot().mix.find_insert(insert)) {
        for (const model::ParamValue& v : m->params) {
            if (v.key == p.key) return v.value;
        }
    }
    return p.default_value;
}

// Read back from the first mapping that can say, so the knob follows the
// parameter wherever it was moved from.
float SmartControlsModel::macro_value(int macro) const {
    const sc::Macro& m = set_.macros[static_cast<std::size_t>(macro)];
    for (const sc::Mapping& p : m.mappings) {
        const api::PluginParam* param = find_param(ctl_, p.insert, p.key);
        if (param == nullptr || p.hi == p.lo) continue;
        float t = (ctl_.value(p.insert, *param) - p.lo) / (p.hi - p.lo);
        if (p.invert) t = 1.0f - t;
        return std::clamp(t, 0.0f, 1.0f);
    }
    return values_[static_cast<std::size_t>(macro)];
}

void SmartControlsModel::select(int macro) {
    if (macro < 0 || macro >= static_cast<int>(set_.macros.size()) || macro == selected_) return;
    selected_ = macro;
    emit changed();
}

void SmartControlsModel::setMacroValue(int macro, double value) {
    if (macro < 0 || macro >= static_cast<int>(set_.macros.size())) return;
    const float v = static_cast<float>(std::clamp(value, 0.0, 1.0));
    values_[static_cast<std::size_t>(macro)] = v;
    sc::ApplyResult result;
    std::string why;
    sink_.clear();
    applying_ = true;
    const bool ok = sc::apply(set_, static_cast<std::size_t>(macro), v, sink_, result, why);
    applying_ = false;
    if (!ok) refuse(why);
    else if (!sink_.error().empty()) refuse(sink_.error());
    emit changed();
}

void SmartControlsModel::renameMacro(int macro, const QString& name) {
    if (macro < 0 || macro >= static_cast<int>(set_.macros.size())) return;
    const std::string before = set_.macros[static_cast<std::size_t>(macro)].name;
    set_.macros[static_cast<std::size_t>(macro)].name = name.trimmed().toStdString();
    const std::string why = sc::validate(set_);
    if (!why.empty()) {
        set_.macros[static_cast<std::size_t>(macro)].name = before;
        refuse("The macro keeps its name: " + why);
        return;
    }
    save();
    emit changed();
}

QVariantList SmartControlsModel::macros() const {
    QVariantList out;
    for (std::size_t i = 0; i < set_.macros.size(); ++i) {
        const sc::Macro& m = set_.macros[i];
        int unresolved = 0;
        for (const sc::Mapping& p : m.mappings) {
            if (!sink_.has_param(p.insert, p.key)) ++unresolved;
        }
        QVariantMap row;
        row[QStringLiteral("name")] = QString::fromStdString(m.name);
        row[QStringLiteral("value")] = macro_value(static_cast<int>(i));
        row[QStringLiteral("mappings")] = static_cast<int>(m.mappings.size());
        row[QStringLiteral("unresolved")] = unresolved;
        out.push_back(row);
    }
    return out;
}

}  // namespace sonora::ui
