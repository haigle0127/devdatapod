#pragma once

#include "ui/theme/IconFactory.h"

#include <QColor>
#include <QPushButton>
#include <QWidget>

class QFrame;
class QLabel;

namespace devdatapod {

// Small shared builders so pages do not re-implement the same three widgets.
namespace ui {

// A #Card surface. Plain QWidgets ignore QSS backgrounds unless styled-background is
// requested explicitly, so makeCard() always sets the attribute.
QWidget* makeCard(QWidget* parent = nullptr);
QFrame* makeAccentBar(const QString& tone, int height, QWidget* parent = nullptr);
QLabel* makeLabel(const QString& text, const QString& objectName, QWidget* parent = nullptr);

}

// Title-bar control. QSS cannot retint an icon, so the idle/hover foreground pair is baked
// from tokens here while QSS keeps owning the shape and the hover background.
class WindowControlButton : public QPushButton {
    Q_OBJECT
public:
    WindowControlButton(Glyph glyph, int size, QWidget* parent = nullptr);

    void setGlyph(Glyph glyph);
    void setHoverColor(const QColor& color);

protected:
    bool event(QEvent* event) override;

private:
    void render();

    Glyph glyph_;
    int size_;
    bool hover_ = false;
    QColor idleColor_;
    QColor hoverColor_;
};

}
