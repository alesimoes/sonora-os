// SPDX-License-Identifier: GPL-3.0-or-later
//
// Click / Count Off at start (ADR-0075): which toggles a preference needs
// given the engine's flags, and the version-1 / version-3 file rules.
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <string>

#include "preferences.h"
#include "sonora/api/state_block.h"
#include "transport_prefs.h"

using namespace sonora;
using namespace sonora::ui;

class TransportPrefsTest : public QObject {
    Q_OBJECT
    static bool write(const QString& path, const char* text) {
        QFile f(path);
        return f.open(QIODevice::WriteOnly) && f.write(text) > 0;
    }
private slots:
    void plan_toggles_only_what_differs() {
        Preferences p;
        auto t = plan_transport_toggles(p, 0);
        QVERIFY(!t.toggle_click && !t.toggle_count_off);
        p.click_at_start = true;
        t = plan_transport_toggles(p, 0);
        QVERIFY(t.toggle_click && !t.toggle_count_off);
        // Already on (a reconnect): nothing to send.
        t = plan_transport_toggles(p, kTransportClick);
        QVERIFY(!t.toggle_click);
        p.count_off_at_start = true;
        t = plan_transport_toggles(p, kTransportClick);
        QVERIFY(!t.toggle_click && t.toggle_count_off);
        // Off in the preference, on in the engine: toggled off.
        p = Preferences{};
        t = plan_transport_toggles(p, kTransportClick | kTransportCountOff);
        QVERIFY(t.toggle_click && t.toggle_count_off);
    }
    void version_1_loads_with_new_fields_false() {
        QTemporaryDir dir;
        const QString f = dir.filePath("p.json");
        QVERIFY(write(f, "{\"version\": 1}"));
        Preferences p;
        p.click_at_start = false;
        std::string why;
        QVERIFY2(load_preferences(f, p, why), why.c_str());
        QVERIFY(!p.click_at_start && !p.count_off_at_start);
    }
    void version_2_round_trips() {
        QTemporaryDir dir;
        const QString f = dir.filePath("p.json");
        Preferences p;
        p.click_at_start = true;
        p.count_off_at_start = true;
        std::string why;
        QVERIFY2(save_preferences(f, p, why), why.c_str());
        Preferences q;
        QVERIFY2(load_preferences(f, q, why), why.c_str());
        QVERIFY(q.click_at_start && q.count_off_at_start);
    }
    void version_3_and_bad_values_are_refused_with_a_reason() {
        QTemporaryDir dir;
        const QString f = dir.filePath("p.json");
        Preferences p;
        std::string why;
        QVERIFY(write(f, "{\"version\": 3}"));
        QVERIFY(!load_preferences(f, p, why));
        QVERIFY(why.find("version 3") != std::string::npos);
        why.clear();
        QVERIFY(write(f, "{\"version\": 2, \"click_at_start\": \"yes\"}"));
        QVERIFY(!load_preferences(f, p, why));
        QVERIFY(!why.empty());
        QVERIFY(!p.click_at_start);
    }
};

QTEST_GUILESS_MAIN(TransportPrefsTest)
#include "test_transport_prefs.moc"
