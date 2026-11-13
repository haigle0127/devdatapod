#pragma once

#include "ui/widgets/ServiceRow.h"

#include <QString>
#include <QVector>
#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QTimer;
class QVBoxLayout;

namespace devdatapod {

class GaugeGroup;
class MeterBar;

namespace sys {
struct Snapshot;
}

// 首页 static restoration of "Screen - 首页" in design/devconsole.pen.
// 套件 rows come from Engine::rows() (role, port and state are real, docs/ROADMAP.md P2)；
// 底部「运行状态」的四个仪表（CPU / 内存 / 两块磁盘）由 setSystemStats 按轮询喂真实数据。
class HomePage : public QWidget {
    Q_OBJECT
public:
    explicit HomePage(const QString& announcement,
                      const QVector<ServiceRowInfo>& services,
                      const QString& version,
                      QWidget* parent = nullptr);

    // Called on every poll. Reuses the existing rows when the instance set is unchanged so
    // a switch does not flicker or lose button focus mid-animation.
    void setServices(const QVector<ServiceRowInfo>& services);

    // 底部仪表：CPU 需要两次采样才有差值，hasCpuBaseline=false 时先画「--」。
    // 左侧「服务器状态」两个圆环 + 右侧「处理器与内存」详情卡一起喂。
    void setSystemStats(const sys::Snapshot& snapshot, bool hasCpuBaseline);

    // 「资源信息」页里的动态计数：实例数（每次轮询）与站点数。静态信息建卡时取一次。
    void setResourceCounts(int instanceCount, int siteCount);

signals:
    void servicePrimaryRequested(const QString& id);
    void serviceRestartRequested(const QString& id);
    void serviceConfigureRequested(const QString& id);
    void servicePromoteRequested(const QString& id);

private:
    QWidget* buildAnnouncement(const QString& text, QWidget* parent);
    QWidget* buildSuite(const QVector<ServiceRowInfo>& services, QWidget* parent);
    QWidget* buildBottomSection(QWidget* parent);
    QWidget* buildResourceInfoPage(QWidget* parent);
    QWidget* buildLogPage(QWidget* parent);
    void refreshLogView();
    void replaceRows(const QVector<ServiceRowInfo>& services);

    QVBoxLayout* suiteLayout_ = nullptr;
    QLabel* suiteTotal_ = nullptr;
    QLabel* suiteRunning_ = nullptr;
    QVector<ServiceRow*> rows_;
    GaugeGroup* serverGauges_ = nullptr;    // 左侧：CPU / 内存 两个圆环
    // 右侧「处理器与内存」详情卡（替代原来的磁盘圆环）：CPU / 内存横向仪表 + 拓扑信息。
    MeterBar* cpuBar_ = nullptr;
    MeterBar* memoryBar_ = nullptr;
    QLabel* cpuModelLabel_ = nullptr;
    QLabel* coreThreadLabel_ = nullptr;
    QLabel* cpuPercentLabel_ = nullptr;
    QLabel* memoryPercentLabel_ = nullptr;
    // 「资源信息」页的标签：静态的在建页时填，动态的三个走轮询。
    QLabel* memoryDetailKv_ = nullptr;      // 物理内存（可用量，3 分钟随统计刷新）
    QLabel* uptimeKv_ = nullptr;            // 运行时长（同上）
    QLabel* countsKv_ = nullptr;            // 实例 · 站点（2 秒轮询）
    // 「日志文件」页的 tail 视图与它的 2 秒刷新定时器（不可见时不读盘）。
    QPlainTextEdit* logView_ = nullptr;
    QTimer* logTimer_ = nullptr;
    QString lastLogText_;    // 内容没变就不重绘，免得滚动位置抖
    QString version_;
};

}
