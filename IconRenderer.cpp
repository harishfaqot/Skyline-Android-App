#include "IconRenderer.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QtMath>

namespace IconRenderer
{
namespace
{

// All icons are drawn in a fixed 24x24 logical viewBox, then the painter
// transform scales that up to whatever pixelSize was requested. Keeping a
// single viewBox makes every icon visually consistent (same stroke weight,
// same optical size) regardless of where it's used.
constexpr qreal kViewBox = 24.0;

QPen strokePen(const QColor &color, qreal width = 1.8)
{
    QPen pen(color);
    pen.setWidthF(width);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    return pen;
}

void drawBattery(QPainter &p, const QColor &color)
{
    p.setPen(strokePen(color));
    p.setBrush(Qt::NoBrush);
    QRectF body(2.5, 7, 16, 10);
    p.drawRoundedRect(body, 2, 2);
    // Terminal nub
    QRectF nub(19.2, 10, 2.3, 4);
    p.setBrush(color);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(nub, 1, 1);
    // Fill level (fixed ~70% — purely decorative, actual % is shown as text)
    QRectF fill(4.3, 8.6, 10.5, 6.8);
    p.setBrush(color);
    p.drawRoundedRect(fill, 1, 1);
}

void drawSignal(QPainter &p, const QColor &color)
{
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    struct Bar { qreal x, h; };
    const Bar bars[4] = { {2.5, 4.5}, {8.0, 8.0}, {13.5, 12.0}, {19.0, 16.0} };
    for (const auto &b : bars) {
        QRectF r(b.x, 20 - b.h, 3.2, b.h);
        p.drawRoundedRect(r, 0.8, 0.8);
    }
}

void drawMap(QPainter &p, const QColor &color)
{
    p.setPen(strokePen(color));
    p.setBrush(Qt::NoBrush);
    QPainterPath path;
    path.moveTo(3, 6);
    path.lineTo(9, 3.5);
    path.lineTo(15, 6);
    path.lineTo(21, 3.5);
    path.lineTo(21, 18);
    path.lineTo(15, 20.5);
    path.lineTo(9, 18);
    path.lineTo(3, 20.5);
    path.closeSubpath();
    p.drawPath(path);
    p.drawLine(QPointF(9, 3.5), QPointF(9, 18));
    p.drawLine(QPointF(15, 6), QPointF(15, 20.5));
}

void drawWaypoint(QPainter &p, const QColor &color)
{
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    QPainterPath pin;
    pin.moveTo(12, 22);
    pin.cubicTo(12, 22, 4.5, 13.2, 4.5, 8.5);
    pin.arcTo(QRectF(4.5, 1.5, 15, 15), 180, -180);
    pin.cubicTo(19.5, 13.2, 12, 22, 12, 22);
    p.drawPath(pin);
    p.setBrush(QColor("#14140f"));
    p.drawEllipse(QPointF(12, 8.6), 2.7, 2.7);
}

void drawScan(QPainter &p, const QColor &color)
{
    p.setPen(strokePen(color, 1.8));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(12, 12), 8.5, 8.5);
    p.drawEllipse(QPointF(12, 12), 3.6, 3.6);
    p.drawLine(QPointF(12, 1.5), QPointF(12, 4.6));
    p.drawLine(QPointF(12, 19.4), QPointF(12, 22.5));
    p.drawLine(QPointF(1.5, 12), QPointF(4.6, 12));
    p.drawLine(QPointF(19.4, 12), QPointF(22.5, 12));
}

void drawSettings(QPainter &p, const QColor &color)
{
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    const QPointF center(12, 12);
    const int teeth = 8;
    QPainterPath gear;
    for (int i = 0; i < teeth; ++i) {
        qreal a0 = (360.0 / teeth) * i;
        qreal a1 = a0 + (360.0 / teeth) * 0.5;
        QRectF outer(center.x() - 10, center.y() - 10, 20, 20);
        if (i == 0) gear.arcMoveTo(outer, a0);
        gear.arcTo(outer, a0, (360.0 / teeth) * 0.5);
        QRectF inner(center.x() - 7.3, center.y() - 7.3, 14.6, 14.6);
        gear.arcTo(inner, a1, (360.0 / teeth) * 0.5);
    }
    gear.closeSubpath();
    p.drawPath(gear);
    p.setBrush(QColor("#14140f"));
    p.drawEllipse(center, 3.6, 3.6);
}

void drawCube3D(QPainter &p, const QColor &color)
{
    p.setPen(strokePen(color, 1.6));
    p.setBrush(Qt::NoBrush);
    const QPointF top(12, 3);
    const QPointF left(3.5, 8);
    const QPointF right(20.5, 8);
    const QPointF bottom(12, 13);
    const QPointF botLeft(3.5, 16.5);
    const QPointF botRight(20.5, 16.5);
    const QPointF botTip(12, 21.5);

    QPainterPath outline;
    outline.moveTo(top);
    outline.lineTo(right);
    outline.lineTo(botRight);
    outline.lineTo(botTip);
    outline.lineTo(botLeft);
    outline.lineTo(left);
    outline.closeSubpath();
    p.drawPath(outline);
    p.drawLine(top, bottom);
    p.drawLine(bottom, botTip);
    p.drawLine(left, bottom);
    p.drawLine(right, bottom);
}

void drawTopView(QPainter &p, const QColor &color)
{
    // Viewfinder corners + center dot: a top-down camera look.
    p.setPen(strokePen(color, 2.0));
    p.setBrush(Qt::NoBrush);
    const qreal len = 5.5;
    // Top-left
    p.drawLine(QPointF(3, 3), QPointF(3 + len, 3));
    p.drawLine(QPointF(3, 3), QPointF(3, 3 + len));
    // Top-right
    p.drawLine(QPointF(21, 3), QPointF(21 - len, 3));
    p.drawLine(QPointF(21, 3), QPointF(21, 3 + len));
    // Bottom-left
    p.drawLine(QPointF(3, 21), QPointF(3 + len, 21));
    p.drawLine(QPointF(3, 21), QPointF(3, 21 - len));
    // Bottom-right
    p.drawLine(QPointF(21, 21), QPointF(21 - len, 21));
    p.drawLine(QPointF(21, 21), QPointF(21, 21 - len));
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawEllipse(QPointF(12, 12), 2.4, 2.4);
}

void drawRecord(QPainter &p, const QColor &color)
{
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawEllipse(QPointF(12, 12), 8, 8);
}

void drawRecordStop(QPainter &p, const QColor &color)
{
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    QRectF r(5.5, 5.5, 13, 13);
    p.drawRoundedRect(r, 2.5, 2.5);
}

void drawExpand(QPainter &p, const QColor &color)
{
    p.setPen(strokePen(color, 2.0));
    p.setBrush(Qt::NoBrush);
    // Top-left corner arrow
    p.drawPolyline(QPolygonF() << QPointF(9, 3) << QPointF(3, 3) << QPointF(3, 9));
    p.drawLine(QPointF(3, 3), QPointF(10, 10));
    // Bottom-right corner arrow
    p.drawPolyline(QPolygonF() << QPointF(15, 21) << QPointF(21, 21) << QPointF(21, 15));
    p.drawLine(QPointF(21, 21), QPointF(14, 14));
}

void drawRestore(QPainter &p, const QColor &color)
{
    p.setPen(strokePen(color, 1.8));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(7.5, 3.5, 13, 13), 1.6, 1.6);
    p.drawRoundedRect(QRectF(3.5, 7.5, 13, 13), 1.6, 1.6);
}

