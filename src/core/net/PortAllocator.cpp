#include "core/net/PortAllocator.h"

#include "core/util/AtomicFile.h"
#include "core/util/Log.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace devdatapod {

namespace {
constexpr int kSchemaVersion = 2;
constexpr const char* kSeedResource = ":/devdatapod/config/ports.default.json";
constexpr const char* kRoleDefault = "default";
constexpr const char* kRoleSecondary = "secondary";

QDateTime nowUtc() { return QDateTime::currentDateTimeUtc(); }

QJsonObject readObject(const QJsonDocument& doc, const QString& key) {
    return doc.object().value(key).toObject();
}

bool readPair(const QJsonValue& value, int* first, int* last) {
    const QJsonArray array = value.toArray();
    if (array.size() != 2) return false;
    *first = array.at(0).toInt();
    *last = array.at(1).toInt();
    return *first > 0 && *last >= *first;
}
}

PortAllocator::PortAllocator(const QString& filePath) : path_(filePath) {}

void PortAllocator::setProblem(const QString& text) {
    problem_ = text;
    healthy_ = false;
    log::error(QStringLiteral("ports: %1").arg(text));
}

// ------------------------------------------------------------------------ load / save
bool PortAllocator::load() {
    loaded_ = false;
    healthy_ = false;
    problem_.clear();
    items_.clear();
    canonical_.clear();
    slots_.clear();
    ranges_.clear();
    panel_.clear();

    QByteArray raw;
    if (!QFileInfo::exists(path_)) {
        QFile seed(QString::fromLatin1(kSeedResource));
        if (!seed.open(QIODevice::ReadOnly)) {
            setProblem(QStringLiteral("no %1 and the built-in seed is missing")
                           .arg(QFileInfo(path_).fileName()));
            return false;
        }
        raw = seed.readAll();
        if (!file::writeAtomically(path_, raw)) return false;
        log::info(QStringLiteral("ports: seeded %1").arg(path_));
    } else {
        QString error;
        if (!file::readAll(path_, &raw, &error)) {
            setProblem(QStringLiteral("cannot read %1: %2").arg(path_, error));
            return false;
        }
    }

    // parse() may call setProblem() and still return true: "readable but frozen". Setting
    // the flag first means that call is the last word instead of being overwritten below.
    healthy_ = true;
    if (!parse(raw)) return false;
    loaded_ = true;
    if (!healthy_) {
        log::warn(QStringLiteral("ports: %1 is readable but frozen: %2").arg(path_, problem_));
        return true;
    }
    log::info(QStringLiteral("ports: %1 allocations, band %2-%3")
                  .arg(items_.size())
                  .arg(bandFirst_)
                  .arg(bandLast_));
    return true;
}

