#include "core/net/Downloader.h"

#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace devdatapod {

Downloader::Downloader(QObject* parent) : QObject(parent) {
    nam_ = new QNetworkAccessManager(this);
}

void Downloader::start(const QString& url, const QString& destPath) {
    cancel();
    destPath_ = destPath;
    partPath_ = destPath + QStringLiteral(".part");

    // 旧的 .part 可能是上次中断留下的，先删掉再开新文件。
    QFile::remove(partPath_);
    file_ = new QFile(partPath_, this);
    if (!file_->open(QIODevice::WriteOnly)) {
        fail(QStringLiteral("无法写入 %1").arg(partPath_));
        return;
    }

    QNetworkRequest request{QUrl(url)};
    // 直连源站（github.com / cdn.mysql.com / nginx.org）；Qt 默认跟重定向，不用额外处理。
    reply_ = nam_->get(request);
    connect(reply_, &QNetworkReply::readyRead, this, &Downloader::onReadyRead);
    connect(reply_, &QNetworkReply::finished, this, &Downloader::onFinished);
    connect(reply_, &QNetworkReply::downloadProgress, this, &Downloader::progress);
}

void Downloader::cancel() {
    if (!reply_) return;
    reply_->disconnect(this);
    reply_->abort();
    reply_->deleteLater();
    reply_ = nullptr;
    if (file_) {
        file_->close();
        file_->deleteLater();
        file_ = nullptr;
    }
    QFile::remove(partPath_);
}

void Downloader::onReadyRead() {
    if (file_ && reply_) file_->write(reply_->readAll());
}

void Downloader::onFinished() {
    if (!reply_) return;
    const QNetworkReply::NetworkError code = reply_->error();
    const QString reason = reply_->errorString();
    reply_->deleteLater();
    reply_ = nullptr;

    if (code != QNetworkReply::NoError) {
        fail(QStringLiteral("下载失败：%1").arg(reason));
        return;
    }

    file_->flush();
    file_->close();
    file_->deleteLater();
    file_ = nullptr;

    // 下完了，把 .part 改成正式名。目标已存在（比如重复下载）就先删。
    QFile::remove(destPath_);
    if (!QFile::rename(partPath_, destPath_)) {
        fail(QStringLiteral("无法保存到 %1").arg(destPath_));
        return;
    }
    emit finished(true, destPath_, QString());
}

void Downloader::fail(const QString& error) {
    if (file_) {
        file_->close();
        file_->deleteLater();
        file_ = nullptr;
    }
    QFile::remove(partPath_);
    emit finished(false, QString(), error);
}

} // namespace devdatapod
