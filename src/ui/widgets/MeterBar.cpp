#include "ui/widgets/MeterBar.h"

#include "ui/theme/Theme.h"

#include <QPainter>

namespace devdatapod {

namespace {
constexpr int kBarHeight = 10;
constexpr int kBarRadius = 5;
} // namespace

MeterBar::MeterBar(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_TranslucentBackground);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

QSize MeterBar::sizeHint() const {
    return {120, kBarHeight};
}

void MeterBar::setFraction(double fraction) {
    fraction_ = qBound(0.0, fraction, 1.0);
    update();
}

void MeterBar::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    Theme& theme = Theme::instance();
    const QRectF trackRect(0, 0, width(), kBarHeight);

    painter.setPen(Qt::NoPen);
    painter.setBrush(theme.color(QStringLiteral("track")));
    painter.drawRoundedRect(trackRect, kBarRadius, kBarRadius);

    if (fraction_ > 0.0) {
        const qreal fillWidth = qMax<qreal>(kBarHeight, trackRect.width() * fraction_);
        painter.setBrush(theme.color(QStringLiteral("accent")));
        painter.drawRoundedRect(QRectF(trackRect.topLeft(), QSizeF(fillWidth, kBarHeight)),
                                kBarRadius, kBarRadius);
    }
}

} // namespace devdatapod
