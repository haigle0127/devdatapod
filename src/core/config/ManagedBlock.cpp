#include "core/config/ManagedBlock.h"

#include "core/util/AtomicFile.h"
#include "core/util/Log.h"

#include <QByteArray>
#include <QFileInfo>
#include <QList>

namespace devdatapod::config::managed {

namespace {

QByteArray bodyAsBytes(const QString& body) { return body.toUtf8(); }

qsizetype findMarker(const QList<QByteArray>& lines, const QByteArray& marker, qsizetype from) {
    const QByteArray wanted = marker.trimmed();
    for (qsizetype i = from; i < lines.size(); ++i) {
        if (lines.at(i).trimmed() == wanted) return i;
    }
    return -1;
}

}

BlockOutcome apply(const QString& path, const QString& body, QString* error) {
    const QByteArray begin = QByteArray(kBeginMarker);
    const QByteArray end = QByteArray(kEndMarker);
    const bool exists = QFileInfo::exists(path);

    if (!exists) {
        QByteArray data;
        data += "# written by devdatapod. Everything outside the managed block below is\n";
        data += "# yours: the panel only ever replaces the lines between the markers.\n";
        data += begin + "\n";
        data += bodyAsBytes(body);
        if (!data.endsWith('\n')) data += "\n";
        data += end + "\n";
        if (!file::writeAtomically(path, data)) {
            if (error) *error = QStringLiteral("cannot create %1").arg(path);
            return BlockOutcome::Failed;
        }
        return BlockOutcome::Written;
    }

    // Byte level on purpose. Decoding the user's file to QString and back would quietly
    // normalise an encoding or a line ending we were never asked to touch.
    QByteArray raw;
    if (!file::readAll(path, &raw, error)) return BlockOutcome::Failed;

    const bool crlf = raw.contains("\r\n");
    QList<QByteArray> lines = raw.split('\n');
    const qsizetype beginIndex = findMarker(lines, begin, 0);
    if (beginIndex < 0) {
        log::info(QStringLiteral("config: %1 has no devdatapod block, leaving it alone").arg(path));
        return BlockOutcome::Preserved;
    }
    const qsizetype endIndex = findMarker(lines, end, beginIndex + 1);
    if (endIndex < 0) {
        if (error) {
            *error = QStringLiteral("%1 has the opening marker at line %2 but no closing one; "
                                    "refusing to guess where the block ends")
                         .arg(path)
                         .arg(beginIndex + 1);
        }
        return BlockOutcome::Failed;
    }

    QByteArray inserted = bodyAsBytes(body);
    if (!inserted.isEmpty() && !inserted.endsWith('\n')) inserted += "\n";
    QList<QByteArray> middle = inserted.split('\n');
    if (!middle.isEmpty() && middle.last().isEmpty()) middle.removeLast();
    if (crlf) {
        for (QByteArray& line : middle) line += '\r';
    }

    QList<QByteArray> rebuilt;
    rebuilt.reserve(lines.size() + middle.size() + 2);
    for (qsizetype i = 0; i < beginIndex; ++i) rebuilt.append(lines.at(i));
    rebuilt.append(crlf ? begin + "\r" : begin);
    rebuilt.append(middle);
    rebuilt.append(crlf ? end + "\r" : end);
    for (qsizetype i = endIndex + 1; i < lines.size(); ++i) rebuilt.append(lines.at(i));

    QByteArray data = rebuilt.join('\n');
    if (!data.endsWith('\n')) data += "\n";
    if (!file::writeAtomically(path, data)) {
        if (error) *error = QStringLiteral("cannot replace the block in %1").arg(path);
        return BlockOutcome::Failed;
    }
    return BlockOutcome::Written;
}

bool hasBlock(const QString& path) {
    QByteArray raw;
    if (!file::readAll(path, &raw, nullptr)) return false;
    const QList<QByteArray> lines = raw.split('\n');
    return findMarker(lines, QByteArray(kBeginMarker), 0) >= 0;
}

}
