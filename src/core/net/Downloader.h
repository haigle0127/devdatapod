#pragma once

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class QFile;

namespace devdatapod {

// 单文件下载器：把 url 下载到 destPath。先写 destPath + ".part"，下完再原子改名，
// 避免半成品被当成完整文件。进度通过信号抛给 UI。
//
// 设计给「软件管理页点下载」用：在 UI 线程创建，事件驱动，不阻塞。一次只下一个；
// 要下第二个就再 new 一个（软件管理页会排队）。
class Downloader : public QObject {
    Q_OBJECT
public:
    explicit Downloader(QObject* parent = nullptr);

    // 开始下载。destPath 是最终落盘路径（含文件名）。重复调用会先取消上一个。
    void start(const QString& url, const QString& destPath);
    void cancel();

    bool isRunning() const { return reply_ != nullptr; }

signals:
    // received/total 字节数；total 未知时是 -1。
    void progress(qint64 received, qint64 total);
    // 成功：path 是最终文件路径。失败：error 非空。
    void finished(bool ok, const QString& path, const QString& error);

private slots:
    void onReadyRead();
    void onFinished();

private:
    void fail(const QString& error);

    QNetworkAccessManager* nam_ = nullptr;
    QNetworkReply* reply_ = nullptr;
    QFile* file_ = nullptr;
    QString destPath_;
    QString partPath_;
};

} // namespace devdatapod
