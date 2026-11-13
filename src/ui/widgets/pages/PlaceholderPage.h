#pragma once

#include <QWidget>

namespace devdatapod {

// Sidebar entries with no page in this build. P1 only restores 首页; 软件管理 and 设置 do
// have designs and come next, while 网站 / 数据库 have none yet (see docs/DESIGN-SPEC.md).
class PlaceholderPage : public QWidget {
    Q_OBJECT
public:
    explicit PlaceholderPage(const QString& title, QWidget* parent = nullptr);
};

}