void drawClose(QPainter &p, const QColor &color)
{
    p.setPen(strokePen(color, 2.1));
    p.drawLine(QPointF(5.5, 5.5), QPointF(18.5, 18.5));
    p.drawLine(QPointF(18.5, 5.5), QPointF(5.5, 18.5));
}

void drawSwap(QPainter &p, const QColor &color)
{
    p.setPen(strokePen(color, 2.0));
    p.setBrush(color);
    // Top arrow, pointing right
    p.drawLine(QPointF(3.5, 8), QPointF(18, 8));
    QPainterPath headR;
    headR.moveTo(14.5, 3.5);
    headR.lineTo(20.5, 8);
    headR.lineTo(14.5, 12.5);
    headR.closeSubpath();
    p.setPen(Qt::NoPen);
    p.drawPath(headR);
    // Bottom arrow, pointing left
    p.setPen(strokePen(color, 2.0));
    p.drawLine(QPointF(20.5, 16), QPointF(6, 16));
    QPainterPath headL;
    headL.moveTo(9.5, 11.5);
    headL.lineTo(3.5, 16);
    headL.lineTo(9.5, 20.5);
    headL.closeSubpath();
    p.setPen(Qt::NoPen);
    p.drawPath(headL);
}

void drawSplit(QPainter &p, const QColor &color)
{
    p.setPen(strokePen(color, 1.8));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(3, 4, 18, 16), 2, 2);
    p.drawLine(QPointF(12, 4), QPointF(12, 20));
}

