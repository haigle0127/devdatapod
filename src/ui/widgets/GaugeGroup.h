#pragma once

#include <QString>
#include <QVector>
#include <QWidget>

class QQuickWidget;

namespace devdatapod {

// One titled cluster of ring gauges. This is the only QML area in the app (AGENTS.md
// rule 4), and its colors come from the generated Tokens object, never from QSS.
class GaugeGroup : public QWidget {
    Q_OBJECT
public:
    struct Gauge {
        QString label;
        QString value;
        double fraction = 0.0;
        QString colorKey;   // camelCased .pen token, e.g. "accent2" for $accent-2
    };

    GaugeGroup(const QString& title,
               const QString& barTone,
               const QVector<Gauge>& gauges,
               QWidget* parent = nullptr);

    // 首页轮询喂新数据：整组替换 context 里的 model，QML 侧重绘圆环。
    void setGauges(const QVector<Gauge>& gauges);

private:
    QQuickWidget* view_ = nullptr;
};

}