bool PortAllocator::parse(const QByteArray& raw) {
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        setProblem(QStringLiteral("%1 is not valid JSON: %2")
                       .arg(path_, error.errorString()));
        return false;
    }
    const QJsonObject root = doc.object();
    const int schema = root.value(QStringLiteral("schema")).toInt();
    if (schema != kSchemaVersion) {
        setProblem(QStringLiteral("schema %1 is not supported (expected %2)")
                       .arg(schema)
                       .arg(kSchemaVersion));
        return false;
    }

    int first = 0;
    int last = 0;
    if (!readPair(root.value(QStringLiteral("band")), &first, &last)) {
        setProblem(QStringLiteral("missing or malformed \"band\""));
        return false;
    }
    bandFirst_ = first;
    bandLast_ = last;
    bind_ = root.value(QStringLiteral("bindAddress")).toString(QStringLiteral("127.0.0.1"));

    const QJsonObject canonical = readObject(doc, QStringLiteral("canonical"));
    for (auto it = canonical.constBegin(); it != canonical.constEnd(); ++it) {
        canonical_.insert(it.key(), it.value().toInt());
    }
    const QJsonObject strides = readObject(doc, QStringLiteral("stride"));
    const QJsonObject slotTable = readObject(doc, QStringLiteral("slots"));
    for (auto it = slotTable.constBegin(); it != slotTable.constEnd(); ++it) {
        Slot slot;
        slot.name = it.key();
        if (!readPair(it.value(), &slot.first, &slot.last)) {
            setProblem(QStringLiteral("slot \"%1\" is not [first, last]").arg(it.key()));
            return false;
        }
        slot.stride = qMax(1, strides.value(it.key()).toInt(1));
        slots_.append(slot);
    }
    std::sort(slots_.begin(), slots_.end(),
              [](const Slot& a, const Slot& b) { return a.name < b.name; });

    const QJsonObject ranges = readObject(doc, QStringLiteral("ranges"));
    for (auto it = ranges.constBegin(); it != ranges.constEnd(); ++it) {
        Slot range;
        range.name = it.key();
        if (!readPair(it.value(), &range.first, &range.last)) {
            setProblem(QStringLiteral("range \"%1\" is not [first, last]").arg(it.key()));
            return false;
        }
        ranges_.insert(it.key(), range);
    }
    const QJsonObject panel = readObject(doc, QStringLiteral("panel"));
    for (auto it = panel.constBegin(); it != panel.constEnd(); ++it) {
        panel_.insert(it.key(), it.value().toInt());
    }

    const QJsonObject allocations = readObject(doc, QStringLiteral("allocations"));
    for (auto it = allocations.constBegin(); it != allocations.constEnd(); ++it) {
        const QJsonObject entry = it.value().toObject();
        Allocation a;
        a.key = it.key();
        a.port = entry.value(QStringLiteral("port")).toInt();
        a.owner = entry.value(QStringLiteral("owner")).toString();
        a.role = entry.value(QStringLiteral("role")).toString();
        const QString bound = entry.value(QStringLiteral("boundAt")).toString();
        if (!bound.isEmpty()) a.boundAt = QDateTime::fromString(bound, Qt::ISODate);
        if (a.port <= 0 || a.owner.isEmpty()) {
            setProblem(QStringLiteral("allocation \"%1\" is incomplete").arg(a.key));
            return false;
        }
        items_.append(a);
    }
    std::sort(items_.begin(), items_.end(),
              [](const Allocation& a, const Allocation& b) { return a.key < b.key; });

    // Duplicate ports are the failure mode that turns into "two services fighting for
    // 3306", so the file is readable but frozen until a human resolves it.
    for (int i = 0; i < items_.size(); ++i) {
        for (int j = i + 1; j < items_.size(); ++j) {
            if (items_[i].port == items_[j].port) {
                setProblem(QStringLiteral("%1 and %2 both claim port %3")
                               .arg(items_[i].key, items_[j].key)
                               .arg(items_[i].port));
                return true;
            }
        }
    }
    return true;
}