void drawTrash(QPainter &p, const QColor &color)
{
    p.setPen(strokePen(color, 1.8));
    p.setBrush(Qt::NoBrush);
    p.drawLine(QPointF(4, 6.5), QPointF(20, 6.5));
    p.drawLine(QPointF(9, 6.5), QPointF(9, 3.3));
    p.drawLine(QPointF(15, 6.5), QPointF(15, 3.3));
    p.drawLine(QPointF(9, 3.3), QPointF(15, 3.3));
    QPainterPath body;
    body.moveTo(5.5, 6.5);
    body.lineTo(6.6, 21);
    body.lineTo(17.4, 21);
    body.lineTo(18.5, 6.5);
    p.drawPath(body);
    p.drawLine(QPointF(10, 10), QPointF(10.4, 17.5));
    p.drawLine(QPointF(14, 10), QPointF(13.6, 17.5));
}

void drawCamera(QPainter &p, const QColor &color)
{
    p.setPen(strokePen(color, 1.8));
    p.setBrush(Qt::NoBrush);
    QPainterPath body;
    body.addRoundedRect(QRectF(2.5, 7, 19, 13), 2.5, 2.5);
    p.drawPath(body);
    QPainterPath bump;
    bump.addRoundedRect(QRectF(8.5, 3.5, 7, 4), 1.5, 1.5);
    p.drawPath(bump);
    p.drawEllipse(QPointF(12, 13.5), 4.2, 4.2);
}

void drawChevron(QPainter &p, const QColor &color, bool pointingRight)
{
    p.setPen(strokePen(color, 2.2));
    p.setBrush(Qt::NoBrush);
    QPolygonF poly;
    if (pointingRight) {
        poly << QPointF(9, 4) << QPointF(16, 12) << QPointF(9, 20);
    } else {
        poly << QPointF(15, 4) << QPointF(8, 12) << QPointF(15, 20);
    }
    p.drawPolyline(poly);
}

void drawChevronDown(QPainter &p, const QColor &color)
{
    p.setPen(strokePen(color, 2.2));
    p.setBrush(Qt::NoBrush);
    QPolygonF poly;
    poly << QPointF(4, 9) << QPointF(12, 16) << QPointF(20, 9);
    p.drawPolyline(poly);
}

void drawZoom(QPainter &p, const QColor &color, bool zoomIn)
{
    p.setPen(strokePen(color, 1.8));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(10.5, 10.5), 7, 7);
    p.drawLine(QPointF(15.6, 15.6), QPointF(21, 21));
    p.drawLine(QPointF(7, 10.5), QPointF(14, 10.5));
    if (zoomIn) {
        p.drawLine(QPointF(10.5, 7), QPointF(10.5, 14));
    }
}

void drawPlus(QPainter &p, const QColor &color)
{
    p.setPen(strokePen(color, 2.2));
    p.drawLine(QPointF(12, 4), QPointF(12, 20));
    p.drawLine(QPointF(4, 12), QPointF(20, 12));
}

void drawMinus(QPainter &p, const QColor &color)
{
    p.setPen(strokePen(color, 2.2));
    p.drawLine(QPointF(4, 12), QPointF(20, 12));
}

void drawDrone(QPainter &p, const QColor &color)
{
    // Simple top-down quadcopter glyph — used as the app-logo fallback if
    // the bundled skyline.png can't be loaded, so it must never depend on
    // an emoji font (that's exactly the "keyboard icon" bug being fixed).
    p.setPen(strokePen(color, 1.8));
    p.setBrush(Qt::NoBrush);
    p.drawLine(QPointF(5, 5), QPointF(10.2, 10.2));
    p.drawLine(QPointF(19, 5), QPointF(13.8, 10.2));
    p.drawLine(QPointF(5, 19), QPointF(10.2, 13.8));
    p.drawLine(QPointF(19, 19), QPointF(13.8, 13.8));
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    for (const QPointF &c : { QPointF(5, 5), QPointF(19, 5), QPointF(5, 19), QPointF(19, 19) }) {
        p.drawEllipse(c, 2.6, 2.6);
    }
    QRectF body(9.3, 9.3, 5.4, 5.4);
    p.drawRoundedRect(body, 1.2, 1.2);
}

