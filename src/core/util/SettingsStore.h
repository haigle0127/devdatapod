#pragma once

#include <QSettings>
#include <QString>
#include <QVariant>

namespace devdatapod {

// data/settings.ini, next to the executable - never the registry, never %APPDATA%.
class SettingsStore {
public:
    static SettingsStore& instance();

    QString filePath() const;
    QVariant value(const QString& key, const QVariant& fallback = QVariant()) const;
    void setValue(const QString& key, const QVariant& value);
    void sync();

private:
    SettingsStore();
    QSettings ini_;
};

}
