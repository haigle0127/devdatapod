#include "core/service/ServiceKind.h"

#include <algorithm>
#include <iterator>

namespace devdatapod::kinds {

namespace {

// A kind may need more than one port (nginx listens on http and https); only the primary
// band is described here. The second one is requested by name from PortAllocator, which
// keeps every number in ports.json.
const Meta kMetas[] = {
    {"mysql", "MySQL", "mysql", "mysql"},
    {"mariadb", "MariaDB", "mariadb", "mariadb"},
    {"redis", "Redis", "redis", "redis"},
    {"nginx", "Nginx", "nginx", "http"},
    {"php", "PHP", "phpFpm", ""},
    {"python", "Python", "", ""},
};

}

const Meta* find(const QString& key) {
    const auto it = std::find_if(std::begin(kMetas), std::end(kMetas),
                                 [&](const Meta& m) { return m.key == key; });
    return it == std::end(kMetas) ? nullptr : &(*it);
}

QVector<Meta> all() {
    QVector<Meta> out;
    out.reserve(static_cast<qsizetype>(std::size(kMetas)));
    for (const Meta& meta : kMetas) out.append(meta);
    return out;
}

QString label(const QString& key) {
    const Meta* meta = find(key);
    return meta ? meta->label : key;
}

bool ownsPort(const QString& key) {
    const Meta* meta = find(key);
    return meta && !meta->slot.isEmpty();
}

}
