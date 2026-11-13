#pragma once

#include "core/process/ProcessStat.h"
#include "core/process/Supervisor.h"
#include "core/service/IServiceDriver.h"

namespace devdatapod {

// The fields the design mock shows in the Redis settings dialog. `port` is deliberately
// absent: it belongs to the allocator, not to a per-instance setting file (rule 2).
struct RedisSettings {
    QString bind = QStringLiteral("127.0.0.1");
    int timeout = 0;
    int keepAlive = 300;
    int maxClients = 10000;
    int databases = 16;
    QString password;
    int maxMemoryMb = 256;
    QString maxMemoryPolicy = QStringLiteral("noeviction");
    // SHUTDOWN SAVE first, NOSAVE only if persisting fails.
    bool persistOnShutdown = true;

    static RedisSettings load(const QString& dataDir);
    bool save(const QString& dataDir) const;
};

// The first driver, and the cheapest place to prove the whole instance model: two Redis
// versions side by side, switching which one owns the canonical port (docs/ROADMAP.md P2).
//
// The 3.0.504 branch is the reason this driver is not just a template: it has no HELLO, no
// ACL, and no recent maxmemory-policy values, so the renderer must be able to leave fields
// out entirely instead of writing them and hoping.
class RedisDriver : public IServiceDriver {
public:
    RedisDriver(const ServiceInstance& instance, int port);

    Detection detect() override;
    bool prepareConfig(QString* error) override;
    bool start(QString* error) override;
    bool stop(QString* error) override;
    ServiceStatus status() override;
    LogTail logs(int lines) override;
    ServiceStats stats() override;

    QString confPath() const;
    QString logPath() const;
    const RedisSettings& settings() const { return settings_; }

private:
    QString absoluteDataDir() const;
    QString findExecutable() const;
    bool versionAtLeast(int major, int minor) const;
    QString renderBlock() const;
    bool probe(const QStringList& verbs, QString* detail);

    RedisSettings settings_;
    proc::Supervisor supervisor_;
    proc::ResourceSampler sampler_;
    bool stoppedByUs_ = false;
    QDateTime lastStart_;
};

}
