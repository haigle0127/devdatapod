#pragma once

#include "core/service/ComponentCatalog.h"

#include <QHash>
#include <QWidget>

class QVBoxLayout;
class QLabel;
class QPushButton;

namespace devdatapod {

class Engine;

// 软件管理页：列出可下载的组件（当前是四个 redis 版本），点「下载」走
// 下载 → 解压 → 注册 一条龙，装好后该实例出现在首页套件列表里。
//
// 页本身只负责渲染和转发按钮点击；真正的下载/解压/注册在 InstallOperator（工作线程）。
// 已装/未装状态靠 Engine 的注册表判断：kind+version 已注册就是「已安装」。
class SoftwarePage : public QWidget {
    Q_OBJECT
public:
    explicit SoftwarePage(Engine* engine, QWidget* parent = nullptr);

    // 安装进行中/结束时由 MainWindow 调进来更新那一行。
    void setRowStage(const QString& componentId, const QString& stage);
    void setRowProgress(const QString& componentId, qint64 received, qint64 total);
    void setRowFinished(const QString& componentId, bool ok, const QString& error);
    // 卸载成功后整页重建，把「已安装」变回「下载」。
    void reload();

signals:
    // 点了某一行的「下载」。
    void installRequested(const devdatapod::Component& component);
    // 点了某一行的「卸载」。
    void uninstallRequested(const devdatapod::Component& component);

private:
    void rebuild();

    Engine* engine_;
    QVBoxLayout* listLayout_ = nullptr;
    // componentId -> 那一行的状态标签，用来显示「下载中 45%」之类。
    QHash<QString, QLabel*> stageLabels_;
    QHash<QString, QPushButton*> buttons_;
};

} // namespace devdatapod
