#include "core/service/Engine.h"

#include "core/net/PortAllocator.h"
#include "core/net/PortProbe.h"
#include "core/service/MysqlDriver.h"
#include "core/service/NginxDriver.h"
#include "core/service/RedisDriver.h"
#include "core/service/ServiceKind.h"
#include "core/util/AppPaths.h"
#include "core/util/Log.h"

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace devdatapod {

namespace {
constexpr const char* kRoleDefault = "default";
constexpr const char* kRoleSecondary = "secondary";
constexpr const char* kPanelOwner = "panel";

QString describeOccupier(const net::PortOccupier& holder) {
    if (!holder.occupied) return QStringLiteral("unknown");
    QString out = holder.processName.isEmpty() ? QStringLiteral("pid %1").arg(holder.pid)
                                               : QStringLiteral("%1 (pid %2)")
                                                     .arg(holder.processName)
                                                     .arg(holder.pid);
    if (!holder.executable.isEmpty()) out += QStringLiteral(" from %1").arg(holder.executable);
    return out;
}
}

Engine::Engine(PortAllocator* ports, InstanceRegistry* registry, QObject* parent)
    : QObject(parent), ports_(ports), registry_(registry) {}

// ------------------------------------------------------------------------------- reconcile
bool Engine::synchronize(QString* error) {
    ready_ = false;
    problem_.clear();

    if (!ports_->isLoaded() || !registry_->isLoaded()) {
        problem_ = QStringLiteral("the port or instance store failed to load");
        if (error) *error = problem_;
        return false;
    }
    // Either file may be readable-but-frozen. Both cases are reported rather than healed:
    // the fix belongs to whoever edited the file, not to a guess made here.
    if (!registry_->isHealthy()) {
        problem_ = QStringLiteral("services.json: %1").arg(registry_->problem());
        if (error) *error = problem_;
        return false;
    }
    if (!ports_->isHealthy()) {
        problem_ = QStringLiteral("ports.json: %1").arg(ports_->problem());
        if (error) *error = problem_;
        return false;
    }

    QHash<QString, ServiceInstance> byKey;
    const QVector<ServiceInstance> all = registry_->instances();
    for (const ServiceInstance& instance : all) {
        if (!instance.portKey.isEmpty()) byKey.insert(instance.portKey, instance);
    }

    // 1. Leftovers from an uninstall. A released port that nobody frees turns into "the
    //    band is full" after a few install/remove cycles.
    QStringList stale;
    for (const PortAllocator::Allocation& a : ports_->allocations()) {
        if (a.owner == QLatin1String(kPanelOwner)) continue;
        if (!byKey.contains(a.key)) stale << a.key;
    }
    for (const QString& key : stale) {
        if (!ports_->release(key)) {
            problem_ = QStringLiteral("cannot release the port held by %1").arg(key);
            if (error) *error = problem_;
            return false;
        }
        log::info(QStringLiteral("engine: released %1, its instance is gone").arg(key));
    }

    // 2. Role drift, before any port is claimed: healing afterwards would find the
    //    canonical port already held by the instance that should have lost it.
    for (const QString& kind : registry_->kindsPresent()) {
        QString reason;
        if (healRoleDrift(kind, &reason) < 0) {
            problem_ = reason;
            if (error) *error = reason;
            return false;
        }
    }

    // 3. Fill whatever is still missing.
    for (const ServiceInstance& instance : all) {
        int port = 0;
        QString reason;
        if (!ensureAllocation(instance, &port, &reason)) {
            problem_ = reason;
            if (error) *error = problem_;
            return false;
        }
    }

    ready_ = true;
    emit instancesChanged();
    log::info(QStringLiteral("engine: %1 instances reconciled against %2 allocations")
                  .arg(all.size())
                  .arg(ports_->allocations().size()));
    return true;
}

int Engine::healRoleDrift(const QString& kind, QString* error) {
    const QString wantId = registry_->defaultId(kind);
    ServiceInstance want;
    if (wantId.isEmpty() || !registry_->find(wantId, &want)) return 0;    // nothing installed

    QString current;    // the key ports.json believes is the default
    for (const PortAllocator::Allocation& a : ports_->allocationsOf(kind)) {
        if (a.role != QLatin1String(kRoleDefault)) continue;
        if (!current.isEmpty()) {
            if (error) {
                *error = QStringLiteral("%1: %2 and %3 are both default in ports.json")
                             .arg(kind, current, a.key);
            }
            return -1;
        }
        current = a.key;
    }
    // No default anywhere is not drift - step 3 hands out the canonical port. Agreement is
    // the normal case and costs nothing.
    if (current.isEmpty() || current == want.portKey) return 0;

    // The two files name different defaults for the same kind. That is only repairable when
    // the wanted instance currently holds a slot port: then one atomic swapRoles() makes
    // both files agree again. Anything else needs a human, because every other guess puts
    // two services on the canonical port.
    if (want.portKey.isEmpty() || ports_->roleOf(want.portKey) != QLatin1String(kRoleSecondary)) {
        if (error) {
            *error = QStringLiteral("%1: ports.json has no slot allocation for %2 to swap "
                                    "with, resolve it in ports.json by hand")
                         .arg(kind, want.portKey);
        }
        return -1;
    }
    if (!ports_->swapRoles(current, want.portKey)) {
        if (error) {
            *error = QStringLiteral("%1: cannot move the default from %2 to %3")
                         .arg(kind, current, want.portKey);
        }
        return -1;
    }
    for (const ServiceInstance& candidate : registry_->ofKind(kind)) {
        if (candidate.portKey == current || candidate.portKey == want.portKey) {
            invalidate(candidate.id);
        }
    }
    log::warn(QStringLiteral("engine: %1 default healed from %2 to %3")
                  .arg(kind, current, want.portKey));
    return 1;
}

bool Engine::ensureAllocation(const ServiceInstance& instance, int* port, QString* error) {
    *port = 0;
    if (instance.portKey.isEmpty()) return true;

    const kinds::Meta* meta = kinds::find(instance.kind);
    if (!meta) {
        if (error) {
            *error = QStringLiteral("%1: unknown kind \"%2\"").arg(instance.id, instance.kind);
        }
        return false;
    }

    if (instance.isDefault && !meta->canonical.isEmpty()) {
        const int canonical = ports_->canonicalPort(meta->canonical);
        const bool alreadyOurs = ports_->portOf(instance.portKey) == canonical
                                 && ports_->roleOf(instance.portKey) == QLatin1String(kRoleDefault);
        if (!alreadyOurs && canonical > 0
            && !net::isPortFree(quint16(canonical), ports_->bindAddress())) {
            // Rule 3: name the holder. On a machine with 宝塔 running this fires every time,
            // and "stop that one first" is the only honest answer.
            const net::PortOccupier holder = net::identifyPortOwner(quint16(canonical));
            if (error) {
                *error = QStringLiteral("%1 wants canonical port %2 but %3 is already "
                                        "listening there")
                             .arg(instance.id)
                             .arg(canonical)
                             .arg(describeOccupier(holder));
            }
            return false;
        }
        if (!ports_->assignCanonical(meta->canonical, instance.portKey, instance.kind)) {
            if (error) {
                *error = QStringLiteral("%1: cannot claim canonical port %2")
                             .arg(instance.id)
                             .arg(canonical);
            }
            return false;
        }
        *port = canonical;
        return true;
    }

    if (meta->slot.isEmpty()) return true;    // python: a kind with no listening socket
    const int allocated = ports_->allocateSlot(meta->slot, instance.portKey, instance.kind);
    if (allocated <= 0) {
        if (error) {
            *error = QStringLiteral("%1: no free port in the %2 band").arg(instance.id, meta->slot);
        }
        return false;
    }
    *port = allocated;
    return true;
}

// ------------------------------------------------------------------------------ 驱动工厂
std::unique_ptr<IServiceDriver> Engine::createDriver(const ServiceInstance& instance, int port,
                                                     QString* error) {
    if (instance.kind == QLatin1String("redis")) {
        return std::make_unique<RedisDriver>(instance, port);
    }
    if (instance.kind == QLatin1String("mysql")) {
        return std::make_unique<MysqlDriver>(instance, port);
    }
    if (instance.kind == QLatin1String("nginx")) {
        return std::make_unique<NginxDriver>(instance, port);
    }
    // mariadb / php / python 留给 P3 后续。明确报错强过一个半成品的驱动，
    // 路线图里写了每种类型排在哪一期。
    if (error) {
        *error = QStringLiteral("类型 \"%1\" 还没有驱动（见 docs/ROADMAP.md P3）")
                     .arg(instance.kind);
    }
    return nullptr;
}

IServiceDriver* Engine::driver(const QString& id) {
    const auto cached = drivers_.find(id);
    if (cached != drivers_.end()) return cached->second.get();

    ServiceInstance instance;
    if (!registry_->find(id, &instance)) {
        problem_ = QStringLiteral("no instance \"%1\" in %2").arg(id, registry_->filePath());
        return nullptr;
    }
    const int port = ports_->portOf(instance.portKey);
    if (instance.hasPort() && port <= 0) {
        problem_ = QStringLiteral("%1 has no port allocation; reconcile ports.json first")
                       .arg(instance.id);
        return nullptr;
    }
    QString error;
    std::unique_ptr<IServiceDriver> created = createDriver(instance, port, &error);
    if (!created) {
        problem_ = error;
        return nullptr;
    }
    IServiceDriver* raw = created.get();
    drivers_.emplace(id, std::move(created));
    return raw;
}

bool Engine::invalidate(const QString& id) {
    const auto it = drivers_.find(id);
    if (it == drivers_.end()) return true;
    // Destroying a driver destroys its Supervisor, and closing the job handle is what kills
    // everything inside it (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE). Dropping a live one would
    // look like a crash of the service, so callers must stop() first.
    if (it->second->status().state != RunState::Stopped) {
        problem_ = QStringLiteral("%1 must be stopped before its driver is rebuilt").arg(id);
        log::error(QStringLiteral("engine: %1").arg(problem_));
        return false;
    }
    drivers_.erase(id);
    return true;
}

// ------------------------------------------------------------------ registry passthrough
QVector<ServiceInstance> Engine::instances() const { return registry_->instances(); }

QVector<ServiceInstance> Engine::ofKind(const QString& kind) const {
    return registry_->ofKind(kind);
}

QStringList Engine::kindsPresent() const { return registry_->kindsPresent(); }

bool Engine::find(const QString& id, ServiceInstance* out) const {
    return registry_->find(id, out);
}

const ServiceInstance* Engine::defaultOf(const QString& kind) const {
    return registry_->defaultOf(kind);
}

int Engine::portOf(const QString& id) const {
    ServiceInstance instance;
    if (!registry_->find(id, &instance)) return 0;
    return ports_->portOf(instance.portKey);
}

// ------------------------------------------------------------------------------- actions
bool Engine::start(const QString& id, QString* error) {
    IServiceDriver* drv = driver(id);
    if (!drv) {
        if (error) *error = problem_;
        return false;
    }
    if (!drv->start(error)) {
        log::error(QStringLiteral("engine: start %1 failed: %2")
                       .arg(id, error ? *error : QStringLiteral("no reason given")));
        emit instanceChanged(id, runStateKey(RunState::Crashed));
        return false;
    }
    log::info(QStringLiteral("engine: %1 starting on port %2").arg(id).arg(drv->port()));
    emit instanceChanged(id, runStateKey(RunState::Starting));
    return true;
}

bool Engine::stop(const QString& id, QString* error) {
    const auto it = drivers_.find(id);
    if (it == drivers_.end()) return true;    // never started: already stopped
    QString reason;
    if (!it->second->stop(&reason)) {
        if (error) *error = reason;
        log::error(QStringLiteral("engine: stop %1 failed: %2").arg(id, reason));
        return false;
    }
    emit instanceChanged(id, runStateKey(RunState::Stopped));
    return true;
}

bool Engine::restart(const QString& id, QString* error) {
    if (!stop(id, error)) return false;
    return start(id, error);
}

bool Engine::reload(const QString& id, QString* error) {
    const auto it = drivers_.find(id);
    if (it == drivers_.end()) {
        if (error) *error = QStringLiteral("%1 没有运行，无法热重载").arg(id);
        return false;
    }
    QString reason;
    if (!it->second->reload(&reason)) {
        if (error) *error = reason;
        log::warn(QStringLiteral("engine: reload %1 failed: %2").arg(id, reason));
        return false;
    }
    log::info(QStringLiteral("engine: %1 reloaded its configuration").arg(id));
    return true;
}

bool Engine::isRunning(const QString& id) const {
    const auto it = drivers_.find(id);
    return it != drivers_.end() && it->second->status().state == RunState::Running;
}

QVector<net::PortOccupier> Engine::siblingPanelProcesses() const {
    return net::siblingPanelListeners(paths::installDir());
}

QStringList Engine::stopSiblingPanels() {
    QStringList stopped;
#ifdef Q_OS_WIN
    const QVector<net::PortOccupier> siblings = net::siblingPanelListeners(paths::installDir());
    for (const net::PortOccupier& item : siblings) {
        HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(item.pid));
        if (!process) continue;
        const bool ok = TerminateProcess(process, 0) != 0;
        CloseHandle(process);
        if (ok) {
            log::warn(QStringLiteral("engine: stopped sibling devdatapod process %1 (pid %2, port %3)")
                          .arg(item.processName)
                          .arg(item.pid)
                          .arg(item.port));
            stopped.append(QStringLiteral("%1 (pid %2, 端口 %3)")
                               .arg(item.processName)
                               .arg(item.pid)
                               .arg(item.port));
        }
    }
#endif
    return stopped;
}

