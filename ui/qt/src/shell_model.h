// SPDX-License-Identifier: GPL-3.0-or-later
//
// The shell's view of the engine, and its one clock.
//
// ShellModel owns the single 60 Hz GUI-thread timer ADR-0006 requires. Each
// tick pulls: replies and events from the engine, one peak per attached
// meter, and -- at a quarter of the rate -- one frame of the state block,
// the transport and every track's flags from the same publish. Nothing pushes
// into it; the engine is another process (ADR-0007 §3) and the only way in
// is EngineAdapter, the seam M4 replaces.
//
// Text properties are re-published only when their value changes, and the
// meters get no properties at all: a meter update is a C++ call plus a
// scheduled redraw, never a signal (ADR-0006).
#pragma once

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <functional>
#include <vector>

#include "engine_adapter.h"
#include "key_router.h"
#include "keymap_profiles.h"

class QQuickWindow;

namespace sonora::ui {

class MeterItem;

// Per-tick cost, for --measure. Pre-sized, written on the GUI thread only.
struct TickStats {
    std::vector<int64_t> tick_ns;
    std::vector<int64_t> interval_ns;
};

class ShellModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(int trackCount READ trackCount NOTIFY engineFormatChanged)
    Q_PROPERTY(QString formatText READ formatText NOTIFY engineFormatChanged)
    Q_PROPERTY(QString profileName READ profileName NOTIFY keymapChanged)
    // The profiles the operator can switch to: {name, file, error, current}
    // each, `error` naming why a file is refused (keymap_profiles.h).
    Q_PROPERTY(QVariantList profiles READ profiles NOTIFY keymapChanged)
    // Pro Tools' Commands Keyboard Focus, and whether this profile has any
    // key it gates (the a-z button is shown only then).
    Q_PROPERTY(bool commandsFocus READ commandsFocus NOTIFY keymapChanged)
    Q_PROPERTY(bool focusKeys READ focusKeys NOTIFY keymapChanged)
    Q_PROPERTY(QString transportText READ transportText NOTIFY transportChanged)
    Q_PROPERTY(bool recording READ recording NOTIFY transportChanged)
    Q_PROPERTY(QString clockText READ clockText NOTIFY transportChanged)
    Q_PROPERTY(int take READ take NOTIFY transportChanged)
    Q_PROPERTY(int xruns READ xruns NOTIFY transportChanged)
    Q_PROPERTY(int overruns READ overruns NOTIFY transportChanged)
    Q_PROPERTY(bool stale READ stale NOTIFY transportChanged)
    Q_PROPERTY(QString healthText READ healthText NOTIFY transportChanged)
    Q_PROPERTY(QList<bool> armed READ armed NOTIFY tracksChanged)
    Q_PROPERTY(bool engineAlive READ engineAlive NOTIFY engineAliveChanged)
    // While the engine is not alive, why the link is down if the backend
    // knows (EngineAdapter::link_error), capitalised: the version refusal
    // among others.
    Q_PROPERTY(QString linkErrorText READ linkErrorText NOTIFY engineAliveChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(QStringList notices READ notices NOTIFY noticesChanged)
    // What the operator must act on, in red: why the backend stopped, why
    // the take stopped writing, why no input is connected. Empty when
    // nothing is wrong.
    Q_PROPERTY(QString alertText READ alertText NOTIFY alertChanged)
    // Takes on disk the open session does not name, as the engine offers
    // them (api::EventKind::RecoveryOffer): one line per take, saying where
    // it would go. Empty when none is offered. Held until the operator
    // places or leaves them; a refusal of either stays under it.
    Q_PROPERTY(QStringList recoveryOffer READ recoveryOffer NOTIFY recoveryChanged)
    Q_PROPERTY(int recoveryCount READ recoveryCount NOTIFY recoveryChanged)
    Q_PROPERTY(QString recoveryRefusal READ recoveryRefusal NOTIFY recoveryChanged)
    Q_PROPERTY(QStringList helpEntries READ helpEntries NOTIFY keymapChanged)
    Q_PROPERTY(QString renderPath READ renderPath NOTIFY renderPathChanged)
    Q_PROPERTY(bool softwareRendering READ softwareRendering NOTIFY renderPathChanged)
    Q_PROPERTY(bool metersAvailable READ metersAvailable NOTIFY renderPathChanged)
    Q_PROPERTY(int meterHz READ meterHz NOTIFY renderPathChanged)

public:
    ShellModel(EngineAdapter& engine, KeyRouter& router, QObject* parent = nullptr);

    // Starts the tick. `hz` is the meter rate; ADR-0006 allows lowering it
    // to protect the callback, never raising the cost of a frame.
    void start(int hz);
    void setMeterHz(int hz);
    // Keys typed into `window` go through the router, all of them: the
    // profile, not Qt's focus chain, decides what Space and Tab do.
    void routeKeysFrom(QQuickWindow* window);
    // Whether key events carry XKB keycodes (qt_keys.h). Read from the
    // platform; the offscreen platform has no keyboard, so a test that sends
    // XKB keycodes says so here.
    void setScanCodesAreXkb(bool xkb) { xkb_scan_codes_ = xkb; }
    bool scanCodesAreXkb() const { return xkb_scan_codes_; }
    void addNotice(const QString& n);
    // Why the last File > Open did not happen. Held, since the operator
    // asked for a session and may look away before a status line clears;
    // replaced by the next refusal and taken away by the next Open (empty).
    void setOpenNotice(const QString& n);
    // The key map's own notices -- a refused profile, the essentials
    // repaired -- which a profile switch replaces rather than piles up. The
    // essentials' are kept apart: a new engine's Hello replaces those alone.
    void setKeymapNotices(const QStringList& profile, const QStringList& essentials = {});

    // The profile switch. `remember` records a choice for the next start
    // and says why it could not; empty, nothing is remembered.
    using Remember = std::function<bool(const std::string& file, std::string& why)>;
    void setProfiles(std::vector<ProfileEntry> entries, std::string current, Remember remember);
    // From the render-path probe, on the GUI thread.
    void setRenderPath(const QString& text, bool software, bool meters_available);

    void attachMeter(MeterItem* m);
    void detachMeter(MeterItem* m);
    int attachedMeters() const { return static_cast<int>(meters_.size()); }
    // For --no-meters, the baseline a measurement subtracts.
    void hideMeters();

    // The clock engine_alive() is asked against: monotonic nanoseconds, the
    // heartbeat's clock. Tests substitute their own.
    using Clock = std::function<uint64_t()>;
    void setClock(Clock c) { now_ns_ = std::move(c); }

    // Called every tick with the engine's transport, after the meters: the
    // Edit window's playhead. A C++ call, never a signal (ADR-0006).
    using TransportSink = std::function<void(const TransportSnapshot&, bool alive)>;
    void setTransportSink(TransportSink s) { transport_sink_ = std::move(s); }

    // The insert slots' view of the engine (ADR-0012 §8), each a C++ call
    // from the tick: a new plugin catalog when one arrives; a PluginFault,
    // by insert id, empty text clearing it; and the state block's frame at
    // the state rate (a quarter of the ticks), for insert flags and the
    // delay compensation. Faults go to the slot, not to the notice list.
    struct PluginSinks {
        std::function<void(const api::PluginCatalog&)> catalog;
        std::function<void(uint64_t insert, const std::string& text)> fault;
        std::function<void(const EngineFrame&, bool alive)> frame;
    };
    void setPluginSinks(PluginSinks s) { plugin_sinks_ = std::move(s); }

    // Measurement hooks; null when not measuring.
    void setTickStats(TickStats* s) { stats_ = s; }

    int trackCount() const { return static_cast<int>(engine_.info().track_count); }
    QString formatText() const;
    QString profileName() const;
    QString transportText() const { return transport_text_; }
    bool recording() const { return recording_; }
    QString clockText() const { return clock_text_; }
    int take() const { return take_; }
    int xruns() const { return xruns_; }
    int overruns() const { return overruns_; }
    bool stale() const { return stale_; }
    QString healthText() const { return health_text_; }
    QList<bool> armed() const { return armed_; }
    bool engineAlive() const { return alive_; }
    QString linkErrorText() const { return link_error_; }
    QString statusText() const { return status_; }
    QStringList notices() const { return notices_; }
    QString alertText() const { return alert_text_; }
    QStringList recoveryOffer() const { return recovery_offer_; }
    int recoveryCount() const { return recovery_count_; }
    QString recoveryRefusal() const { return recovery_refusal_; }
    // What a RecoveryOffer event does: the screenshot scene's way in, as
    // the demo's engine has no takes on disk.
    void showRecoveryOffer(const QString& text, int count);
    QStringList helpEntries() const;
    QVariantList profiles() const;
    bool commandsFocus() const { return router_.commandsFocus(); }
    bool focusKeys() const { return router_.hasFocusKeys(); }

    // Switches to the profile in `file` ("" for the built-in default), as
    // listed in `profiles`. A file that no longer loads is refused, loudly,
    // and the keys stay as they were: the operator is never left on a
    // half-loaded map, nor moved to the default behind their back.
    Q_INVOKABLE void useProfile(const QString& file);
    QString renderPath() const { return render_path_; }
    bool softwareRendering() const { return software_; }
    bool metersAvailable() const { return meters_available_; }
    int meterHz() const { return hz_; }

    // The same path as a key, for a click: the arm button sends exactly what
    // the profile's arm key would.
    Q_INVOKABLE void invoke(const QString& command, int target);
    // The same with the value a dialog asked for (Repeat's count). True when
    // it ran, so the dialog closes; on a refusal the reason is on the status
    // line and the dialog stays open to correct it.
    Q_INVOKABLE bool invokeWith(const QString& command, int target, const QString& argument);
    // Whether this build can run `command`, for drawing a control as
    // unavailable instead of letting it look live.
    Q_INVOKABLE bool implemented(const QString& command) const {
        return router_.implemented(command.toStdString());
    }
    // A line for the status line, from anything that is not a key: a
    // refused mouse gesture, say.
    Q_INVOKABLE void say(const QString& message);
    // While a text field has the keyboard (the plugin chooser's search),
    // keys go to it and not to the profile: typing "s" there must not
    // solo a track. The field sets this on focus and clears it on leaving,
    // and must leave on Escape and Return, so the profile is never locked out.
    Q_INVOKABLE void setTextEntry(bool on) { text_entry_ = on; }
    bool textEntry() const { return text_entry_; }

    // Where `db` sits on a meter of height `h`, in the meter's coordinates,
    // so the scale labels beside the meters use the meters' own mapping.
    Q_INVOKABLE qreal scaleY(qreal db, qreal h) const;
    Q_INVOKABLE QList<qreal> scaleMarks() const;

    // Runs one tick now. The timer calls this; tests call it directly.
    void tick();

signals:
    // The engine's SessionFolder (ADR-0018 §4): where the open session's
    // takes are written, an absolute path; `untitled`: kFolderUntitled.
    void sessionFolder(const QString& path, bool untitled);
    // A new engine's Hello with another sample rate, buffer size or track
    // count: trackCount and formatText changed, and what was sized from them
    // here (the meters' peaks, the arm buttons, the clock) already follows.
    void engineFormatChanged();
    void transportChanged();
    void tracksChanged();
    void engineAliveChanged();
    void statusChanged();
    void noticesChanged();
    void alertChanged();
    void recoveryChanged();
    void renderPathChanged();
    void keymapChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void report(const RouteResult& r);
    // `read_frame` false: tick() has just read frame_.
    void refreshState(bool read_frame = true);
    void takeEvents();
    // A new Hello with another command list: the router follows it.
    void followEngineCommands();
    // A new Hello with another format or track count: resize and say so.
    void followEngineFormat();
    // The format and track count sized for; a reconnect compares against it.
    uint32_t sized_rate_ = 0;
    uint32_t sized_buffer_ = 0;
    uint32_t sized_tracks_ = 0;

    EngineAdapter& engine_;
    KeyRouter& router_;
    QTimer timer_;
    QElapsedTimer clock_;
    int64_t last_tick_ns_ = -1;
    int hz_ = 60;
    unsigned tick_count_ = 0;
    std::vector<MeterItem*> meters_;
    // One peak per channel per tick, however many meters show the channel:
    // take_peak() moves this reader's cursor, so a second read in the same
    // tick would see nothing and draw a meter that lies.
    std::vector<float> peaks_;
    std::vector<bool> peak_read_;
    TransportSink transport_sink_;
    PluginSinks plugin_sinks_;
    api::PluginCatalog catalog_;   // kept, so taking one reuses its storage
    std::vector<CommandOutcome> replies_;
    std::vector<EngineEvent> events_;
    EngineFrame frame_;
    TickStats* stats_ = nullptr;
    Clock now_ns_;
    bool xkb_scan_codes_ = false;
    bool text_entry_ = false;
    bool seen_focus_ = true;
    QStringList keymap_notices_;      // profile_notices_, then essentials_notices_
    QStringList profile_notices_;     // a refused profile, a choice not remembered
    QStringList essentials_notices_;  // ensure_essentials(): redone on a new Hello
    std::vector<ProfileEntry> profiles_;
    std::string profile_file_;
    Remember remember_;

    QString transport_text_ = QStringLiteral("STOP");
    bool recording_ = false;
    QString clock_text_ = QStringLiteral("00:00:00.000");
    int take_ = 0;
    int xruns_ = 0;
    int overruns_ = 0;
    bool stale_ = false;
    QString health_text_;
    QList<bool> armed_;
    bool alive_ = false;
    QString link_error_;
    QString status_;
    int64_t status_until_ns_ = 0;
    QStringList notices_;
    QString open_notice_;
    QString fatal_;
    QString take_failed_;
    QString inputs_missing_;
    QString alert_text_;
    QStringList recovery_offer_;
    int recovery_count_ = 0;
    QString recovery_refusal_;
    QString render_path_ = QStringLiteral("not yet known");
    bool software_ = false;
    bool meters_available_ = true;
};

}  // namespace sonora::ui
