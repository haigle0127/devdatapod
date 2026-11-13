#include "core/service/ServiceInstance.h"

#include "core/service/ServiceKind.h"

namespace devdatapod {

QString ServiceInstance::makeId(const QString& kind, const QString& version) {
    return kind + QLatin1Char('-') + version;
}

QString ServiceInstance::makePortKey(const QString& kind, const QString& version) {
    return kind + QLatin1Char('.') + version;
}

QString ServiceInstance::makeDataDir(const QString& kind, const QString& version) {
    return QStringLiteral("data/instances/%1/%2").arg(kind, version);
}

QString ServiceInstance::displayName() const {
    return QStringLiteral("%1 %2").arg(kinds::label(kind), version);
}

QString ServiceInstance::roleLabel() const {
    return isDefault ? QStringLiteral("默认") : QStringLiteral("备用");
}

}
