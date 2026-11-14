#pragma once
// Registers the bundled DejaVu Sans (free licence, tests/fonts/LICENSE-DejaVu)
// so text measures the same on runners with no system fonts.
#include <QFontDatabase>
#include <QGuiApplication>
#include <QString>
#include <cstdio>

namespace sonora_test {

// Call after the QGuiApplication exists and before the first widget/view.
// Fails loudly: a missing font would make width checks measure zero again.
inline bool registerTestFont() {
    // Not __FILE__: ccache's CCACHE_BASEDIR makes it relative to the build
    // directory, which is not where ctest runs the test.
    const QString path = QString::fromUtf8(SONORA_TEST_FONT_DIR)
        + QStringLiteral("/DejaVuSans.ttf");
    const int id = QFontDatabase::addApplicationFont(path);
    if (id < 0) {
        std::fprintf(stderr, "cannot load test font %s\n", qPrintable(path));
        return false;
    }
    QFont f = QGuiApplication::font();
    f.setFamily(QFontDatabase::applicationFontFamilies(id).value(0));
    QGuiApplication::setFont(f);
    return true;
}

}  // namespace sonora_test
