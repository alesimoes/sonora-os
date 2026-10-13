// SPDX-License-Identifier: GPL-3.0-or-later
//
// theme.h's tokens, for QML, as the `Theme` singleton of the Sonora module.
// QML names a token (Theme.panel, Theme.space2, Theme.textBody); it never
// spells a colour or a size. Every property is CONSTANT: the theme does not
// change while the interface runs, so no binding on it is re-evaluated.
// Written out longhand because moc does not expand macros.
#pragma once

#include <QColor>
#include <QObject>
#include <QString>

#include "theme.h"

namespace sonora::ui {

class ThemeObject : public QObject {
    Q_OBJECT
    Q_PROPERTY(QColor sunken READ sunken CONSTANT)
    Q_PROPERTY(QColor window READ window CONSTANT)
    Q_PROPERTY(QColor panel READ panel CONSTANT)
    Q_PROPERTY(QColor raised READ raised CONSTANT)
    Q_PROPERTY(QColor hover READ hover CONSTANT)
    Q_PROPERTY(QColor borderSubtle READ borderSubtle CONSTANT)
    Q_PROPERTY(QColor borderStrong READ borderStrong CONSTANT)
    Q_PROPERTY(QColor text READ text CONSTANT)
    Q_PROPERTY(QColor textSecondary READ textSecondary CONSTANT)
    Q_PROPERTY(QColor textTertiary READ textTertiary CONSTANT)
    Q_PROPERTY(QColor textOnAccent READ textOnAccent CONSTANT)
    Q_PROPERTY(QColor accent READ accent CONSTANT)
    Q_PROPERTY(QColor record READ record CONSTANT)
    Q_PROPERTY(QColor solo READ solo CONSTANT)
    Q_PROPERTY(QColor mute READ mute CONSTANT)
    Q_PROPERTY(QColor input READ input CONSTANT)
    Q_PROPERTY(QColor caution READ caution CONSTANT)
    Q_PROPERTY(QColor alarm READ alarm CONSTANT)
    Q_PROPERTY(QColor alarmSurface READ alarmSurface CONSTANT)
    Q_PROPERTY(QColor playhead READ playhead CONSTANT)
    Q_PROPERTY(QColor bypass READ bypass CONSTANT)
    Q_PROPERTY(int insertSlotHeight READ insertSlotHeight CONSTANT)
    Q_PROPERTY(QColor meterWell READ meterWell CONSTANT)
    Q_PROPERTY(QColor highlight READ highlight CONSTANT)
    Q_PROPERTY(QColor shadow READ shadow CONSTANT)
    Q_PROPERTY(QColor selectionFill READ selectionFill CONSTANT)
    Q_PROPERTY(int space1 READ space1 CONSTANT)
    Q_PROPERTY(int space2 READ space2 CONSTANT)
    Q_PROPERTY(int space3 READ space3 CONSTANT)
    Q_PROPERTY(int space4 READ space4 CONSTANT)
    Q_PROPERTY(int space6 READ space6 CONSTANT)
    Q_PROPERTY(int radius READ radius CONSTANT)
    Q_PROPERTY(int radiusSmall READ radiusSmall CONSTANT)
    Q_PROPERTY(int textOverline READ textOverline CONSTANT)
    Q_PROPERTY(int textCaption READ textCaption CONSTANT)
    Q_PROPERTY(int textSmall READ textSmall CONSTANT)
    Q_PROPERTY(int textBody READ textBody CONSTANT)
    Q_PROPERTY(int textTitle READ textTitle CONSTANT)
    Q_PROPERTY(int textCounter READ textCounter CONSTANT)
    Q_PROPERTY(int textDisplay READ textDisplay CONSTANT)
    Q_PROPERTY(int transportHeight READ transportHeight CONSTANT)
    Q_PROPERTY(int toolbarHeight READ toolbarHeight CONSTANT)
    Q_PROPERTY(int rulerHeight READ rulerHeight CONSTANT)
    Q_PROPERTY(int trackHeaderWidth READ trackHeaderWidth CONSTANT)
    Q_PROPERTY(int trackHeight READ trackHeight CONSTANT)
    Q_PROPERTY(QString fontFamily READ fontFamily CONSTANT)

public:
    explicit ThemeObject(QObject* parent = nullptr) : QObject(parent) {}

