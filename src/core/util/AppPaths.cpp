#include "core/util/AppPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

namespace devdatapod::paths {

namespace {

QString resolveInstallDir() {
    const QString override = QString::fromLocal8Bit(qgetenv("DEVDATAPOD_ROOT"));
    if (!override.isEmpty() && QFileInfo::exists(override)) {
        return QDir(override).absolutePath();
    }

    // Shipped layout is <root>/bin/devdatapod(.app). Anything else (dev build tree,
    // running from a temp dir) keeps the exe directory as the root.
    QDir exeDir = QCoreApplication::applicationDirPath();
    if (exeDir.dirName().compare(QStringLiteral("bin"), Qt::CaseInsensitive) == 0) {
        exeDir.cdUp();
    }
    return exeDir.absolutePath();
}

QString sub(const QString& part) {
    return QDir(installDir()).filePath(part);
}

}

QString installDir() {
    static const QString cached = resolveInstallDir();
    return cached;
}

QString dataDir() { return sub(QStringLiteral("data")); }
QString logsDir() { return sub(QStringLiteral("logs")); }
QString extensionsDir() { return sub(QStringLiteral("Extensions")); }
QString wwwDir() { return sub(QStringLiteral("WWW")); }

bool ensureWritableDirs() {
    QDir dir;
    return dir.mkpath(dataDir()) && dir.mkpath(logsDir());
}

}