void drawBolt(QPainter &p, const QColor &color)
{
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    QPainterPath bolt;
    bolt.moveTo(13.2, 2.5);
    bolt.lineTo(5.5, 13.8);
    bolt.lineTo(10.8, 13.8);
    bolt.lineTo(9.6, 21.5);
    bolt.lineTo(18.5, 9.6);
    bolt.lineTo(13.0, 9.6);
    bolt.closeSubpath();
    p.drawPath(bolt);
}

void drawWaveform(QPainter &p, const QColor &color)
{
    // Simple current/amperage glyph: an oscillating trace, like a tiny
    // ammeter readout.
    p.setPen(strokePen(color, 1.8));
    p.setBrush(Qt::NoBrush);
    QPainterPath wave;
    wave.moveTo(2, 12);
    wave.lineTo(6.5, 12);
    wave.cubicTo(7.5, 5, 9.5, 5, 10.5, 12);
    wave.cubicTo(11.5, 19, 13.5, 19, 14.5, 12);
    wave.cubicTo(15.5, 5, 17.5, 5, 18.5, 12);
    wave.lineTo(22, 12);
    p.drawPath(wave);
}

void drawMessages(QPainter &p, const QColor &color)
{
    // Speech-bubble glyph — used for the ArduPilot STATUSTEXT / "Messages"
    // panel entry point, same idea as QGC's toolbar message indicator.
    p.setPen(strokePen(color, 1.8));
    p.setBrush(Qt::NoBrush);
    QPainterPath bubble;
    bubble.addRoundedRect(QRectF(2.5, 4, 19, 13), 3, 3);
    p.drawPath(bubble);
    QPainterPath tail;
    tail.moveTo(7, 17);
    tail.lineTo(7, 21.5);
    tail.lineTo(11.5, 17);
    tail.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawPath(tail);
    p.setPen(strokePen(color, 1.8));
    p.drawLine(QPointF(6.5, 8.2), QPointF(17.5, 8.2));
    p.drawLine(QPointF(6.5, 12), QPointF(14, 12));
}

} // namespace

QIcon icon(Id id, const QColor &color, int pixelSize)
{
    pixelSize = qMax(8, pixelSize);
    QPixmap pixmap(pixelSize, pixelSize);
    pixmap.fill(Qt::transparent);

    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.scale(pixelSize / kViewBox, pixelSize / kViewBox);

    switch (id) {
    case Id::Battery:      drawBattery(p, color); break;
    case Id::Signal:       drawSignal(p, color); break;
    case Id::Map:          drawMap(p, color); break;
    case Id::Waypoint:     drawWaypoint(p, color); break;
    case Id::Scan:         drawScan(p, color); break;
    case Id::Settings:     drawSettings(p, color); break;
    case Id::Cube3D:       drawCube3D(p, color); break;
    case Id::TopView:      drawTopView(p, color); break;
    case Id::Record:       drawRecord(p, color); break;
    case Id::RecordStop:   drawRecordStop(p, color); break;
    case Id::Expand:       drawExpand(p, color); break;
    case Id::Restore:      drawRestore(p, color); break;
    case Id::Close:        drawClose(p, color); break;
    case Id::Swap:         drawSwap(p, color); break;
    case Id::Split:        drawSplit(p, color); break;
    case Id::Trash:        drawTrash(p, color); break;
    case Id::Camera:       drawCamera(p, color); break;
    case Id::ChevronLeft:  drawChevron(p, color, false); break;
    case Id::ChevronRight: drawChevron(p, color, true); break;
    case Id::ChevronDown:  drawChevronDown(p, color); break;
    case Id::ZoomIn:       drawZoom(p, color, true); break;
    case Id::ZoomOut:      drawZoom(p, color, false); break;
    case Id::Plus:         drawPlus(p, color); break;
    case Id::Minus:        drawMinus(p, color); break;
    case Id::Drone:        drawDrone(p, color); break;
    case Id::Bolt:         drawBolt(p, color); break;
    case Id::Waveform:     drawWaveform(p, color); break;
    case Id::Messages:     drawMessages(p, color); break;
    }
    p.end();

    return QIcon(pixmap);
}

} // namespace IconRenderer