    QColor sunken() const { return QColor::fromRgba(theme::kSunken); }
    QColor window() const { return QColor::fromRgba(theme::kWindow); }
    QColor panel() const { return QColor::fromRgba(theme::kPanel); }
    QColor raised() const { return QColor::fromRgba(theme::kRaised); }
    QColor hover() const { return QColor::fromRgba(theme::kHover); }
    QColor borderSubtle() const { return QColor::fromRgba(theme::kBorderSubtle); }
    QColor borderStrong() const { return QColor::fromRgba(theme::kBorderStrong); }
    QColor text() const { return QColor::fromRgba(theme::kText); }
    QColor textSecondary() const { return QColor::fromRgba(theme::kTextSecondary); }
    QColor textTertiary() const { return QColor::fromRgba(theme::kTextTertiary); }
    QColor textOnAccent() const { return QColor::fromRgba(theme::kTextOnAccent); }
    QColor accent() const { return QColor::fromRgba(theme::kAccent); }
    QColor record() const { return QColor::fromRgba(theme::kRecord); }
    QColor solo() const { return QColor::fromRgba(theme::kSolo); }
    QColor mute() const { return QColor::fromRgba(theme::kMute); }
    QColor input() const { return QColor::fromRgba(theme::kInput); }
    QColor caution() const { return QColor::fromRgba(theme::kCaution); }
    QColor alarm() const { return QColor::fromRgba(theme::kAlarm); }
    QColor alarmSurface() const { return QColor::fromRgba(theme::kAlarmSurface); }
    QColor playhead() const { return QColor::fromRgba(theme::kPlayhead); }
    QColor bypass() const { return QColor::fromRgba(theme::kBypass); }
    int insertSlotHeight() const { return theme::kInsertSlotHeight; }
    QColor meterWell() const { return QColor::fromRgba(theme::kMeterWell); }
    QColor highlight() const { return QColor::fromRgba(theme::kHighlight); }
    QColor shadow() const { return QColor::fromRgba(theme::kShadow); }
    QColor selectionFill() const { return QColor::fromRgba(theme::kSelectionFill); }
    int space1() const { return theme::kSpace1; }
    int space2() const { return theme::kSpace2; }
    int space3() const { return theme::kSpace3; }
    int space4() const { return theme::kSpace4; }
    int space6() const { return theme::kSpace6; }
    int radius() const { return theme::kRadius; }
    int radiusSmall() const { return theme::kRadiusSmall; }
    int textOverline() const { return theme::kTextOverline; }
    int textCaption() const { return theme::kTextCaption; }
    int textSmall() const { return theme::kTextSmall; }
    int textBody() const { return theme::kTextBody; }
    int textTitle() const { return theme::kTextTitle; }
    int textCounter() const { return theme::kTextCounter; }
    int textDisplay() const { return theme::kTextDisplay; }
    int transportHeight() const { return theme::kTransportHeight; }
    int toolbarHeight() const { return theme::kToolbarHeight; }
    int rulerHeight() const { return theme::kRulerHeight; }
    int trackHeaderWidth() const { return theme::kTrackHeaderWidth; }
    int trackHeight() const { return theme::kTrackHeight; }
    QString fontFamily() const { return QString::fromLatin1(theme::kFontFamily); }

    // The region colour of display track `index`, for its header swatch.
    Q_INVOKABLE QColor trackColour(int index) const {
        return QColor::fromRgba(theme::region_colour(index));
    }
};

}  // namespace sonora::ui
