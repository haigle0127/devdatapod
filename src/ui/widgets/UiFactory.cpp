#include "ui/widgets/UiFactory.h"

#include "ui/theme/Theme.h"

#include <QEvent>
#include <QFrame>
#include <QLabel>

namespace devdatapod {

namespace ui {

QWidget* makeCard(QWidget* parent) {
    auto* card = new QWidget(parent);
    card->setObjectName(QStringLiteral("Card"));
    card->setAttribute(Qt::WA_StyledBackground, true);
    return card;
}

QFrame* makeAccentBar(const QString& tone, int height, QWidget* parent) {
    auto* bar = new QFrame(parent);
    bar->setObjectName(QStringLiteral("AccentBar"));
    bar->setAttribute(Qt::WA_StyledBackground, true);
    bar->setProperty("tone", tone);
    bar->setFixedWidth(4);
    bar->setFixedHeight(height);
    return bar;
}

QLabel* makeLabel(const QString& text, const QString& objectName, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(objectName);
    return label;
}

}

WindowControlButton::WindowControlButton(Glyph glyph, int size, QWidget* parent)
    : QPushButton(parent), glyph_(glyph), size_(size) {
    idleColor_ = Theme::instance().color(QStringLiteral("ink-2"));
    hoverColor_ = Theme::instance().color(QStringLiteral("ink"));

    setAttribute(Qt::WA_Hover, true);
    setFocusPolicy(Qt::NoFocus);
    setFlat(true);
    setIconSize(QSize(size, size));
    render();
}

void WindowControlButton::setGlyph(Glyph glyph) {
    glyph_ = glyph;
    render();
}

void WindowControlButton::setHoverColor(const QColor& color) {
    hoverColor_ = color;
    render();
}

void WindowControlButton::render() {
    setIcon(icons::icon(glyph_, hover_ ? hoverColor_ : idleColor_, size_));
}

bool WindowControlButton::event(QEvent* event) {
    if (event->type() == QEvent::HoverEnter) {
        hover_ = true;
        render();
    } else if (event->type() == QEvent::HoverLeave) {
        hover_ = false;
        render();
    }
    return QPushButton::event(event);
}

}
