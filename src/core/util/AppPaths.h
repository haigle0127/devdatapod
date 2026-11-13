#pragma once

#include <QString>

namespace devdatapod::paths {

// Green install root: <root>/bin, <root>/data, <root>/logs, <root>/Extensions.
// Resolved from the executable location, overridable with DEVDATAPOD_ROOT for dev runs.
QString installDir();
QString dataDir();
QString logsDir();
QString extensionsDir();
QString wwwDir();

// Creates data/ and logs/ if missing. Returns false and logs when the root is read-only.
bool ensureWritableDirs();

}
