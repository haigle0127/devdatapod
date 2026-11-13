#pragma once

#include "ui/theme/IconFactory.h"

#include <QPushButton>
#include <QWidget>

class QButtonGroup;

namespace devdatapod {

// 216px dark rail: brand block, the six sections from the design, promo card pinned low.
class Sidebar : public QWidget {
    Q_OBJECT
public:
    explicit Sidebar(QWidget* parent = nullptr);

    void setCurrentPage(int index);

signals:
    void pageSelected(int index);

private:
    QPushButton* addButton(const QString& text, Glyph glyph);

    QButtonGroup* group_ = nullptr;
};

}
