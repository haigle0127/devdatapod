#pragma once

#include <QByteArray>
#include <QString>

namespace devdatapod::file {

// Write-then-replace, used for every json the panel owns (ports.json, services.json).
// A crash mid-write must not leave a half-written file that the next launch reads as
// "two instances claim 3306".
bool writeAtomically(const QString& path, const QByteArray& data);

// Read a whole file, or return false with the reason in *error.
bool readAll(const QString& path, QByteArray* data, QString* error = nullptr);

}
