#pragma once

#include "core/service/ServiceInstance.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

namespace devdatapod {

// Reads and writes data/services.json - the persistent half of the instance model.
//
// The invariant this class exists to hold (AGENTS.md rule 10): a kind has *at most one*
// default instance. In the file that is structural - each kind stores a single "default"
// id - and ServiceInstance::isDefault is derived from it in memory, so the two can never
// disagree inside one process.
//
// Role here is orthogonal to run state (core/model/RunState.h): "default" is intent, not
// "currently running". A secondary instance may run on its slot port at the same time.
//
// Order is preserved exactly as stored: that is install order, and it is what makes slot
// allocation deterministic (docs/PORT-PLAN.md).
class InstanceRegistry {
public:
    explicit InstanceRegistry(const QString& filePath);

    // A missing file is not an error - the panel simply has nothing installed yet.
    // A file that parses but contradicts itself loads *unhealthy*: reads work, writes
    // refuse until the offending kind is fixed.
    bool load();
    bool save();

    bool isLoaded() const { return loaded_; }
    bool isHealthy() const { return healthy_; }
    QString problem() const { return problem_; }
    QString filePath() const { return path_; }

    // ------------------------------------------------------------------ queries
    QVector<ServiceInstance> instances() const { return items_; }
    QVector<ServiceInstance> ofKind(const QString& kind) const;
    bool find(const QString& id, ServiceInstance* out) const;
    // nullptr when the kind has no instances or its default was uninstalled.
    const ServiceInstance* defaultOf(const QString& kind) const;
    QString defaultId(const QString& kind) const { return defaults_.value(kind); }
    bool isDefault(const QString& id) const;
    int countOfKind(const QString& kind) const;
    QStringList kindsPresent() const;

    // ---------------------------------------------------------------- mutations
    // Fills in id / portKey / dataDir when the caller left them empty. The first instance
    // of a kind becomes its default; every later one is secondary. Returns false when the
    // id already exists or the kind is unknown.
    bool add(ServiceInstance instance);
    // Removing the default promotes the earliest remaining instance of that kind, so a
    // kind with installed versions never ends up with nothing holding the canonical port.
    bool remove(const QString& id);
    // Role flip only. The matching port swap is PortAllocator::swapRoles and the two are
    // driven by one transaction in DefaultSwitcher - neither belongs on its own.
    bool promoteDefault(const QString& id);

private:
    // Fatal = nothing usable was read. Frozen = the data is readable but contradicts
    // itself, so it loads read-only and every write refuses until a human fixes it.
    enum class ParseResult { Ok, Frozen, Fatal };

    ParseResult parse(const QByteArray& raw);
    QByteArray toJson() const;
    void syncFlags();
    void setProblem(const QString& text);
    int indexOf(const QString& id) const;

    QString path_;
    QVector<ServiceInstance> items_;
    QHash<QString, QString> defaults_;    // kind -> instance id
    bool loaded_ = false;
    bool healthy_ = false;
    bool needsHeal_ = false;
    QString problem_;
};

}
