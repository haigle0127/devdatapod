#pragma once

#include "core/service/ComponentCatalog.h"

#include <QObject>
#include <QString>

namespace devdatapod {

class Engine;

// 安装编排：下载 → 解压 → 注册实例。跑在专用工作线程里（和 ServiceOperator 一个套路），
// 解压大文件不卡 UI。下载本身是事件驱动的，得在 UI 线程做，所以这里把「下载」这一步
// 通过阻塞等待事件循环的方式串起来——工作线程里跑一个临时 QEventLoop 等下载器信号。
class InstallOperator : public QObject {
    Q_OBJECT
public:
    explicit InstallOperator(Engine* engine, QObject* parent = nullptr);

public slots:
    // 主入口：装 component 指向的组件。整个流程跑完发 finished。
    void install(const devdatapod::Component& component);
    // 卸载：停服务 → 删注册 → 删 Extensions 目录 → 删 datadir。
    void uninstall(const devdatapod::Component& component);

signals:
    // 阶段提示：「下载中」「解压中」「注册中」，给 UI 状态行用。
    void stageChanged(const QString& componentId, const QString& stage);
    // 仅下载阶段有效：received/total 字节。
    void downloadProgress(const QString& componentId, qint64 received, qint64 total);
    // 全部结束。ok=false 时 error 是原因。
    void finished(const QString& componentId, bool ok, const QString& error);
    // 安装或卸载结束（无论成败），UI 收到后重建列表，刷新按钮状态。
    void operationFinished();

private:
    // 在工作线程里同步跑下载（内部开一个 QEventLoop 等 Downloader 完成）。
    bool download(const devdatapod::Component& component, const QString& destPath, QString* error);

    devdatapod::Engine* engine_;
};

} // namespace devdatapod
