#include "ui/theme/IconFactory.h"

#include <QColor>
#include <QPainter>
#include <QPainterPath>
#include <QRectF>
#include <QtMath>

namespace devdatapod::icons {

namespace {

void poly(QPainter& p, const QVector<QPointF>& pts, bool closed = false) {
    QPainterPath path;
    if (pts.isEmpty()) return;
    path.moveTo(pts.first());
    for (int i = 1; i < pts.size(); ++i) path.lineTo(pts[i]);
    if (closed) path.closeSubpath();
    p.drawPath(path);
}

void dot(QPainter& p, const QPointF& center, qreal radius) {
    const QColor fill = p.pen().color();
    p.save();
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawEllipse(center, radius, radius);
    p.restore();
}

}

QPixmap pixmap(Glyph glyph, const QColor& color, int size, qreal penWidth) {
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(size / 24.0, size / 24.0);
    p.setPen(QPen(color, penWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);

    switch (glyph) {
    case Glyph::House:
        poly(p, {{3, 10.5}, {12, 3}, {21, 10.5}});
        p.drawRoundedRect(QRectF(5, 10, 14, 11), 2, 2);
        break;
    case Glyph::Globe:
        p.drawEllipse(QPointF(12, 12), 9, 9);
        p.drawEllipse(QPointF(12, 12), 3.9, 9);
        p.drawLine(QPointF(3.4, 12), QPointF(20.6, 12));
        break;
    case Glyph::Database:
        p.drawEllipse(QRectF(4, 2.5, 16, 5.6));
        p.drawArc(QRectF(4, 9.2, 16, 5.6), 180 * 16, -180 * 16);
        p.drawArc(QRectF(4, 15.9, 16, 5.6), 180 * 16, -180 * 16);
        p.drawLine(QPointF(4, 5.3), QPointF(4, 18.7));
        p.drawLine(QPointF(20, 5.3), QPointF(20, 18.7));
        break;
    case Glyph::Monitor:
        p.drawRoundedRect(QRectF(3, 4, 18, 12), 2, 2);
        p.drawLine(QPointF(9, 20.5), QPointF(15, 20.5));
        p.drawLine(QPointF(12, 16), QPointF(12, 20.5));
        break;
    case Glyph::Package:
        poly(p, {{12, 2.5}, {20.5, 7.2}, {20.5, 16.8}, {12, 21.5}, {3.5, 16.8}, {3.5, 7.2}}, true);
        poly(p, {{3.5, 7.2}, {12, 12}, {20.5, 7.2}});
        p.drawLine(QPointF(12, 12), QPointF(12, 21.5));
        break;
    case Glyph::Settings: {
        p.drawEllipse(QPointF(12, 12), 3.2, 3.2);
        for (int i = 0; i < 8; ++i) {
            const double angle = qDegreesToRadians(i * 45.0);
            const QPointF dir(std::cos(angle), std::sin(angle));
            p.drawLine(12 + dir.x() * 5.6, 12 + dir.y() * 5.6, 12 + dir.x() * 8.6, 12 + dir.y() * 8.6);
        }
        break;
    }
    case Glyph::Activity:
        poly(p, {{3, 12}, {7.5, 12}, {10.5, 4.5}, {14, 19.5}, {16.8, 12}, {21, 12}});
        break;
    case Glyph::ChartPie:
        p.drawEllipse(QPointF(12, 12), 9, 9);
        p.drawLine(QPointF(12, 12), QPointF(12, 3));
        p.drawLine(QPointF(12, 12), QPointF(19.4, 16.4));
        break;
    case Glyph::FileText:
        poly(p, {{6, 3}, {14, 3}, {19, 8}, {19, 21}, {6, 21}}, true);
        poly(p, {{14, 3}, {14, 8}, {19, 8}});
        p.drawLine(QPointF(9, 13.5), QPointF(16, 13.5));
        p.drawLine(QPointF(9, 17.5), QPointF(16, 17.5));
        break;
    case Glyph::Speaker:
        poly(p, {{3, 9.5}, {7, 9.5}, {12, 5}, {12, 19}, {7, 14.5}, {3, 14.5}}, true);
        p.drawArc(QRectF(12, 7.5, 8, 9), -50 * 16, 100 * 16);
        p.drawArc(QRectF(12, 4, 13, 16), -45 * 16, 90 * 16);
        break;
    case Glyph::Info:
        p.drawEllipse(QPointF(12, 12), 9, 9);
        p.drawLine(QPointF(12, 11.2), QPointF(12, 16.4));
        dot(p, QPointF(12, 7.8), 0.9);
        break;
    case Glyph::Minus:
        p.drawLine(QPointF(4.5, 12), QPointF(19.5, 12));
        break;
    case Glyph::Square:
        p.drawRoundedRect(QRectF(4.5, 4.5, 15, 15), 2, 2);
        break;
    case Glyph::Restore:
        p.drawRoundedRect(QRectF(8.5, 4.5, 11, 11), 2, 2);
        poly(p, {{4.5, 8.5}, {4.5, 19.5}, {15.5, 19.5}});
        break;
    case Glyph::Close:
        p.drawLine(QPointF(6, 6), QPointF(18, 18));
        p.drawLine(QPointF(18, 6), QPointF(6, 18));
        break;
    }
    p.end();
    return pm;
}

QIcon icon(Glyph glyph, const QColor& color, int size, qreal penWidth) {
    return QIcon(pixmap(glyph, color, size, penWidth));
}

}
