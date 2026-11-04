// SPDX-License-Identifier: GPL-3.0-or-later
//
// A curve a plugin publishes (ADR-0057): an EQ's frequency response, a
// compressor's transfer curve. `points` are y values at evenly spaced x,
// spaced evenly in the log domain when `xLog`. Axes come from the plugin's
// own declaration, so the grid labels are its units.
//
// No points, no line: before the first frame, or once they stop, the well is
// drawn empty with its message, never a flat line that would read as a
// response of 0 dB.
import QtQuick
import Sonora

Item {
    id: view
    property var points: []
    property string title
    property string xUnit
    property string yUnit
    property bool xLog: true
    property real xMin: 20
    property real xMax: 20000
    property real yMin: -24
    property real yMax: 24
    property string emptyText: "waiting for the plugin"
    // The y that draws as the reference line (0 dB on an EQ, the diagonal's
    // start on a transfer curve is the plugin's business).
    property bool zeroLine: true
    implicitHeight: 132

    onPointsChanged: canvas.requestPaint()
    onYMinChanged: canvas.requestPaint()
    onYMaxChanged: canvas.requestPaint()

    Rectangle {
        anchors.fill: parent
        radius: Theme.radius
        color: Theme.meterWell
        border.color: Theme.borderSubtle
    }
    Canvas {
        id: canvas
        anchors { fill: parent; margins: 1 }
        renderTarget: Canvas.Image
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const w = width, h = height
            const left = 30, bottom = 14
            const pw = w - left - 6, ph = h - bottom - 6
            function px(x) {
                const t = view.xLog ? Math.log(x / view.xMin) / Math.log(view.xMax / view.xMin)
                                    : (x - view.xMin) / (view.xMax - view.xMin)
                return left + Math.max(0, Math.min(1, t)) * pw
            }
            function py(y) {
                return 6 + (1 - (Math.max(view.yMin, Math.min(view.yMax, y)) - view.yMin) / (view.yMax - view.yMin)) * ph
            }
            ctx.font = "9px sans-serif"
            ctx.lineWidth = 1
            // Horizontal grid: five lines, labelled in the y unit.
            for (let i = 0; i <= 4; ++i) {
                const y = view.yMin + (view.yMax - view.yMin) * i / 4
                ctx.strokeStyle = (view.zeroLine && Math.abs(y) < 1e-6) ? "#4a525b" : "#232830"
                ctx.beginPath()
                ctx.moveTo(left, Math.round(py(y)) + 0.5)
                ctx.lineTo(left + pw, Math.round(py(y)) + 0.5)
                ctx.stroke()
                ctx.fillStyle = "#7c8590"
                ctx.textAlign = "right"
                ctx.fillText(Math.round(y), left - 4, py(y) + 3)
            }
            // Vertical grid: decades on a log axis, quarters otherwise.
            ctx.textAlign = "center"
            const marks = []
            if (view.xLog) {
                for (let d = Math.pow(10, Math.floor(Math.log(view.xMin) / Math.LN10)); d <= view.xMax; d *= 10)
                    for (const m of [1, 2, 5]) if (d * m >= view.xMin && d * m <= view.xMax) marks.push(d * m)
            } else {
                for (let i = 0; i <= 4; ++i) marks.push(view.xMin + (view.xMax - view.xMin) * i / 4)
            }
            for (const x of marks) {
                ctx.strokeStyle = "#1d2127"
                ctx.beginPath()
                ctx.moveTo(Math.round(px(x)) + 0.5, 6)
                ctx.lineTo(Math.round(px(x)) + 0.5, 6 + ph)
                ctx.stroke()
                const label = x >= 1000 ? (x / 1000) + "k" : "" + Math.round(x)
                if (label[0] === "1" || label[0] === "2" || label[0] === "5" || !view.xLog) {
                    ctx.fillStyle = "#7c8590"
                    ctx.fillText(label, px(x), h - 3)
                }
            }
            const pts = view.points
            if (!pts || pts.length < 2) return
            ctx.strokeStyle = "#3fc1b0"
            ctx.lineWidth = 1.6
            ctx.beginPath()
            for (let i = 0; i < pts.length; ++i) {
                const t = i / (pts.length - 1)
                const x = left + t * pw
                if (i === 0) ctx.moveTo(x, py(pts[i]))
                else ctx.lineTo(x, py(pts[i]))
            }
            ctx.stroke()
            // A faint fill to the reference line.
            ctx.lineTo(left + pw, py(Math.max(view.yMin, Math.min(view.yMax, 0))))
            ctx.lineTo(left, py(Math.max(view.yMin, Math.min(view.yMax, 0))))
            ctx.closePath()
            ctx.fillStyle = "rgba(63,193,176,0.12)"
            ctx.fill()
        }
    }
    Text {
        visible: !view.points || view.points.length < 2
        anchors.centerIn: parent
        text: view.emptyText
        color: Theme.textTertiary
        font { family: Theme.fontFamily; pixelSize: Theme.textSmall }
    }
    Text {
        anchors { left: parent.left; top: parent.top; margins: Theme.space1 }
        leftPadding: 28
        text: view.title + (view.yUnit.length > 0 ? " (" + view.yUnit + ")" : "")
        color: Theme.textSecondary
        font { family: Theme.fontFamily; pixelSize: Theme.textCaption }
    }
}
