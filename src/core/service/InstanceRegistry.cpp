#include "core/service/InstanceRegistry.h"

#include "core/service/ServiceKind.h"
#include "core/util/AtomicFile.h"
#include "core/util/Log.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace devdatapod {

namespace {
constexpr int kSchemaVersion = 1;
constexpr const char* kRoleDefault = "default";
constexpr const char* kRoleSecondary = "secondary";

// Kind order for the written file: first appearance among instances, which is install
// order. Sorting alphabetically would make diffs of services.json jump around.
QStringList kindOrder(const QVector<ServiceInstance>& items) {
    QStringList out;
    for (const ServiceInstance& inst : items) {
        if (!out.contains(inst.kind)) out.append(inst.kind);
    }
    return out;
}
}

InstanceRegistry::InstanceRegistry(const QString& filePath) : path_(filePath) {}

void InstanceRegistry::setProblem(const QString& text) {
    problem_ = text;
    healthy_ = false;
    log::error(QStringLiteral("services: %1").arg(text));
}

// ------------------------------------------------------------------------ load / save
bool InstanceRegistry::load() {
    loaded_ = false;
    healthy_ = false;
    needsHeal_ = false;
    problem_.clear();
    items_.clear();
    defaults_.clear();

    if (!QFileInfo::exists(path_)) {
        // Nothing installed yet is the normal state on a fresh machine, not a failure.
        loaded_ = true;
        healthy_ = true;
        log::info(QStringLiteral("services: no %1 yet, starting with an empty registry")
                      .arg(QFileInfo(path_).fileName()));
        return true;
    }

    QByteArray raw;
    QString error;
    if (!file::readAll(path_, &raw, &error)) {
        setProblem(QStringLiteral("cannot read %1: %2").arg(path_, error));
        return false;
    }
    switch (parse(raw)) {
    case ParseResult::Fatal:
        return false;
    case ParseResult::Frozen:
        // Parsed but self-contradictory: still readable so the UI can show what is
        // installed, but every write path refuses until the user fixes the file.
        loaded_ = true;
        syncFlags();
        return true;
    case ParseResult::Ok:
        break;
    }

    syncFlags();
    loaded_ = true;
    healthy_ = true;
    log::info(QStringLiteral("services: %1 instance(s) across %2 kind(s)")
                  .arg(items_.size())
                  .arg(kindOrder(items_).size()));

    // The "default" id is the only authority in the file, so healing a stale per-instance
    // "role" has exactly one correct answer and is not a guess about user intent.
    if (needsHeal_ && !save()) {
        log::warn(QStringLiteral("services: %1 could not be rewritten to fix role markers")
                      .arg(path_));
    }
    return true;
}

InstanceRegistry::ParseResult InstanceRegistry::parse(const QByteArray& raw) {
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        setProblem(QStringLiteral("%1 is not valid JSON: %2").arg(path_, error.errorString()));
        return ParseResult::Fatal;
    }
    const QJsonObject root = doc.object();
    const int schema = root.value(QStringLiteral("schema")).toInt();
    if (schema != kSchemaVersion) {
        setProblem(QStringLiteral("schema %1 is not supported (expected %2)")
                       .arg(schema)
                       .arg(kSchemaVersion));
        return ParseResult::Fatal;
    }

    const QJsonObject kinds = root.value(QStringLiteral("kinds")).toObject();
    for (auto kindIt = kinds.constBegin(); kindIt != kinds.constEnd(); ++kindIt) {
        const QString kindName = kindIt.key();
        if (kinds::find(kindName) == nullptr) {
            // Read-only tolerated state: a kind written by a newer build. Writing it back
            // is fine, but nothing may allocate ports for it without a driver entry.
            log::warn(QStringLiteral("services: kind \"%1\" has no driver table entry")
                          .arg(kindName));
        }
        const QJsonObject block = kindIt.value().toObject();
        const QJsonArray array = block.value(QStringLiteral("instances")).toArray();

        int markedDefault = 0;
        QString markedId;
        for (const QJsonValue& value : array) {
            const QJsonObject entry = value.toObject();
            ServiceInstance inst;
            inst.kind = kindName;
            inst.version = entry.value(QStringLiteral("version")).toString();
            inst.id = entry.value(QStringLiteral("id")).toString();
            if (inst.id.isEmpty()) inst.id = ServiceInstance::makeId(kindName, inst.version);
            inst.portKey = entry.value(QStringLiteral("portKey")).toString();
            inst.installPath = entry.value(QStringLiteral("installPath")).toString();
            inst.dataDir = entry.value(QStringLiteral("dataDir")).toString();
            const QString role = entry.value(QStringLiteral("role")).toString();

            if (inst.version.isEmpty()) {
                setProblem(QStringLiteral("%1 has an instance without a version").arg(kindName));
                return ParseResult::Frozen;
            }
            if (indexOf(inst.id) >= 0) {
                setProblem(QStringLiteral("instance id \"%1\" appears twice").arg(inst.id));
                return ParseResult::Frozen;
            }
            if (role == QLatin1String(kRoleDefault)) {
                ++markedDefault;
                if (markedId.isEmpty()) markedId = inst.id;
            }
            items_.append(inst);
        }

        if (markedDefault > 1) {
            setProblem(QStringLiteral("%1 has %2 instances marked default")
                           .arg(kindName)
                           .arg(markedDefault));
            return ParseResult::Frozen;
        }

        const QString declared = block.value(QStringLiteral("default")).toString();
        if (!declared.isEmpty()) {
            const int index = indexOf(declared);
            if (index < 0 || items_[index].kind != kindName) {
                setProblem(QStringLiteral("%1 points at default \"%2\" which is not installed")
                               .arg(kindName, declared));
                return ParseResult::Frozen;
            }
            defaults_.insert(kindName, declared);
            if (!markedId.isEmpty() && markedId != declared) needsHeal_ = true;
        } else if (!markedId.isEmpty()) {
            defaults_.insert(kindName, markedId);
            needsHeal_ = true;
        }
    }
    return ParseResult::Ok;
}

