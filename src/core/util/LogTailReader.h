#pragma once

#include <QByteArray>
#include <QString>

namespace devdatapod::text {

// Decoded slice of a service log. Shared by every driver's logs() implementation so the
// quirks are fixed once: the window must start on a line boundary, and a Chinese Windows
// redis writes GBK.
struct Tail {
    QString text;
    QString encoding;      // "UTF-8" | "CP936" | ... what it actually decoded as
    QString path;
    bool truncated = false;  // the file is larger than the window that was read
};

// Last `lines` lines of `path`, reading at most `maxBytes` from the end.
Tail readTail(const QString& path, int lines, qint64 maxBytes = 256 * 1024);

// Strict UTF-8 well-formedness: rejects overlong forms, surrogate halves and >U+10FFFF,
// because QString::fromUtf8() silently maps all of those to U+FFFD and a log full of
// replacement boxes is what "it decoded" looks like when the guess was wrong.
bool looksLikeUtf8(const QByteArray& data);

// Decode with the best guess, filling *encoding with what was used.
QString decode(const QByteArray& data, QString* encoding = nullptr);

}
