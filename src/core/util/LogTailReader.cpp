#include "core/util/LogTailReader.h"

#include <QFile>
#include <QFileInfo>
#include <QStringList>

#include <string>
#include <vector>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace devdatapod::text {

bool looksLikeUtf8(const QByteArray& data) {
    auto at = [&data](qsizetype i) { return static_cast<unsigned char>(data.at(i)); };
    const qsizetype size = data.size();
    for (qsizetype i = 0; i < size;) {
        const unsigned char lead = at(i);
        if (lead < 0x80) {
            ++i;
            continue;
        }
        int extra = 0;
        unsigned int code = 0;
        if ((lead & 0xE0) == 0xC0) {
            extra = 1;
            code = lead & 0x1Fu;
            if (code < 2) return false;              // overlong
        } else if ((lead & 0xF0) == 0xE0) {
            extra = 2;
            code = lead & 0x0Fu;
        } else if ((lead & 0xF8) == 0xF0) {
            extra = 3;
            code = lead & 0x07u;
        } else {
            return false;                            // invalid lead byte
        }
        if (i + extra >= size) return false;
        for (int k = 1; k <= extra; ++k) {
            const unsigned char follow = at(i + k);
            if ((follow & 0xC0) != 0x80) return false;
            code = (code << 6) | (follow & 0x3Fu);
        }
        if (extra == 2 && code < 0x800) return false;         // overlong
        if (extra == 3 && code < 0x10000) return false;       // overlong
        if (code > 0x10FFFFu) return false;
        if (code >= 0xD800u && code <= 0xDFFFu) return false;  // surrogates never appear in UTF-8
        i += extra + 1;
    }
    return true;
}

QString decode(const QByteArray& data, QString* encoding) {
    if (looksLikeUtf8(data)) {
        if (encoding) *encoding = QStringLiteral("UTF-8");
        return QString::fromUtf8(data);
    }
#ifdef Q_OS_WIN
    // Not valid UTF-8, so on a Chinese Windows it is almost certainly the ANSI code page
    // (936 = GBK). Going through the OS avoids depending on Qt5Compat's QTextCodec.
    const UINT acp = GetACP();
    if (encoding) *encoding = QStringLiteral("CP%1").arg(acp);
    const int needed = MultiByteToWideChar(acp, 0, data.constData(),
                                          static_cast<int>(data.size()), nullptr, 0);
    if (needed > 0) {
        std::u16string buffer(static_cast<size_t>(needed), u'\0');
        const int written = MultiByteToWideChar(acp, 0, data.constData(),
                                               static_cast<int>(data.size()),
                                               reinterpret_cast<LPWSTR>(&buffer[0]), needed);
        if (written > 0) {
            buffer.resize(static_cast<size_t>(written));
            return QString::fromUtf16(buffer.data(), static_cast<qsizetype>(buffer.size()));
        }
    }
#else
    // macOS services log UTF-8; nothing in the portable component set emits another codepage.
    if (encoding) *encoding = QStringLiteral("UTF-8 (best effort)");
#endif
    return QString::fromUtf8(data);
}

Tail readTail(const QString& path, int lines, qint64 maxBytes) {
    Tail tail;
    tail.path = path;
    QFile file(path);
    if (!QFileInfo::exists(path)) {
        tail.encoding = QStringLiteral("UTF-8");
        return tail;    // no log yet is normal before the first start
    }
    if (!file.open(QIODevice::ReadOnly)) {
        tail.text = QStringLiteral("<cannot open %1: %2>").arg(path, file.errorString());
        tail.encoding = QStringLiteral("UTF-8");
        return tail;
    }

    const qint64 size = file.size();
    const qint64 start = qMax<qint64>(0, size - qMax<qint64>(1024, maxBytes));
    if (start > 0) tail.truncated = true;
    if (start > 0 && !file.seek(start)) {
        tail.text = QStringLiteral("<cannot seek %1>").arg(path);
        return tail;
    }
    QByteArray raw = file.read(qMax<qint64>(1024, maxBytes));

    // The window opened mid-line: the first fragment belongs to a line we only have half
    // of, so drop it rather than show a line that starts with tail-end bytes.
    if (start > 0) {
        const qsizetype newline = raw.indexOf('\n');
        if (newline >= 0) raw = raw.mid(newline + 1);
    }

    QString decoded = decode(raw, &tail.encoding);
    QStringList rows = decoded.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (QString& row : rows) {
        if (row.endsWith(QLatin1Char('\r'))) row.chop(1);
    }
    if (lines > 0 && rows.size() > lines) rows = rows.mid(rows.size() - lines);
    tail.text = rows.join(QLatin1Char('\n'));
    return tail;
}

}
