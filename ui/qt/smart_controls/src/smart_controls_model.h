// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Smart Controls panel's model (ADR-0117): eight named macro knobs, each
// fanning out to insert parameters through InsertParamSink, the mapping list
// of the selected macro, learn mode, and the session's sidecar file.
//
// What it shows is what is true:
//   - a knob reads back from its first resolved mapping's parameter, so a
//     plug-in panel drag or a session reload moves the knob too;
//   - a mapping whose insert or parameter is not there is listed, flagged
//     unresolved and counted on its knob, never dropped or hidden;
//   - a refusal (a corrupt file, a refused value, a duplicate mapping) is a
//     held notice the operator dismisses, not a status flash.
//
// Learn (Logic's workflow, reproduced as behaviour under ADR-0004): arm a
// macro, touch a parameter in any plug-in panel; that parameter becomes the
// macro's mapping, its range from the value before the touch to where the
// touch left it. A click with no travel maps the parameter's whole range.
// The touch is seen through learnTap(), a ParamSender that wraps the one the
// engine already gets, so nothing else in the interface has to know.
#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <array>
#include <memory>
#include <string>

#include "insert_controller.h"
#include "insert_param_sink.h"
#include "sonora/smart_controls/smart_controls.h"

namespace sonora::ui {

class SmartControlsModel : public QObject {
    Q_OBJECT
    // Eight entries: name, value (0..1), mappings, unresolved (count).
    Q_PROPERTY(QVariantList macros READ macros NOTIFY changed)
    Q_PROPERTY(int selected READ selected WRITE select NOTIFY changed)
    // The selected macro's mappings: label, plugin, param, loPos, hiPos
    // (0..1 along the parameter), loText, hiText, invert, unresolved,
    // capturing (being learned now).
    Q_PROPERTY(QVariantList mappings READ mappings NOTIFY changed)
    // The macro armed for learn, -1 when none.
    Q_PROPERTY(int learning READ learning NOTIFY changed)
    Q_PROPERTY(QString notice READ notice NOTIFY changed)

public:
    static constexpr const char* kFileName = "smart_controls.json";

    explicit SmartControlsModel(InsertController& ctl, QObject* parent = nullptr);
    ~SmartControlsModel() override;

    // A session opened: every per-session thing resets (values, learn, the
    // selection, the notice) and the set is loaded from `store`. A store
    // that cannot be read is reported, held, and never written over.
    // `exists` false: the session has no Smart Controls yet, so nothing is
    // loaded and the eight default macros are offered.
    void open(std::unique_ptr<smart_controls::SmartControlsStore> store, bool exists = true);
    // open() with the JSON file in the session's folder.
    Q_INVOKABLE void setSessionFolder(const QString& folder);
    // A ParamSender that forwards to `downstream` and feeds learn.
    InsertController::ParamSender learnTap(InsertController::ParamSender downstream);
    // A parameter moved by the operator (what learnTap sees).
    void touched(model::Id insert, const std::string& key, float value);

    const smart_controls::SmartControlSet& set() const { return set_; }
    // Adds a mapping by hand (and the scenes and tests); saved.
    bool addMapping(int macro, const smart_controls::Mapping& m, std::string& why);

    QVariantList macros() const;
    QVariantList mappings() const;
    int selected() const { return selected_; }
    int learning() const { return learning_; }
    QString notice() const { return notice_; }

    Q_INVOKABLE void select(int macro);
    Q_INVOKABLE void setMacroValue(int macro, double value);
    // Arms learn on `macro`; on the armed one, finishes it.
    Q_INVOKABLE void learn(int macro);
    Q_INVOKABLE void finishLearn();
    Q_INVOKABLE void renameMacro(int macro, const QString& name);
    // On the selected macro's mapping `mapping`.
    Q_INVOKABLE void setInvert(int mapping, bool invert);
    Q_INVOKABLE void setRangePosition(int mapping, double lo, double hi);
    Q_INVOKABLE void removeMapping(int mapping);
    Q_INVOKABLE void dismissNotice();

signals:
    void changed();

private:
    void reset_session();
    void pad_macros();
    void save();
    void refuse(const std::string& why);
    float macro_value(int macro) const;
    float document_value(model::Id insert, const api::PluginParam& p) const;
    smart_controls::Mapping* selected_mapping(int mapping);
    QVariantMap mapping_view(const smart_controls::Mapping& m, bool capturing) const;

    InsertController& ctl_;
    InsertParamSink sink_;
    std::unique_ptr<smart_controls::SmartControlsStore> store_;
    bool store_broken_ = false;
    smart_controls::SmartControlSet set_;
    std::array<float, smart_controls::kMaxMacros> values_{};
    int selected_ = 0;
    int learning_ = -1;
    // The mapping being learned, while its touch runs.
    bool capturing_ = false;
    model::Id capture_insert_ = 0;
    std::string capture_key_;
    // A macro move's own parameter changes are not a touch.
    bool applying_ = false;
    QString notice_;
};

}  // namespace sonora::ui
