#pragma once

#include "ui/widgets/UiFactory.h"

#include <QWidget>

class QMouseEvent;

namespace devdatapod {

// The design has no native title bar: 46px tall, app identity left, window controls right.
class TitleBar : public QWidget {
    Q_OBJECT
public:
    explicit TitleBar(QWidget* parent = nullptr);

    void setMaximized(bool maximized);

signals:
    void minimizeRequested();
    void toggleMaximizeRequested();
    void closeRequested();

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    WindowControlButton* makeControl(Glyph glyph, const char* objectName);
    WindowControlButton* maximizeButton_ = nullptr;
};

}
