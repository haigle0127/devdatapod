#pragma once

#include <QString>
#include <QVector>

namespace devdatapod::kinds {

// Static metadata about the service kinds devdatapod manages. Names only - every port number
// still comes from data/ports.json (AGENTS.md rule 2).
//
// mariadb and mysql are two kinds, deliberately not interchangeable: their data
// directories and handshake packets differ (docs/ARCHITECTURE.md 兼容性矩阵).
struct Meta {
    QString key;            // the spelling used in services.json / ports.json
    QString label;          // what the UI shows
    QString slot;           // band name in ports.json "slots", empty = no listening port
    QString canonical;      // band name in ports.json "canonical", empty = none
};

const Meta* find(const QString& key);
QVector<Meta> all();

// label for a known kind, the raw key for an unknown one (components can bring new kinds
// in without a rebuild).
QString label(const QString& key);
bool ownsPort(const QString& key);

}
