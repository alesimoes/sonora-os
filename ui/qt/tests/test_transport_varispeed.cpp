// SPDX-License-Identifier: GPL-3.0-or-later
//
// The varispeed field (ADR-0190):
//   - typing 150 sends transport.varispeed with "1.5", and the field shows
//     150 % and +7.02 st;
//   - 49 and 201, text that is not a number, and NaN are refused in the
//     field before anything is sent, the reason held, the value unchanged;
//   - a reset sends "1" and is 0.00 st;
//   - through the shell the command reaches the fake engine, and the bar
//     carries the field.
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickView>
#include <cstdio>
#include <string>
#include <vector>

#include "control_adapter.h"
#include "key_router.h"
#include "peak_source.h"
#include "register_types.h"
#include "replica_document.h"
#include "session_model.h"
#include "shell_model.h"
#include "sonora/api/fake_engine.h"
#include "sonora/model/op_codec.h"
#include "theme.h"
#include "timeline_document.h"
#include "transport_varispeed.h"

using namespace sonora;
using namespace sonora::ui;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                            \
        }                                                                        \
    } while (0)

namespace {

int typing() {
    std::vector<std::string> sent;
    TransportVarispeed field([&sent](const QString& r) {
        sent.push_back(r.toStdString());
        return true;
    });
    CHECK(field.atUnity());
    CHECK(field.semitonesText() == QStringLiteral("0.00 st"));

    CHECK(field.submit(QStringLiteral("150")));
    CHECK(sent.size() == 1 && sent[0] == "1.5");
    CHECK(field.percentText() == QStringLiteral("150 %"));
    CHECK(field.semitonesText() == QStringLiteral("+7.02 st"));

    CHECK(field.submit(QStringLiteral(" 50 % ")));
    CHECK(sent.back() == "0.5");
    CHECK(field.semitonesText() == QStringLiteral("-12.00 st"));
    CHECK(field.submit(QStringLiteral("200")));
    CHECK(sent.back() == "2");

    CHECK(field.submit(QStringLiteral("87.5")));
    CHECK(sent.back() == "0.875");
    CHECK(field.percentText() == QStringLiteral("87.50 %"));
    return 0;
}

int refusals() {
    std::vector<std::string> sent;
    TransportVarispeed field([&sent](const QString& r) {
        sent.push_back(r.toStdString());
        return true;
    });
    CHECK(field.submit(QStringLiteral("150")));
    sent.clear();
    for (const char* bad : {"49", "201", "49.99", "0", "-100", "", "abc", "nan", "inf", "1e2x"}) {
        CHECK(!field.submit(QString::fromLatin1(bad)));
        CHECK(!field.refusal().isEmpty());
        CHECK(field.percent() == 150.0);  // the shown value is still the sent one
    }
    CHECK(sent.empty());
    CHECK(field.refusal().contains(QStringLiteral("not a speed")));
    CHECK(!field.submit(QStringLiteral("201")));
    CHECK(field.refusal().contains(QStringLiteral("outside 50 to 200")));
    // The next good entry takes the notice away.
    CHECK(field.submit(QStringLiteral("100")));
    CHECK(field.refusal().isEmpty());

    // Nothing to send through: refused, not shown as done.
    TransportVarispeed unlinked;
    CHECK(!unlinked.submit(QStringLiteral("120")));
    CHECK(unlinked.atUnity() && !unlinked.refusal().isEmpty());
    TransportVarispeed refused([](const QString&) { return false; });
    CHECK(!refused.submit(QStringLiteral("120")));
    CHECK(refused.atUnity());
    return 0;
}

int resetting() {
    std::vector<std::string> sent;
    TransportVarispeed field([&sent](const QString& r) {
        sent.push_back(r.toStdString());
        return true;
    });
    CHECK(field.submit(QStringLiteral("75")));
    CHECK(!field.atUnity());
    CHECK(field.reset());
    CHECK(sent.back() == "1");
    CHECK(field.atUnity() && field.semitonesText() == QStringLiteral("0.00 st"));
    return 0;
}

int through_the_shell_and_qml() {
    api::testing::FakeEngine::Config cfg;
    cfg.tracks = 2;
    api::testing::FakeEngine fake{cfg};
    ControlAdapter engine{fake};
    ReplicaDocument doc{[&engine](const api::ModelRequest& r) { return engine.submitModel(r); },
                        make_empty_session(2, 48000)};
    SyntheticPeakSource peaks{48000, 120.0};
    SessionModel session{doc, peaks, true};
    KeyRouter router{KeyMap::built_in_default(), api::testing::FakeEngine::engine_commands(),
                     [&engine](const std::string& c, const CommandContext& ctx) {
                         return engine.send(c, ctx);
                     }};
    router.prepare({});
    ShellModel shell{engine, router};
    std::string error;
    CHECK(engine.connect(error));
    CHECK(shell.implemented(QStringLiteral("transport.varispeed")));

    QQuickView view;
    view.resize(1280, 720);
    view.setInitialProperties({{QStringLiteral("shell"), QVariant::fromValue(&shell)},
                               {QStringLiteral("session"), QVariant::fromValue(&session)}});
    view.setSource(QUrl(QStringLiteral("qrc:/sonora/qml/Main.qml")));
    for (const auto& e : view.errors()) std::fprintf(stderr, "%s\n", qPrintable(e.toString()));
    CHECK(view.status() == QQuickView::Ready);
    view.show();
    QQuickItem* bar = view.rootObject()->findChild<QQuickItem*>(QStringLiteral("varispeed"));
    CHECK(bar != nullptr);
    auto* model = bar->property("model").value<TransportVarispeed*>();
    CHECK(model != nullptr);
    CHECK(model->submit(QStringLiteral("150")));
    CHECK(!model->submit(QStringLiteral("49")));
    for (int i = 0; i < 12; ++i) shell.tick();
    CHECK(model->percent() == 150.0);
    QQuickItem* st = bar->findChild<QQuickItem*>(QStringLiteral("varispeedSemitones"));
    CHECK(st != nullptr);
    CHECK(st->property("text").toString() == QStringLiteral("+7.02 st"));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    register_types();
    if (typing() != 0) return 1;
    if (refusals() != 0) return 1;
    if (resetting() != 0) return 1;
    if (through_the_shell_and_qml() != 0) return 1;
    std::printf("ui transport varispeed: all checks passed\n");
    return 0;
}