QByteArray InstanceRegistry::toJson() const {
    QJsonObject kinds;
    for (const QString& kindName : kindOrder(items_)) {
        QJsonArray array;
        for (const ServiceInstance& inst : items_) {
            if (inst.kind != kindName) continue;
            QJsonObject entry;
            entry.insert(QStringLiteral("id"), inst.id);
            entry.insert(QStringLiteral("version"), inst.version);
            entry.insert(QStringLiteral("role"),
                         QString::fromLatin1(inst.isDefault ? kRoleDefault : kRoleSecondary));
            if (!inst.portKey.isEmpty()) entry.insert(QStringLiteral("portKey"), inst.portKey);
            entry.insert(QStringLiteral("installPath"), inst.installPath);
            entry.insert(QStringLiteral("dataDir"), inst.dataDir);
            array.append(entry);
        }
        QJsonObject block;
        block.insert(QStringLiteral("default"), defaults_.value(kindName));
        block.insert(QStringLiteral("instances"), array);
        kinds.insert(kindName, block);
    }

    QJsonObject root;
    root.insert(QStringLiteral("schema"), kSchemaVersion);
    root.insert(QStringLiteral("kinds"), kinds);
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

bool InstanceRegistry::save() {
    if (!loaded_) {
        log::error(QStringLiteral("services: refusing to save before a successful load()"));
        return false;
    }
    if (!healthy_) {
        log::error(QStringLiteral("services: refusing to write, %1").arg(problem_));
        return false;
    }
    syncFlags();
    needsHeal_ = false;
    return file::writeAtomically(path_, toJson());
}

// ------------------------------------------------------------------------------- derived
void InstanceRegistry::syncFlags() {
    for (ServiceInstance& inst : items_) {
        inst.isDefault = defaults_.value(inst.kind) == inst.id;
    }
}

int InstanceRegistry::indexOf(const QString& id) const {
    for (int i = 0; i < items_.size(); ++i) {
        if (items_[i].id == id) return i;
    }
    return -1;
}

// ------------------------------------------------------------------------------ queries
QVector<ServiceInstance> InstanceRegistry::ofKind(const QString& kind) const {
    QVector<ServiceInstance> out;
    for (const ServiceInstance& inst : items_) {
        if (inst.kind == kind) out.append(inst);
    }
    return out;
}

bool InstanceRegistry::find(const QString& id, ServiceInstance* out) const {
    const int index = indexOf(id);
    if (index < 0) return false;
    if (out) *out = items_[index];
    return true;
}

const ServiceInstance* InstanceRegistry::defaultOf(const QString& kind) const {
    const int index = indexOf(defaults_.value(kind));
    if (index < 0) return nullptr;
    return &items_[index];
}

bool InstanceRegistry::isDefault(const QString& id) const {
    const int index = indexOf(id);
    return index >= 0 && items_[index].isDefault;
}

int InstanceRegistry::countOfKind(const QString& kind) const {
    int n = 0;
    for (const ServiceInstance& inst : items_) {
        if (inst.kind == kind) ++n;
    }
    return n;
}

QStringList InstanceRegistry::kindsPresent() const {
    return kindOrder(items_);
}

// ------------------------------------------------------------------------------ mutations
bool InstanceRegistry::add(ServiceInstance instance) {
    if (!healthy_) return false;
    const kinds::Meta* meta = kinds::find(instance.kind);
    if (!meta) {
        log::error(QStringLiteral("services: refusing to register a %1, no driver for it")
                       .arg(instance.kind));
        return false;
    }
    if (instance.version.isEmpty()) {
        log::error(QStringLiteral("services: cannot register a %1 without a version")
                       .arg(instance.kind));
        return false;
    }
    if (instance.id.isEmpty()) {
        instance.id = ServiceInstance::makeId(instance.kind, instance.version);
    }
    if (indexOf(instance.id) >= 0) {
        log::warn(QStringLiteral("services: %1 is already registered").arg(instance.id));
        return false;
    }
    if (instance.dataDir.isEmpty()) {
        instance.dataDir = ServiceInstance::makeDataDir(instance.kind, instance.version);
    }
    if (instance.portKey.isEmpty() && !meta->slot.isEmpty()) {
        instance.portKey = ServiceInstance::makePortKey(instance.kind, instance.version);
    }

    const bool firstOfKind = countOfKind(instance.kind) == 0;
    const QVector<ServiceInstance> snapshot = items_;
    const QString previousDefault = defaults_.value(instance.kind);

    items_.append(instance);
    if (firstOfKind) {
        // The first version installed is the one sites talk to. Anything later has to be
        // promoted explicitly, so installing a second version never hijacks 3306.
        defaults_.insert(instance.kind, instance.id);
        log::info(QStringLiteral("services: %1 is the default of %2")
                      .arg(instance.id, instance.kind));
    }
    syncFlags();
    if (!save()) {
        items_ = snapshot;
        if (previousDefault.isEmpty()) defaults_.remove(instance.kind);
        else defaults_.insert(instance.kind, previousDefault);
        syncFlags();
        return false;
    }
    return true;
}

bool InstanceRegistry::remove(const QString& id) {
    if (!healthy_) return false;
    const int index = indexOf(id);
    if (index < 0) return true;

    const QString kind = items_[index].kind;
    const QVector<ServiceInstance> snapshot = items_;
    const QString previousDefault = defaults_.value(kind);

    items_.remove(index);
    if (previousDefault != id) {
        syncFlags();
        return save();
    }

    // A kind with versions left must still name a default, or nothing would hold the
    // canonical port and every site connection would fail with no explanation.
    const QVector<ServiceInstance> remaining = ofKind(kind);
    if (remaining.isEmpty()) {
        defaults_.remove(kind);
        log::warn(QStringLiteral("services: %1 removed, %2 has no instances left").arg(id, kind));
    } else {
        defaults_.insert(kind, remaining.first().id);
        log::warn(QStringLiteral("services: %1 removed, default of %2 moved to %3")
                      .arg(id, kind, remaining.first().id));
    }
    if (!save()) {
        items_ = snapshot;
        defaults_.insert(kind, previousDefault);
        syncFlags();
        return false;
    }
    return true;
}

bool InstanceRegistry::promoteDefault(const QString& id) {
    if (!healthy_) return false;
    const int index = indexOf(id);
    if (index < 0) {
        log::error(QStringLiteral("services: cannot promote unknown instance %1").arg(id));
        return false;
    }
    const QString kind = items_[index].kind;
    const QString previousDefault = defaults_.value(kind);
    if (previousDefault == id) return true;

    defaults_.insert(kind, id);
    syncFlags();
    if (!save()) {
        // Inserting an empty string would leave the kind naming "" as its default, which the
        // next load() reads as "declared default is not installed" and freezes the file.
        if (previousDefault.isEmpty()) defaults_.remove(kind);
        else defaults_.insert(kind, previousDefault);
        syncFlags();
        return false;
    }
    log::info(QStringLiteral("services: default of %1 is now %2 (was %3)")
                  .arg(kind, id,
                       previousDefault.isEmpty() ? QStringLiteral("nobody") : previousDefault));
    return true;
}

}
