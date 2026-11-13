#pragma once

#include <QWidget>

namespace devdatapod {

// 圆角水平进度条，内存详情卡用。配色走 Theme token。
class MeterBar : public QWidget {
    Q_OBJECT
public:
    explicit MeterBar(QWidget* parent = nullptr);

    QSize sizeHint() const override;

    void setFraction(double fraction);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    double fraction_ = 0.0;
};

} // namespace devdatapod
