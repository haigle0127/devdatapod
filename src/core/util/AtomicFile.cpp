#include "core/util/AtomicFile.h"

#include "core/util/Log.h"

#include <QFile>
#include <QFileInfo>

namespace devdatapod::file {

bool readAll(const QString& path, QByteArray* data, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    *data = file.readAll();
    return true;
}

bool writeAtomically(const QString& path, const QByteArray& data) {
    const QString temp = path + QStringLiteral(".tmp");
    {
        QFile file(temp);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            log::error(QStringLiteral("cannot write %1: %2").arg(temp, file.errorString()));
            return false;
        }
        if (file.write(data) != data.size()) {
            log::error(QStringLiteral("short write to %1").arg(temp));
            file.close();
            QFile::remove(temp);
            return false;
        }
        if (!file.flush()) {
            log::error(QStringLiteral("cannot flush %1: %2").arg(temp, file.errorString()));
            file.close();
            QFile::remove(temp);
            return false;
        }
    }

    // QFile::rename refuses to replace an existing file on Windows. The gap between the
    // remove and the rename is sub-millisecond, and a crash there still leaves a complete
    // .tmp behind rather than a truncated live file.
    if (QFile::exists(path) && !QFile::remove(path)) {
        log::error(QStringLiteral("cannot replace %1").arg(path));
        QFile::remove(temp);
        return false;
    }
    if (!QFile::rename(temp, path)) {
        log::error(QStringLiteral("cannot move %1 over %2").arg(temp, path));
        return false;
    }
    return true;
}

}
