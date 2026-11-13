#pragma once

#include <QIcon>
#include <QPixmap>

class QColor;
class QPainter;

namespace devdatapod {

// The .pen uses the lucide icon set. Rather than shipping an icon font, the handful of
// glyphs the shell needs are drawn in a normalized 24x24 space so they can be tinted
// with design tokens at any size.
enum class Glyph {
    House,
    Globe,
    Database,
    Monitor,
    Package,
    Settings,
    Activity,
    ChartPie,
    FileText,
    Speaker,
    Info,
    Minus,
    Square,
    Restore,
    Close,
};

namespace icons {

QPixmap pixmap(Glyph glyph, const QColor& color, int size, qreal penWidth = 1.75);
QIcon icon(Glyph glyph, const QColor& color, int size, qreal penWidth = 1.75);

}

}
