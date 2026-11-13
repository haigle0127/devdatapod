#pragma once

#include <QString>

namespace devdatapod::log {

// Must be called after paths::ensureWritableDirs(). Safe to call twice.
bool init();
void shutdown();

// Routes qDebug/qWarning/qCritical into the same sink. Call once after init().
void installQtMessageHandler();

void info(const QString& message);
void warn(const QString& message);
void error(const QString& message);

}
