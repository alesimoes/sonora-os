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
    // __FILE__ keeps the build files out of this: the font sits beside us.
    const QString path = QString::fromUtf8(__FILE__).replace(QLatin1Char('\\'),QLatin1Char('/')).section(QLatin1Char('/'), 0, -2)
        + QStringLiteral("/fonts/DejaVuSans.ttf");
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
