#pragma once

#include "core/model/RunState.h"
#include "core/service/IServiceDriver.h"
#include "core/service/InstanceRegistry.h"
#include "core/service/ServiceInstance.h"

#include <QObject>
#include <QString>
#include <QVector>

#include <memory>
#include <unordered_map>

namespace devdatapod {

class PortAllocator;

// The single owner of the instance model, and the only object that resolves a kind-level
// request. IServiceDriver deliberately has no "start MySQL": a vague request becomes
//   kind -> registry.defaultOf(kind) -> port from the allocator -> that instance's driver
// right here, so the ambiguity never reaches a driver (docs/ARCHITECTURE.md 模型落点).
//
// A QObject so widgets can subscribe, but it includes nothing from QtWidgets/QtQuick
// (AGENTS.md rule 3) - v1.1 puts this behind an IPC boundary and only adds a proxy.
class Engine : public QObject {
    Q_OBJECT
public:
    // Neither owner is transferred: main.cpp keeps both alive, because the settings page
    // reads the raw band table straight off the allocator.
    Engine(PortAllocator* ports, InstanceRegistry* registry, QObject* parent = nullptr);

    // Reconcile services.json against ports.json. Every instance gets an allocation - the
    // kind's canonical port when it is the default, a slot port otherwise - and a role that
    // disagrees between the two files is repaired with one swapRoles() write.
    //
    // Refuses when no counterpart exists to swap with: inventing a port there is how two
    // services end up fighting for 3306. Returns false with the reason; the panel then runs
    // read-only and shows the problem instead of half-working.
    bool synchronize(QString* error);

    bool isReady() const { return ready_; }
    QString problem() const { return problem_; }

    // ------------------------------------------------------------- registry passthrough
    QVector<ServiceInstance> instances() const;
    QVector<ServiceInstance> ofKind(const QString& kind) const;
    QStringList kindsPresent() const;
    bool find(const QString& id, ServiceInstance* out) const;
    const ServiceInstance* defaultOf(const QString& kind) const;
    // 0 when the instance owns no socket or has no allocation yet.
    int portOf(const QString& id) const;

    // ------------------------------------------------------------------ driver lifecycle
    // Created on first use and cached, because a Supervisor owns a job object and rebuilding
    // it would forget the running child. Returns nullptr for a kind whose driver has not
    // been written yet (P3) or when no port could be resolved.
    IServiceDriver* driver(const QString& id);
    // Drop the cached driver so the next driver() rebuilds it from the current registry
    // entry and the current allocation. Every role or port change must go through this,
    // otherwise a switched instance keeps running with the port it no longer owns.
    //
    // Returns false for a live instance: destroying a driver destroys its Supervisor, and
    // closing the job handle is exactly what kills the service. stop() first.
    bool invalidate(const QString& id);

    // Kind-level: acts on that kind's default instance. This is what the row button does.
    bool startKind(const QString& kind, QString* error = nullptr);
    bool stopKind(const QString& kind, QString* error = nullptr);
    bool restartKind(const QString& kind, QString* error = nullptr);

    bool start(const QString& id, QString* error = nullptr);
    bool stop(const QString& id, QString* error = nullptr);
    bool restart(const QString& id, QString* error = nullptr);
    // Re-read configuration in place (no restart). Drivers that cannot do it say so.
    bool reload(const QString& id, QString* error = nullptr);
    bool isRunning(const QString& id) const;
    ServiceStatus statusOf(const QString& id);

    // Stopped, starting, running ... for every instance. The one call the service list and
    // the status bar poll.
    struct Row {
        ServiceInstance instance;
        int port = 0;
        RunState state = RunState::Stopped;
        qint64 pid = 0;
        QString detail;
    };
    QVector<Row> rows();

    // Stop everything we started, in reverse install order. Called on quit; the job objects
    // are the crash-path backstop, this is the polite one.
    void stopAll();

    PortAllocator* ports() const { return ports_; }
    InstanceRegistry* registry() const { return registry_; }

signals:
    // stateKey is runStateKey(state) - the same spelling QSS matches on, so a signal-driven
    // refresh and a style refresh cannot disagree about what "starting" is called.
    void instanceChanged(const QString& id, const QString& stateKey);
    void instancesChanged();

private:
    std::unique_ptr<IServiceDriver> createDriver(const ServiceInstance& instance, int port,
                                                 QString* error);
    // Give one instance the allocation its role calls for. *port stays 0 for a kind that
    // owns no socket, so the bool is the only failure signal.
    bool ensureAllocation(const ServiceInstance& instance, int* port, QString* error);
    // Repair services.json vs ports.json role drift for one kind.
    // 1 = swapped, 0 = nothing to do, -1 = not repairable here (*error carries the reason).
    int healRoleDrift(const QString& kind, QString* error);

    PortAllocator* ports_;
    InstanceRegistry* registry_;
    std::unordered_map<QString, std::unique_ptr<IServiceDriver>> drivers_;
    bool ready_ = false;
    QString problem_;
};

}
