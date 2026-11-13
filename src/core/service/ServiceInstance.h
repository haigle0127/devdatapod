#pragma once

#include <QString>

namespace devdatapod {

// The persistent half of an installed service version. Runtime state is not here: role is
// intent (data/services.json), state is observed (the driver). "default" does not mean
// "running" and "running" does not mean "default".
struct ServiceInstance {
    QString id;            // "mysql-8.0.12", unique across kinds
    QString kind;          // "mysql" | "mariadb" | "redis" | ... see ServiceKind.h
    QString version;       // "8.0.12"
    bool isDefault = false;
    QString portKey;       // key in ports.json; empty when the kind owns no port
    QString installPath;   // relative to the install root, e.g. Extensions/MySQL8.0.12
    QString dataDir;       // relative, one per instance, never shared

    bool hasPort() const { return !portKey.isEmpty(); }

    static QString makeId(const QString& kind, const QString& version);
    static QString makePortKey(const QString& kind, const QString& version);
    // data/instances/<kind>/<version>/ - MySQL 8 initialises a directory 5.7 cannot open,
    // so sharing one between instances is data loss waiting to happen.
    static QString makeDataDir(const QString& kind, const QString& version);

    // "MySQL 8.0.12" - the label from ServiceKind plus the version.
    QString displayName() const;
    // "默认" / "备用" for the badge next to it.
    QString roleLabel() const;
};

}
