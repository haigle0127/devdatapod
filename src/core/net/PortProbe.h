#pragma once

#include <QString>

#include <cstdint>

namespace devdatapod::net {

// Answering "is 3306 free?" is not enough: rule 3 of docs/PORT-PLAN.md says an external
// holder must be reported by name and PID, otherwise the user is told "port busy" with no
// way out. On this machine 宝塔's mysqld really does sit on 3306.
struct PortOccupier {
    quint16 port = 0;
    bool occupied = false;
    std::uint64_t pid = 0;
    QString address;        // what the holder bound to
    QString processName;    // empty when the platform cannot tell
    QString executable;     // full path, when readable
};

// Cheap bind test on the loopback address. Qt asks for exclusive address usage, so a
// wildcard listener somewhere else in the system also shows up as "not free".
bool isPortFree(quint16 port, const QString& address = QStringLiteral("127.0.0.1"));

// Requires a real system table lookup, hence the separate call: the UI only pays for it
// after isPortFree() already said "occupied".
PortOccupier identifyPortOwner(quint16 port);

// 全系统扫描：正在监听、且可执行文件路径落在「别的 devdatapod 安装目录」下的进程。
// 本安装（ownRoot）自己的进程不算——用于新旧两个安装抢端口时的和平接管：
// 面板启动发现规范端口被占，且占位者路径里带 devdatapod、又不属于本安装，
// 那就是另一个安装的残留/在跑实例，可以征得用户同意后停掉。
QVector<PortOccupier> siblingPanelListeners(const QString& ownRoot);

}
