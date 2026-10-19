// SPDX-License-Identifier: GPL-3.0-or-later
#include "refusal_page.h"

#include <QColor>
#include <QQuickView>
#include <QUrl>
#include <QVariant>

#include "theme.h"

namespace sonora::ui {

bool load_refusal_page(QQuickView& view, const QString& reason, QString& error) {
    QString text = reason;
    if (!text.isEmpty()) text[0] = text[0].toUpper();
    view.setColor(QColor::fromRgba(theme::kWindow));
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    view.setInitialProperties({{QStringLiteral("reason"), text}});
    view.setSource(QUrl(QStringLiteral("qrc:/sonora/qml/RefusalPage.qml")));
    if (view.status() != QQuickView::Error) return true;
    for (const auto& e : view.errors()) {
        if (!error.isEmpty()) error += QLatin1Char('\n');
        error += e.toString();
    }
    return false;
}

}  // namespace sonora::ui