QByteArray PortAllocator::toJson() const {
    QJsonObject canonical;
    for (auto it = canonical_.constBegin(); it != canonical_.constEnd(); ++it) {
        canonical.insert(it.key(), it.value());
    }
    QJsonObject slotTable;
    QJsonObject strides;
    for (const Slot& slot : slots_) {
        slotTable.insert(slot.name, QJsonArray{slot.first, slot.last});
        if (slot.stride != 1) strides.insert(slot.name, slot.stride);
    }
    QJsonObject ranges;
    for (auto it = ranges_.constBegin(); it != ranges_.constEnd(); ++it) {
        const Slot& range = it.value();
        ranges.insert(it.key(), QJsonArray{range.first, range.last});
    }
    QJsonObject panel;
    for (auto it = panel_.constBegin(); it != panel_.constEnd(); ++it) {
        panel.insert(it.key(), it.value());
    }
    QJsonObject allocations;
    for (const Allocation& a : items_) {
        QJsonObject entry;
        entry.insert(QStringLiteral("port"), a.port);
        entry.insert(QStringLiteral("owner"), a.owner);
        if (!a.role.isEmpty()) entry.insert(QStringLiteral("role"), a.role);
        if (a.boundAt.isValid()) {
            entry.insert(QStringLiteral("boundAt"), a.boundAt.toString(Qt::ISODate));
        }
        allocations.insert(a.key, entry);
    }

    QJsonObject root;
    root.insert(QStringLiteral("schema"), kSchemaVersion);
    root.insert(QStringLiteral("band"), QJsonArray{bandFirst_, bandLast_});
    root.insert(QStringLiteral("bindAddress"), bind_);
    root.insert(QStringLiteral("canonical"), canonical);
    root.insert(QStringLiteral("slots"), slotTable);
    if (!strides.isEmpty()) root.insert(QStringLiteral("stride"), strides);
    if (!ranges.isEmpty()) root.insert(QStringLiteral("ranges"), ranges);
    root.insert(QStringLiteral("panel"), panel);
    root.insert(QStringLiteral("allocations"), allocations);
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

bool PortAllocator::save() {
    if (!loaded_) {
        log::error(QStringLiteral("ports: refusing to save before a successful load()"));
        return false;
    }
    if (!healthy_) {
        log::error(QStringLiteral("ports: refusing to write, %1").arg(problem_));
        return false;
    }
    return file::writeAtomically(path_, toJson());
}

// ------------------------------------------------------------------------ band table
int PortAllocator::canonicalPort(const QString& band) const {
    return canonical_.value(band, -1);
}

const PortAllocator::Slot* PortAllocator::findSlot(const QString& name) const {
    const auto it = std::find_if(slots_.begin(), slots_.end(),
                                 [&](const Slot& s) { return s.name == name; });
    return it == slots_.end() ? nullptr : &(*it);
}

bool PortAllocator::slotRange(const QString& slot, int* first, int* last) const {
    const Slot* found = findSlot(slot);
    if (!found) return false;
    if (first) *first = found->first;
    if (last) *last = found->last;
    return true;
}

int PortAllocator::slotStride(const QString& slot) const {
    const Slot* found = findSlot(slot);
    return found ? found->stride : 1;
}

bool PortAllocator::reservedRange(const QString& name, int* first, int* last) const {
    const auto it = ranges_.constFind(name);
    if (it == ranges_.constEnd()) return false;
    if (first) *first = it->first;
    if (last) *last = it->last;
    return true;
}

int PortAllocator::panelPort(const QString& name) const {
    return panel_.value(name, -1);
}

// ---------------------------------------------------------------------------- lookups
int PortAllocator::indexOf(const QString& key) const {
    for (int i = 0; i < items_.size(); ++i) {
        if (items_[i].key == key) return i;
    }
    return -1;
}

bool PortAllocator::contains(const QString& key) const {
    return indexOf(key) >= 0;
}

int PortAllocator::portOf(const QString& key) const {
    const int index = indexOf(key);
    return index < 0 ? 0 : items_[index].port;
}

QString PortAllocator::roleOf(const QString& key) const {
    const int index = indexOf(key);
    return index < 0 ? QString() : items_[index].role;
}

QVector<PortAllocator::Allocation> PortAllocator::allocationsOf(const QString& owner) const {
    QVector<Allocation> out;
    for (const Allocation& a : items_) {
        if (a.owner == owner) out.append(a);
    }
    return out;
}

bool PortAllocator::hasPort(int port, const QString& exceptKey) const {
    for (const Allocation& a : items_) {
        if (a.key != exceptKey && a.port == port) return true;
    }
    return false;
}

int PortAllocator::freePortIn(const Slot& slot, int skip) const {
    for (int port = slot.first; port <= slot.last; port += qMax(1, slot.stride)) {
        if (port != skip && !hasPort(port)) return port;
    }
    return 0;
}

// --------------------------------------------------------------------------- mutations
int PortAllocator::allocateSlot(const QString& slot, const QString& key, const QString& owner) {
    if (!healthy_) return -1;
    const int existing = portOf(key);
    if (existing) return existing;    // fixed at first install, never re-drawn

    const Slot* band = findSlot(slot);
    if (!band) {
        log::error(QStringLiteral("ports: no slot band \"%1\" in %2").arg(slot, path_));
        return -1;
    }
    const int port = freePortIn(*band);
    if (!port) {
        log::error(QStringLiteral("ports: band %1-%2 is full").arg(band->first).arg(band->last));
        return -1;
    }

    const QVector<Allocation> snapshot = items_;
    items_.append({key, port, owner, QString::fromLatin1(kRoleSecondary), nowUtc()});
    std::sort(items_.begin(), items_.end(),
              [](const Allocation& a, const Allocation& b) { return a.key < b.key; });
    if (!save()) {
        items_ = snapshot;
        return -1;
    }
    log::info(QStringLiteral("ports: %1 -> %2 (%3)").arg(key).arg(port).arg(slot));
    return port;
}

bool PortAllocator::assignCanonical(const QString& band, const QString& key, const QString& owner) {
    if (!healthy_) return false;
    const int port = canonicalPort(band);
    if (port <= 0) {
        log::error(QStringLiteral("ports: band \"%1\" has no canonical port").arg(band));
        return false;
    }
    const int index = indexOf(key);
    if (index >= 0 && items_[index].port == port
            && items_[index].role == QLatin1String(kRoleDefault)) {
        return true;
    }
    // Somebody else holds it. Whether that is an external process or our own secondary,
    // stealing it silently is exactly how "the site says 3306 but nothing answers" starts.
    if (hasPort(port)) {
        log::error(QStringLiteral("ports: canonical %1 for %2 is already held")
                       .arg(port)
                       .arg(band));
        return false;
    }
    for (const Allocation& a : items_) {
        if (a.key != key && a.owner == owner && a.role == QLatin1String(kRoleDefault)) {
            log::error(QStringLiteral("ports: %1 already owns the default of %2")
                           .arg(a.key, band));
            return false;
        }
    }

    const QVector<Allocation> snapshot = items_;
    Allocation fresh{key, port, owner, QString::fromLatin1(kRoleDefault), nowUtc()};
    if (index >= 0) items_[index] = fresh;
    else items_.append(fresh);
    std::sort(items_.begin(), items_.end(),
              [](const Allocation& a, const Allocation& b) { return a.key < b.key; });
    if (!save()) {
        items_ = snapshot;
        return false;
    }
    return true;
}

bool PortAllocator::release(const QString& key) {
    if (!healthy_) return false;
    const int index = indexOf(key);
    if (index < 0) return true;
    const QVector<Allocation> snapshot = items_;
    items_.remove(index);
    if (!save()) {
        items_ = snapshot;
        return false;
    }
    return true;
}

int PortAllocator::retarget(const QString& key, const QString& slot) {
    if (!healthy_) return 0;
    const int index = indexOf(key);
    if (index < 0) return 0;
    if (items_[index].role == QLatin1String(kRoleDefault)) {
        // A default instance belongs on the canonical port. Moving it off 3306 because
        // something else is there is the "route around the conflict" this forbids
        // (AGENTS.md rule 3) - report the holder instead.
        log::error(QStringLiteral("ports: %1 is the default of %2, it does not move")
                       .arg(key, items_[index].owner));
        return 0;
    }
    const Slot* band = findSlot(slot);
    if (!band) {
        log::error(QStringLiteral("ports: %1 has no band \"%2\" to move inside")
                       .arg(key, slot));
        return 0;
    }
    const int oldPort = items_[index].port;
    const int port = freePortIn(*band, oldPort);
    if (!port) return 0;

    const QVector<Allocation> snapshot = items_;
    items_[index].port = port;
    items_[index].boundAt = nowUtc();
    if (!save()) {
        items_ = snapshot;
        return 0;
    }
    log::warn(QStringLiteral("ports: %1 moved %2 -> %3, references must be re-rendered")
                  .arg(key)
                  .arg(oldPort)
                  .arg(port));
    return port;
}

bool PortAllocator::swapRoles(const QString& defaultKey, const QString& otherKey) {
    if (!healthy_) return false;
    const int a = indexOf(defaultKey);
    const int b = indexOf(otherKey);
    if (a < 0 || b < 0 || a == b) {
        log::error(QStringLiteral("ports: swap needs two known keys, got %1 and %2")
                       .arg(defaultKey, otherKey));
        return false;
    }
    if (items_[a].owner != items_[b].owner) {
        log::error(QStringLiteral("ports: %1 and %2 are different kinds").arg(defaultKey, otherKey));
        return false;
    }
    const QString roleA = items_[a].role;
    const QString roleB = items_[b].role;
    if (roleA == roleB) {
        log::error(QStringLiteral("ports: swap wants exactly one default, %1 and %2 are both \"%3\"")
                       .arg(defaultKey, otherKey, roleA));
        return false;
    }

    // Each port must belong to its own key only: the old default was stopped first, so
    // both are free and no temporary third port is required.
    const int portA = items_[a].port;
    const int portB = items_[b].port;
    if (hasPort(portA, defaultKey) || hasPort(portB, otherKey)) {
        log::error(QStringLiteral("ports: a third allocation holds %1 or %2, refusing to swap")
                       .arg(portA)
                       .arg(portB));
        return false;
    }

    const QVector<Allocation> snapshot = items_;
    std::swap(items_[a].port, items_[b].port);
    std::swap(items_[a].role, items_[b].role);
    items_[a].boundAt = nowUtc();
    items_[b].boundAt = nowUtc();
    if (!save()) {
        items_ = snapshot;
        return false;
    }
    log::info(QStringLiteral("ports: default of %1 is now %2 (swapped ports %3 and %4)")
                  .arg(items_[a].owner, otherKey)
                  .arg(portA)
                  .arg(portB));
    return true;
}

bool PortAllocator::setPort(const QString& key, int port) {
    if (!healthy_) return false;
    const int index = indexOf(key);
    if (index < 0) {
        log::error(QStringLiteral("ports: cannot set port on unknown key %1").arg(key));
        return false;
    }
    if (port <= 0 || port > 65535) {
        log::error(QStringLiteral("ports: %1 is not a valid port").arg(port));
        return false;
    }
    if (hasPort(port, key)) {
        QString holder;
        for (const Allocation& a : items_) {
            if (a.key != key && a.port == port) { holder = a.key; break; }
        }
        log::error(QStringLiteral("ports: %1 already holds %2, cannot give it to %3")
                       .arg(holder).arg(port).arg(key));
        return false;
    }

    // 新端口必须在规划内：canonical 表或某个 slot 段。
    bool inPlan = false;
    bool isCanonical = false;
    for (auto it = canonical_.constBegin(); it != canonical_.constEnd(); ++it) {
        if (it.value() == port) { inPlan = true; isCanonical = true; break; }
    }
    for (const Slot& slot : slots_) {
        if (!inPlan && port >= slot.first && port <= slot.last) inPlan = true;
    }
    if (!inPlan) {
        log::error(QStringLiteral("ports: %1 is not a planned port, refusing to set it on %2")
                       .arg(port).arg(key));
        return false;
    }

    const QVector<Allocation> snapshot = items_;
    items_[index].port = port;
    items_[index].boundAt = nowUtc();
    // default 实例必须站在 canonical 端口上；挪走了就不再是 default。
    if (!isCanonical && items_[index].role == QLatin1String(kRoleDefault)) {
        items_[index].role = QString::fromLatin1(kRoleSecondary);
    }
    std::sort(items_.begin(), items_.end(),
              [](const Allocation& a, const Allocation& b) { return a.key < b.key; });
    if (!save()) {
        items_ = snapshot;
        return false;
    }
    log::info(QStringLiteral("ports: %1 manually set to %2 (role=%3)")
                  .arg(key).arg(port).arg(items_[index].role));
    return true;
}

bool PortAllocator::rebind(const QString& key, int port, const QString& role,
                           const QString& owner) {
    if (!healthy_) return false;
    if (port <= 0 || owner.isEmpty()) {
        log::error(QStringLiteral("ports: refusing to rebind %1 to an incomplete allocation")
                       .arg(key));
        return false;
    }
    if (hasPort(port, key)) {
        QString holder;
        for (const Allocation& a : items_) {
            if (a.key != key && a.port == port) { holder = a.key; break; }
        }
        log::error(QStringLiteral("ports: %1 still holds %2, cannot rebind %3 to it")
                       .arg(holder)
                       .arg(port)
                       .arg(key));
        return false;
    }
    // Rule 1 again, on the way back: a rollback may restore a planned number, never invent
    // one. Canonical table first, then any slot band.
    bool inPlan = false;
    for (auto it = canonical_.constBegin(); it != canonical_.constEnd(); ++it) {
        if (it.value() == port) { inPlan = true; break; }
    }
    for (const Slot& slot : slots_) {
        if (!inPlan && port >= slot.first && port <= slot.last) inPlan = true;
    }
    if (!inPlan) {
        log::error(QStringLiteral("ports: %1 is not a planned port, refusing to rebind %2 to it")
                       .arg(port)
                       .arg(key));
        return false;
    }

    const int index = indexOf(key);
    // A fresh boundAt would make a rolled-back ports.json differ from the one before the
    // switch, which is exactly what "restart must not drift" rules out.
    const QDateTime boundAt = index >= 0 ? items_[index].boundAt : nowUtc();
    const QVector<Allocation> snapshot = items_;
    Allocation fresh{key, port, owner, role, boundAt};
    if (index >= 0) items_[index] = fresh;
    else items_.append(fresh);
    std::sort(items_.begin(), items_.end(),
              [](const Allocation& a, const Allocation& b) { return a.key < b.key; });
    if (!save()) {
        items_ = snapshot;
        return false;
    }
    log::info(QStringLiteral("ports: %1 rebound to %2 (%3)").arg(key).arg(port).arg(role));
    return true;
}

}
