// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Smart Controls core's ParamSink (ADR-0117 §3) over the insert
// controller's public gesture: begin, set (a PluginParameterChange each),
// end (one SetInsertParams). A macro move is then the same gesture a
// parameter drag in the plug-in panel is, so the engine, undo and the audio
// thread see nothing new.
#pragma once

#include <string>

#include "insert_controller.h"
#include "sonora/smart_controls/smart_controls.h"

namespace sonora::ui {

class InsertParamSink final : public smart_controls::ParamSink {
public:
    explicit InsertParamSink(InsertController& ctl) : ctl_(ctl) {}

    // The insert is in the document and its plugin's catalog entry names
    // the key: anything less and the mapping is unresolved.
    bool has_param(std::uint64_t insert, const std::string& key) const override;
    void begin(std::uint64_t insert) override;
    void set(std::uint64_t insert, const std::string& key, float value) override;
    void end(std::uint64_t insert) override;

    // The first refusal since clear(), in the controller's words; empty
    // when every step went through.
    const std::string& error() const { return error_; }
    void clear() { error_.clear(); }

private:
    void note(bool ok, const std::string& why);

    InsertController& ctl_;
    std::string error_;
};

// The catalog's description of `key` on `insert`'s plugin, or null.
const api::PluginParam* find_param(const InsertController& ctl, model::Id insert,
                                   const std::string& key);

}  // namespace sonora::ui
