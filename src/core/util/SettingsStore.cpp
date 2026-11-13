#include "core/util/SettingsStore.h"

#include "core/util/AppPaths.h"

#include <QDir>
#include <QFileInfo>

namespace devdatapod {

SettingsStore& SettingsStore::instance() {
    static SettingsStore only;
    return only;
}

SettingsStore::SettingsStore()
    : ini_(QDir(paths::dataDir()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat) {}

QString SettingsStore::filePath() const { return ini_.fileName(); }

QVariant SettingsStore::value(const QString& key, const QVariant& fallback) const {
    return ini_.value(key, fallback);
}

void SettingsStore::setValue(const QString& key, const QVariant& value) {
    ini_.setValue(key, value);
}

void SettingsStore::sync() { ini_.sync(); }

}
