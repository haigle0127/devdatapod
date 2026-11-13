#include "ui/widgets/ToggleSwitch.h"

#include "ui/theme/Theme.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>

#define _USE_MATH_DEFINES
#include <cmath>

namespace devdatapod {

namespace {
// 轨道与滑块尺寸，接近 iOS 的比例。
constexpr int kTrackW = 44;
constexpr int kTrackH = 26;
constexpr int kKnob = 20;          // 滑块直径
constexpr int kMargin = 3;         // 滑块到轨道边缘的间距
constexpr int kAnimMs = 160;       // 滑动动画时长
}

ToggleSwitch::ToggleSwitch(QWidget* parent) : QWidget(parent) {
    setFixedSize(kTrackW, kTrackH);
    setCursor(Qt::PointingHandCursor);
}

QSize ToggleSwitch::sizeHint() const {
    return QSize(kTrackW, kTrackH);
}

void ToggleSwitch::setOn(bool on, bool animate) {
    if (on_ == on) return;
    on_ = on;
    const qreal target = on_ ? 1.0 : 0.0;
    if (animate) {
        auto* anim = new QPropertyAnimation(this, "offset", this);
        anim->setDuration(kAnimMs);
        anim->setStartValue(offset_);
        anim->setEndValue(target);
        anim->start(QAbstractAnimation::DeleteWhenStopped);
    } else {
        setOffset(target);
    }
    update();
}

void ToggleSwitch::setLocked(bool locked) {
    locked_ = locked;
    setCursor(locked_ ? Qt::ForbiddenCursor : Qt::PointingHandCursor);
    update();
}

void ToggleSwitch::setOffset(qreal value) {
    offset_ = value;
    update();
}

void ToggleSwitch::mousePressEvent(QMouseEvent* event) {
    if (locked_) {
        event->ignore();
        return;
    }
    setOn(!on_);
    emit toggled(on_);
    QWidget::mousePressEvent(event);
}

void ToggleSwitch::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    Theme& theme = Theme::instance();
    const QRectF track = rect();

    // 轨道：开 = 主色，关 = 灰色轨道色。锁定时整体变淡。
    QColor trackColor = on_ ? theme.color(QStringLiteral("accent"))
                            : theme.color(QStringLiteral("track"));
    if (locked_) trackColor.setAlpha(120);
    p.setPen(Qt::NoPen);
    p.setBrush(trackColor);
    p.drawRoundedRect(track, kTrackH / 2.0, kTrackH / 2.0);

    // 滑块：白色圆形，随 offset 从左滑到右。
    const qreal travel = kTrackW - kKnob - kMargin * 2;
    const qreal x = kMargin + offset_ * travel;
    const QRectF knob(x, (kTrackH - kKnob) / 2.0, kKnob, kKnob);
    p.setBrush(theme.color(QStringLiteral("card")));
    p.drawEllipse(knob);

    // 图标画在轨道里、圆点对面那一侧：开（圆点右）→ 对勾在左；关（圆点左）→ 叉在右。
    // 图标落在彩色/灰色轨道上，所以用白色画。
    const qreal sideCenterX = on_ ? (kMargin + kKnob / 2.0)          // 轨道左侧中心
                                  : (kTrackW - kMargin - kKnob / 2.0); // 轨道右侧中心
    drawIcon(p, QPointF(sideCenterX, kTrackH / 2.0));
}

void ToggleSwitch::drawIcon(QPainter& p, const QPointF& center) {
    // 画在轨道上，统一用白色（开是蓝轨道、关是灰轨道，白色都清晰）。
    QColor color(255, 255, 255);
    if (locked_) color.setAlpha(120);
    p.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));

    const qreal cx = center.x();
    const qreal cy = center.y();

    switch (icon_) {
    case Checkmark: {
        QPainterPath check;
        check.moveTo(cx - 4, cy);
        check.lineTo(cx - 1, cy + 3.5);
        check.lineTo(cx + 4.5, cy - 3.5);
        p.drawPath(check);
        break;
    }
    case Star: {
        // 五角星：外接圆半径 6，内圈一半。
        QPainterPath star;
        const qreal rOut = 6.0, rIn = 2.5;
        for (int i = 0; i < 10; ++i) {
            const qreal r = (i % 2 == 0) ? rOut : rIn;
            const qreal angle = -M_PI / 2 + i * M_PI / 5;    // 从顶点开始
            const QPointF pt(cx + r * std::cos(angle), cy + r * std::sin(angle));
            if (i == 0) star.moveTo(pt);
            else star.lineTo(pt);
        }
        star.closeSubpath();
        p.drawPath(star);
        break;
    }
    case Power: {
        // 电源：一段开口圆弧 + 顶部一竖。
        p.drawArc(QRectF(cx - 5, cy - 4, 10, 10), -30 * 16, 240 * 16);
        p.drawLine(QPointF(cx, cy - 7), QPointF(cx, cy - 1));
        break;
    }
    case CheckCross: {
        // 开 = ✓（U+2713），关 = ✕（U+2715），用字体渲染比手画路径规整。
        QFont f;
        f.setFamilies({QStringLiteral("MiSans")});
        f.setPixelSize(13);
        f.setBold(true);
        p.setFont(f);
        const QString mark = on_ ? QStringLiteral("✓") : QStringLiteral("✕");
        // 以轨道那侧的中心为锚点画字符。
        const QRectF box(cx - 8, cy - 8, 16, 16);
        p.drawText(box, Qt::AlignCenter, mark);
        break;
    }
    }
}

}
