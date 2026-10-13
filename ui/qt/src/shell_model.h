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
#include <functional>
#include <vector>

#include "engine_adapter.h"
#include "key_router.h"

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
    Q_PROPERTY(int trackCount READ trackCount CONSTANT)
    Q_PROPERTY(QString formatText READ formatText CONSTANT)
    Q_PROPERTY(QString profileName READ profileName CONSTANT)
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
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(QStringList notices READ notices NOTIFY noticesChanged)
    // What the operator must act on, in red: why the backend stopped, why
    // the take stopped writing, why no input is connected. Empty when
    // nothing is wrong.
    Q_PROPERTY(QString alertText READ alertText NOTIFY alertChanged)
    Q_PROPERTY(QStringList helpEntries READ helpEntries CONSTANT)
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
    QString statusText() const { return status_; }
    QStringList notices() const { return notices_; }
    QString alertText() const { return alert_text_; }
    QStringList helpEntries() const;
    QString renderPath() const { return render_path_; }
    bool softwareRendering() const { return software_; }
    bool metersAvailable() const { return meters_available_; }
    int meterHz() const { return hz_; }

    // The same path as a key, for a click: the arm button sends exactly what
    // the profile's arm key would.
    Q_INVOKABLE void invoke(const QString& command, int target);
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
    void transportChanged();
    void tracksChanged();
    void engineAliveChanged();
    void statusChanged();
    void noticesChanged();
    void alertChanged();
    void renderPathChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void report(const RouteResult& r);
    // `read_frame` false: tick() has just read frame_.
    void refreshState(bool read_frame = true);
    void takeEvents();

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
    QString status_;
    int64_t status_until_ns_ = 0;
    QStringList notices_;
    QString fatal_;
    QString take_failed_;
    QString inputs_missing_;
    QString alert_text_;
    QString render_path_ = QStringLiteral("not yet known");
    bool software_ = false;
    bool meters_available_ = true;
};

}  // namespace sonora::ui