ServiceStatus Engine::statusOf(const QString& id) {
    ServiceStatus status;
    IServiceDriver* drv = driver(id);
    if (!drv) {
        status.state = RunState::Idle;
        status.detail = problem_;
        return status;
    }
    return drv->status();
}

QVector<Engine::Row> Engine::rows() {
    QVector<Row> out;
    const QVector<ServiceInstance> all = registry_->instances();
    out.reserve(all.size());
    for (const ServiceInstance& instance : all) {
        Row row;
        row.instance = instance;
        row.port = ports_->portOf(instance.portKey);
        IServiceDriver* drv = driver(instance.id);
        if (drv) {
            const ServiceStatus status = drv->status();
            row.state = status.state;
            row.pid = status.pid;
            row.detail = status.detail;
        } else {
            // No driver means we cannot observe it - not that it is stopped. Idle is the
            // state that means "installed but nothing to run yet", which is close enough to
            // show and problem() carries the real reason.
            row.state = RunState::Idle;
            row.detail = problem_;
        }
        out.append(row);
    }
    return out;
}

bool Engine::startKind(const QString& kind, QString* error) {
    const ServiceInstance* target = registry_->defaultOf(kind);
    if (!target) {
        if (error) {
            *error = QStringLiteral("%1 has no default instance installed").arg(kinds::label(kind));
        }
        return false;
    }
    return start(target->id, error);
}

bool Engine::stopKind(const QString& kind, QString* error) {
    const ServiceInstance* target = registry_->defaultOf(kind);
    if (!target) {
        if (error) *error = QStringLiteral("%1 has no default instance").arg(kinds::label(kind));
        return false;
    }
    return stop(target->id, error);
}

bool Engine::restartKind(const QString& kind, QString* error) {
    const ServiceInstance* target = registry_->defaultOf(kind);
    if (!target) {
        if (error) *error = QStringLiteral("%1 has no default instance").arg(kinds::label(kind));
        return false;
    }
    return restart(target->id, error);
}

void Engine::stopAll() {
    // Reverse install order: a php-fpm stopped after the nginx pointing at it produces fewer
    // error lines in the log than the other way round.
    QVector<ServiceInstance> all = registry_->instances();
    for (int i = all.size() - 1; i >= 0; --i) {
        const QString& id = all.at(i).id;
        if (drivers_.find(id) == drivers_.end()) continue;
        QString error;
        if (!stop(id, &error)) {
            log::error(QStringLiteral("engine: %1 survived shutdown: %2").arg(id, error));
        }
    }
}

}
