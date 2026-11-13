#pragma once

#include "ui/widgets/StatusDot.h"

#include <QString>
#include <QWidget>

class QLabel;
class QPushButton;

namespace devdatapod {

class RoleIndicator;
class ToggleSwitch;

// One row of the 套件 list. Role and port are shown next to the version because with
// several versions of a kind installed, the version alone does not say who owns 3306.
struct ServiceRowInfo {
    QString id;       // the services.json instance id - what start/stop/promote act on
    QString name;
    QString role;   // "default" | "secondary"
    QString port;   // the port this instance actually listens on
    QString detail; // driver 的状态说明（崩溃原因等），崩溃时接管提示用
    bool promotable = false;    // secondary of a kind that has a default: 「设为默认」可用
    RunState state = RunState::Stopped;
};

class ServiceRow : public QWidget {
    Q_OBJECT
public:
    explicit ServiceRow(const ServiceRowInfo& info, QWidget* parent = nullptr);

    void setState(RunState state);
    RunState state() const { return state_; }
    // Role and port move when another instance is promoted, without rebuilding the row.
    void setInfo(const ServiceRowInfo& info);
    QString id() const { return id_; }

signals:
    void primaryRequested();
    void restartRequested();
    void configureRequested();
    void promoteRequested();

private:
    QString id_;
    RunState state_ = RunState::Stopped;
    QLabel* dot_ = nullptr;
    QLabel* portLabel_ = nullptr;
    RoleIndicator* roleIndicator_ = nullptr;
    ToggleSwitch* runSwitch_ = nullptr;
    QPushButton* restartButton_ = nullptr;
};

}
