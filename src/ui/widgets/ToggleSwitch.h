#pragma once

#include <QWidget>

namespace devdatapod {

// iOS 风格的滑动开关：椭圆轨道 + 白色圆形滑块，开时滑块在右并带对勾。
// 用 Q_PROPERTY(qreal offset) 驱动滑动动画，paintEvent 自绘，颜色走 Theme token。
//
// 语义由调用方决定：在 ServiceRow 里它表示「是否为默认实例」。
// default 实例 setLocked(true) 锁定在开（灰色不可滑）；备用实例滑到开触发「设为默认」。
class ToggleSwitch : public QWidget {
    Q_OBJECT
    Q_PROPERTY(qreal offset READ offset WRITE setOffset)

public:
    // 滑块里画的图标，用来区分开关的用途（两个开关并排时不会分不清）。
    enum Icon {
        Checkmark,   // 对勾（默认，开时）
        Star,        // 星：「是否为默认」
        Power,       // 电源：「启动/停止」
        CheckCross   // 开 = 对勾，关 = 叉（启动/停止用，状态一眼可见）
    };

    explicit ToggleSwitch(QWidget* parent = nullptr);

    bool isOn() const { return on_; }
    void setOn(bool on, bool animate = true);

    void setIcon(Icon icon) { icon_ = icon; update(); }

    // 锁定后开关变灰且不可点（default 实例：总得有一个默认，不能滑回去）。
    void setLocked(bool locked);
    bool isLocked() const { return locked_; }

    QSize sizeHint() const override;

    // Q_PROPERTY 的读写方法（滑动动画用），不是信号。
    qreal offset() const { return offset_; }
    void setOffset(qreal value);

signals:
    // 用户滑动改变了状态（setOn 程序化设置不触发）。
    void toggled(bool on);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    void drawIcon(QPainter& p, const QPointF& center);

    bool on_ = false;
    bool locked_ = false;
    Icon icon_ = Checkmark;
    qreal offset_ = 0.0;    // 0.0 = 滑块在最左（关），1.0 = 最右（开）
};

}
