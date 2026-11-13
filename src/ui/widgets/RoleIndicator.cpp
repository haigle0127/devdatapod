#include "ui/widgets/RoleIndicator.h"

#include <QMouseEvent>
#include <QPainter>
#include <QSvgRenderer>

namespace devdatapod {

namespace {
constexpr int kIconSize = 22;
constexpr const char* kOnIcon = ":/devdatapod/ui/theme/icons/default-on.svg";
constexpr const char* kOffIcon = ":/devdatapod/ui/theme/icons/default-off.svg";
}

RoleIndicator::RoleIndicator(QWidget* parent) : QLabel(parent) {
    setFixedSize(kIconSize, kIconSize);
}

void RoleIndicator::setRole(bool isDefault, bool clickable) {
    isDefault_ = isDefault;
    clickable_ = clickable;
    // default 不可点（已是默认，没得切）；备用可点（点了设为默认）。
    setCursor(clickable_ ? Qt::PointingHandCursor : Qt::ArrowCursor);
    setToolTip(isDefault_ ? QStringLiteral("默认实例")
                          : QStringLiteral("设为默认实例"));
    render();
}

void RoleIndicator::render() {
    QSvgRenderer renderer(QString::fromLatin1(isDefault_ ? kOnIcon : kOffIcon));
    QPixmap pixmap(kIconSize, kIconSize);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    renderer.render(&p);
    setPixmap(pixmap);
}

void RoleIndicator::mousePressEvent(QMouseEvent* event) {
    if (clickable_ && event->button() == Qt::LeftButton) {
        emit clicked();
        return;
    }
    QLabel::mousePressEvent(event);
}

}
