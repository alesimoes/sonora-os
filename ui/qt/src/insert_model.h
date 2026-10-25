// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Mix window's insert slots for QML: InsertController behind a QObject.
// QML names a slot by display track and slot (0 for A); an insert id never
// reaches QML, so a panel left open follows its insert across a move.
//
// Fed by ShellModel's one tick (catalog, faults, the state block's insert
// flags, through ShellModel::PluginSinks) and by SessionModel's change
// signals; every edit goes through the TimelineDocument and reports
// through SessionModel, so a refused insert edit reaches the status line
// the way a refused region edit does.
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <algorithm>

#include "insert_controller.h"

namespace sonora::ui {

class SessionModel;

class InsertModel : public QObject {
    Q_OBJECT
    // [{latency, latencyText, slots: [10 x {present, letter, name, active,
    // bypass, faulted, missing, restarting, running, fault}]}], one per
    // display track.
    Q_PROPERTY(QVariantList strips READ strips NOTIFY changed)
    Q_PROPERTY(bool hasCatalog READ hasCatalog NOTIFY changed)
    Q_PROPERTY(QStringList categories READ categories NOTIFY changed)
    Q_PROPERTY(QString compensationText READ compensationText NOTIFY changed)
    // The compensation is clamped at its maximum: the inserts need more.
    Q_PROPERTY(bool compensationShort READ compensationShort NOTIFY changed)
    Q_PROPERTY(bool pluginsDisabled READ pluginsDisabled NOTIFY changed)

    // The plugin chooser.
    Q_PROPERTY(bool browserOpen READ browserOpen NOTIFY browserChanged)
    Q_PROPERTY(QString browserTitle READ browserTitle NOTIFY browserChanged)
    Q_PROPERTY(int browserTrack READ browserTrack NOTIFY browserChanged)

    // The generic parameter panel.
    Q_PROPERTY(bool panelOpen READ panelOpen NOTIFY panelChanged)
    Q_PROPERTY(int panelTrack READ panelTrack NOTIFY panelChanged)
    Q_PROPERTY(int panelSlot READ panelSlot NOTIFY panelChanged)
    Q_PROPERTY(QString panelTitle READ panelTitle NOTIFY panelChanged)
    Q_PROPERTY(QString panelSubtitle READ panelSubtitle NOTIFY panelChanged)
    Q_PROPERTY(QVariantMap panelSlotInfo READ panelSlotInfo NOTIFY panelChanged)
    // [{key, name, steps, labels}]: what the controls are. Apart from the
    // values, so a slider being dragged is not rebuilt under the hand.
    Q_PROPERTY(QVariantList panelParams READ panelParams NOTIFY panelChanged)
    // {key: {text, position, step}}: where they stand.
    Q_PROPERTY(QVariantMap panelValues READ panelValues NOTIFY panelValuesChanged)
    Q_PROPERTY(QString panelNote READ panelNote NOTIFY panelChanged)

public:
    InsertModel(TimelineDocument& doc, SessionModel& session, QObject* parent = nullptr);

    InsertController& controller() { return ctl_; }

    // From ShellModel's tick.
    void onCatalog(const api::PluginCatalog& c);
    void onFault(uint64_t insert, const std::string& text);
    void onFrame(const EngineFrame& f, bool alive);
    // A new connection to the engine: its replay of standing faults follows.
    void onLinkReplaced();

    QVariantList strips() const { return strips_; }
    bool hasCatalog() const { return ctl_.has_catalog(); }
    QStringList categories() const;
    QString compensationText() const;
    bool compensationShort() const { return ctl_.compensation_shortfall() > 0; }
    bool pluginsDisabled() const { return ctl_.plugins_disabled(); }

    bool browserOpen() const { return browser_track_ >= 0; }
    QString browserTitle() const;
    int browserTrack() const { return browser_track_; }

    bool panelOpen() const { return panel_insert_ != kNoId; }
    int panelTrack() const { return panel_track_; }
    int panelSlot() const { return panel_slot_; }
    QString panelTitle() const { return panel_title_; }
    QString panelSubtitle() const { return panel_subtitle_; }
    QVariantMap panelSlotInfo() const { return panel_slot_info_; }
    QVariantList panelParams() const { return panel_params_; }
    QVariantMap panelValues() const { return panel_values_; }
    QString panelNote() const { return panel_note_; }

    // A click on a slot, with the mouse's modifiers: an empty slot opens the
    // chooser; Ctrl-click bypasses, Ctrl+Start-click makes it inactive or
    // active, a plain click opens the parameter panel.
    Q_INVOKABLE void slotClicked(int track, int slot, int modifiers);
    // A drag let go on another slot: moves it, or copies it with `copy`
    // (Alt held at the drop).
    Q_INVOKABLE void dropInsert(int fromTrack, int fromSlot, int toTrack, int toSlot, bool copy);
    Q_INVOKABLE void toggleBypass(int track, int slot);
    Q_INVOKABLE void setActive(int track, int slot, bool active);
    Q_INVOKABLE void remove(int track, int slot);

    // [{index, name, vendor, format, channels, host, category, refusal}]
    // for the chooser's track; `host` is "Windows (Wine)" for a plug-in
    // bridged by yabridge, empty for a native one.
    Q_INVOKABLE QVariantList browse(const QString& category, const QString& search) const;
    // Opens the chooser on a slot; on a slot that holds an insert it
    // replaces that insert.
    Q_INVOKABLE void openBrowser(int track, int slot);
    Q_INVOKABLE void closeBrowser();
    // Puts catalog plugin `index` in the chooser's slot. False (and the
    // reason on the status line) when it cannot go there.
    Q_INVOKABLE bool choose(int index);

    Q_INVOKABLE void openPanel(int track, int slot);
    Q_INVOKABLE void closePanel();
    // The panel's controls: a slider gesture, a step chosen, the default.
    Q_INVOKABLE void paramBegin();
    Q_INVOKABLE void paramPosition(const QString& key, qreal position);
    Q_INVOKABLE void paramEnd();
    Q_INVOKABLE void paramStep(const QString& key, int step);
    Q_INVOKABLE void paramReset(const QString& key);

    Q_INVOKABLE QString latencyText(qreal frames) const;
    // short_plugin_name(), for a slot `chars` characters wide.
    Q_INVOKABLE QString shortName(const QString& name, int chars) const {
        return QString::fromStdString(
            short_plugin_name(name.toStdString(), static_cast<std::size_t>(std::max(1, chars))));
    }

signals:
    void changed();
    void browserChanged();
    void panelChanged();
    void panelValuesChanged();

private:
    void refresh();
    void refreshPanel();
    bool report(bool ok, const std::string& why);
    // Finds the panel's insert again after the document changed; closes the
    // panel if it is gone.
    void locatePanel();

    InsertController ctl_;
    SessionModel& session_;
    QVariantList strips_;
    QVariantMap summary_;   // the other properties, as last published
    int browser_track_ = -1;
    int browser_slot_ = -1;
    bool browser_replaces_ = false;
    model::Id panel_insert_ = kNoId;
    int panel_track_ = -1;
    int panel_slot_ = -1;
    QString panel_title_, panel_subtitle_, panel_note_;
    QVariantMap panel_slot_info_;
    QVariantList panel_params_;
    QVariantMap panel_values_;
};

}  // namespace sonora::ui
