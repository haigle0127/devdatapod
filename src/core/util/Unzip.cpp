#include "core/util/Unzip.h"

#include "core/process/RunOnce.h"

#include <QDir>
#include <QFileInfo>
#include <QDirIterator>

namespace devdatapod::unpack {

namespace {

// 系统自带 bsdtar 的绝对路径。不依赖 PATH（Git 的 tar 也在 PATH 里，但它不认 zip）。
QString systemTar() {
    return QStringLiteral("C:/Windows/System32/tar.exe");
}

// 在 root 下递归找 serverBinary，返回它所在的目录；找不到返回空串。
QString findServerDir(const QString& root, const QString& serverBinary) {
    QDirIterator it(root, {serverBinary}, QDir::Files, QDirIterator::Subdirectories);
    if (!it.hasNext()) return QString();
    return QFileInfo(it.next()).absolutePath();
}

// 把 srcDir 里的所有内容（文件 + 子目录）搬进 destDir。destDir 已存在。
bool moveContents(const QString& srcDir, const QString& destDir, QString* error) {
    QDir src(srcDir);
    QDir dest(destDir);
    const QFileInfoList entries = src.entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries);
    for (const QFileInfo& entry : entries) {
        const QString target = dest.absoluteFilePath(entry.fileName());
        // 目标已存在就先删掉，保证摊平后是干净的。
        if (QFileInfo::exists(target)) {
            if (entry.isDir()) {
                QDir(target).removeRecursively();
            } else {
                QFile::remove(target);
            }
        }
        if (!QFile::rename(entry.absoluteFilePath(), target)) {
            if (error) *error = QStringLiteral("无法移动 %1 到 %2").arg(entry.fileName(), target);
            return false;
        }
    }
    return true;
}

} // namespace

bool extract(const QString& archivePath, const QString& destDir,
             const QString& serverBinary, QString* error) {
    if (!QFileInfo::exists(archivePath)) {
        if (error) *error = QStringLiteral("压缩包不存在：%1").arg(archivePath);
        return false;
    }

    // 先解到 destDir 旁边的临时目录，确认能解出 serverBinary 再摊平，避免半成品
    // 直接落进 Extensions。
    const QString tempDir = destDir + QStringLiteral(".unpacking");
    QDir(tempDir).removeRecursively();
    if (!QDir().mkpath(tempDir)) {
        if (error) *error = QStringLiteral("无法创建临时目录 %1").arg(tempDir);
        return false;
    }

    // bsdtar 自动识别 zip / tar.gz，不需要分参数。-x 解开，-C 指定落点。
    const proc::CommandResult result =
        proc::runOnce(systemTar(), {QStringLiteral("-xf"), archivePath, QStringLiteral("-C"), tempDir},
                      120000);
    if (!result.ran || result.exitCode != 0) {
        if (error) *error = QStringLiteral("解压失败：%1").arg(result.errorOutput.isEmpty()
                                                                   ? result.output
                                                                   : result.errorOutput);
        QDir(tempDir).removeRecursively();
        return false;
    }

    // 递归定位 serverBinary 所在的目录（不同包壳层数不一样）。
    QString serverDir = findServerDir(tempDir, serverBinary);
    if (serverDir.isEmpty()) {
        if (error) *error = QStringLiteral("解压后没找到 %1").arg(serverBinary);
        QDir(tempDir).removeRecursively();
        return false;
    }

    // MySQL 官方包的布局是 <壳>/bin/mysqld.exe + <壳>/share/errmsg.sys：只摊平 bin/
    // 会丢掉 share/，mysqld 初始化时找不到错误消息文件直接拒启。二进制在 bin/ 目录
    // 里的包一律摊平其父目录（整个包壳），redis / nginx 的二进制不在 bin/，不受影响。
    if (QDir(serverDir).dirName().compare(QStringLiteral("bin"), Qt::CaseInsensitive) == 0) {
        serverDir = QFileInfo(serverDir).absolutePath();
    }

    // 摊平到 destDir。
    QDir(destDir).removeRecursively();
    if (!QDir().mkpath(destDir)) {
        if (error) *error = QStringLiteral("无法创建安装目录 %1").arg(destDir);
        QDir(tempDir).removeRecursively();
        return false;
    }
    if (!moveContents(serverDir, destDir, error)) {
        QDir(tempDir).removeRecursively();
        return false;
    }

    QDir(tempDir).removeRecursively();
    return true;
}

bool extractRedis(const QString& archivePath, const QString& destDir, QString* error) {
    return extract(archivePath, destDir, QStringLiteral("redis-server.exe"), error);
}

} // namespace devdatapod::unpack
