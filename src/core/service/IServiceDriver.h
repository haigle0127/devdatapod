#pragma once

#include "core/model/RunState.h"
#include "core/service/ServiceInstance.h"
#include "core/util/LogTailReader.h"

#include <QString>
#include <QStringList>

namespace devdatapod {

// What detect() learned about an installed version.
struct Detection {
    bool installed = false;
    QString executable;    // absolute, inside the install root
    QString version;        // as reported by the binary, not as filed
    QString message;
};

struct ServiceStatus {
    RunState state = RunState::Stopped;
    qint64 pid = 0;
    QString detail;         // shown verbatim in the failure tooltip
};

struct ServiceStats {
    bool valid = false;
    double cpuPercent = 0.0;    // across every process in the instance, not system-wide
    qint64 rssBytes = 0;
    qint64 uptimeMs = 0;
};

// text::Tail, under the name the driver interface speaks.
using LogTail = text::Tail;

// The product's heart: one implementation per service kind (redis, nginx, mysql, php,
// python), and every method takes an *instance*. There is deliberately no kind-level
// "start MySQL" entry point - a request that vague is resolved against the registry by
// the caller first (docs/ARCHITECTURE.md 模型落点).
//
// Contract that drivers must honour:
//  - start() returns as soon as the child is spawned. Readiness is decided by status(),
//    which must probe the port and speak the protocol, because "process exists" is not
//    "service is up".
//  - stop() must be graceful first (the service's own shutdown verb) and only escalate to
//    killing the container after a timeout.
//  - Nothing here may touch widgets: core is UI-free (AGENTS.md rule 3).
class IServiceDriver {
public:
    // `port` is what this instance listens on: the canonical port when it is the default,
    // its slot port otherwise, 0 for kinds that own no socket. Callers resolve it through
    // PortAllocator - a driver must never invent a number (rule 2).
    IServiceDriver(const ServiceInstance& instance, int port)
        : instance_(instance), port_(port) {}
    virtual ~IServiceDriver() = default;

    IServiceDriver(const IServiceDriver&) = delete;
    IServiceDriver& operator=(const IServiceDriver&) = delete;

    const ServiceInstance& instance() const { return instance_; }
    QString kind() const { return instance_.kind; }
    int port() const { return port_; }

    // The seven capabilities a driver must implement (docs/ARCHITECTURE.md 服务引擎).
    virtual Detection detect() = 0;
    virtual bool prepareConfig(QString* error) = 0;
    virtual bool start(QString* error) = 0;
    virtual bool stop(QString* error) = 0;
    virtual ServiceStatus status() = 0;
    virtual LogTail logs(int lines) = 0;
    virtual ServiceStats stats() = 0;

    // Optional: make a running service re-read its configuration without dropping
    // connections (nginx's `-s reload`). The default is "not supported", so a driver that
    // has no such verb is not forced to write an empty shell.
    virtual bool reload(QString* error) {
        if (error) *error = QStringLiteral("%1 不支持热重载").arg(instance_.id);
        return false;
    }

protected:
    ServiceInstance instance_;
    int port_ = 0;
};

}
