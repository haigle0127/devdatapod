#include "ui/InstallOperator.h"

#include "core/net/Downloader.h"
#include "core/service/Engine.h"
#include "core/service/ServiceInstance.h"
#include "core/util/AppPaths.h"
#include "core/util/Log.h"
#include "core/util/Unzip.h"

#include <QDir>
#include <QEventLoop>
#include <QTimer>

namespace devdatapod {

InstallOperator::InstallOperator(Engine* engine, QObject* parent)
    : QObject(parent), engine_(engine) {}

void InstallOperator::install(const Component& component) {
    const QString id = component.id;

    // 下载到 data/downloads/<fileName>，解压到 Extensions/<kind><version>/。
    const QString downloadDir = paths::dataDir() + QStringLiteral("/downloads");
    QDir().mkpath(downloadDir);
    const QString archivePath = downloadDir + QLatin1Char('/') + component.fileName;
    const QString destDir =
        paths::extensionsDir() + QLatin1Char('/') + component.kind + component.version;

    // 按 kind 选 server 二进制名，解压器靠它定位并摊平。
    QString serverBinary;
    if (component.kind == QLatin1String("redis")) {
        serverBinary = QStringLiteral("redis-server.exe");
    } else if (component.kind == QLatin1String("mysql")) {
        serverBinary = QStringLiteral("mysqld.exe");
    } else if (component.kind == QLatin1String("nginx")) {
        serverBinary = QStringLiteral("nginx.exe");
    } else {
        emit finished(id, false, QStringLiteral("未知组件类型 %1").arg(component.kind));
        return;
    }

    // 1) 下载
    emit stageChanged(id, QStringLiteral("下载中"));
    QString error;
    if (!download(component, archivePath, &error)) {
        emit finished(id, false, error);
        return;
    }

    // 2) 解压（摊平成 server 二进制直接在 destDir 下）
    emit stageChanged(id, QStringLiteral("解压中"));
    if (!unpack::extract(archivePath, destDir, serverBinary, &error)) {
        emit finished(id, false, error);
        return;
    }

    // 3) 注册实例。installPath 相对安装根目录。
    emit stageChanged(id, QStringLiteral("注册中"));
    ServiceInstance instance;
    instance.kind = component.kind;
    instance.version = component.version;
    instance.installPath = QStringLiteral("Extensions/") + component.kind + component.version;
    // id / portKey / dataDir 让 InstanceRegistry::add 自己补。
    if (!engine_->registry()->add(instance)) {
        emit finished(id, false, QStringLiteral("注册实例失败（可能已存在）"));
        return;
    }
    // 让 Engine 给新实例分配端口并刷新列表。
    QString syncError;
    if (!engine_->synchronize(&syncError)) {
        emit finished(id, false, QStringLiteral("端口分配失败：%1").arg(syncError));
        return;
    }

    log::info(QStringLiteral("组件 %1 安装完成，已注册并分配端口").arg(id));
    emit finished(id, true, QString());
    emit operationFinished();
}

bool InstallOperator::download(const Component& component, const QString& destPath,
                               QString* error) {
    // Downloader 是事件驱动的，得在有事件循环的线程里跑。这里在工作线程开一个临时
    // QEventLoop，把异步下载变成「阻塞等结果」，好和解压/注册串成一条直线。
    Downloader downloader;
    QEventLoop loop;
    bool ok = false;

    connect(&downloader, &Downloader::progress, this,
            [this, id = component.id](qint64 received, qint64 total) {
                emit downloadProgress(id, received, total);
            });
    connect(&downloader, &Downloader::finished, this,
            [&](bool success, const QString&, const QString& err) {
                ok = success;
                if (error) *error = err;
                loop.quit();
            });

    downloader.start(component.url, destPath);
    loop.exec();    // 等 finished 信号
    return ok;
}

void InstallOperator::uninstall(const Component& component) {
    const QString id = component.id;
    const QString instanceId = component.kind + QLatin1Char('-') + component.version;

    // 无论成败都发 operationFinished，让 UI 重建列表、解锁按钮。
    auto done = [this](const QString& cid, bool ok, const QString& err) {
        emit finished(cid, ok, err);
        emit operationFinished();
    };

    // 1) 找到实例并停掉
    ServiceInstance inst;
    if (!engine_->find(instanceId, &inst)) {
        done(id, false, QStringLiteral("实例不存在"));
        return;
    }

    emit stageChanged(id, QStringLiteral("停止中"));
    if (engine_->isRunning(instanceId)) {
        QString stopError;
        if (!engine_->stop(instanceId, &stopError)) {
            done(id, false, QStringLiteral("停止失败：%1").arg(stopError));
            return;
        }
    }

    // 2) 删掉缓存的 driver（释放 Supervisor / Job Object）
    engine_->invalidate(instanceId);

    // 3) 从注册表移除
    emit stageChanged(id, QStringLiteral("移除注册"));
    if (!engine_->registry()->remove(instanceId)) {
        done(id, false, QStringLiteral("移除注册失败"));
        return;
    }

    // 4) 刷新端口分配
    QString syncError;
    if (!engine_->synchronize(&syncError)) {
        done(id, false, QStringLiteral("端口同步失败：%1").arg(syncError));
        return;
    }

    // 5) 删 Extensions/<kind><version>/ 目录
    emit stageChanged(id, QStringLiteral("删除文件"));
    const QString extDir = paths::extensionsDir() + QLatin1Char('/') + component.kind
                           + component.version;
    if (QDir(extDir).exists() && !QDir(extDir).removeRecursively()) {
        done(id, false, QStringLiteral("删除 %1 失败").arg(extDir));
        return;
    }

    // 6) 删 data/instances/<kind>/<version>/ 目录
    const QString dataDir = paths::dataDir() + QStringLiteral("/instances/")
                            + component.kind + QLatin1Char('/') + component.version;
    if (QDir(dataDir).exists() && !QDir(dataDir).removeRecursively()) {
        done(id, false, QStringLiteral("删除 %1 失败").arg(dataDir));
        return;
    }

    log::info(QStringLiteral("组件 %1 已卸载").arg(id));
    done(id, true, QString());
}

} // namespace devdatapod
