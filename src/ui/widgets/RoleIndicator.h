#pragma once

#include <QLabel>

namespace devdatapod {

// 「是否为默认」的图标指示器：default 显示 default-on.svg（不可点），
// 备用显示 default-off.svg，点击触发「设为默认」。
// 图标是 SVG，矢量缩放不糊，文件在 src/ui/theme/icons/。
class RoleIndicator : public QLabel {
    Q_OBJECT
public:
    explicit RoleIndicator(QWidget* parent = nullptr);

    // 设置是否为默认 + 是否可点（备用且可提升时才可点）。
    void setRole(bool isDefault, bool clickable);

signals:
    void clicked();

protected:
    void mousePressEvent(QMouseEvent* event) override;

private:
    void render();

    bool isDefault_ = false;
    bool clickable_ = false;
};

}
