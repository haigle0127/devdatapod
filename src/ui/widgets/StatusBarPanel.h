#pragma once

#include "ui/widgets/StatusDot.h"

#include <QVector>
#include <QWidget>

class QHBoxLayout;
class QLabel;

namespace devdatapod {

struct ServiceStatusItem {
    QString name;
    RunState state = RunState::Stopped;
};

// 36px card pinned under the page: one dot+name chip per instance, version block at the right.
class StatusBarPanel : public QWidget {
    Q_OBJECT
public:
    explicit StatusBarPanel(const QString& version, QWidget* parent = nullptr);

    void setServices(const QVector<ServiceStatusItem>& items);

private:
    QHBoxLayout* chipRow_ = nullptr;
    QLabel* versionLabel_ = nullptr;
};

}
