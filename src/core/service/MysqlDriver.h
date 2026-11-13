#pragma once

#include "core/process/ProcessStat.h"
#include "core/process/Supervisor.h"
#include "core/service/IServiceDriver.h"

namespace devdatapod {

// 设计稿里 MySQL 设置弹窗要展示的字段。port 故意不在这里：端口归 PortAllocator 管，
// 不属于单个实例的设置文件（规则 2）。
struct MysqlSettings {
    QString bind = QStringLiteral("127.0.0.1");
    int maxConnections = 100;
    QString charset = QStringLiteral("utf8mb4");
    QString storageEngine = QStringLiteral("InnoDB");
    int innodbBufferPoolMb = 64;
    int maxAllowedPacketMb = 16;
    // 大小写敏感：Windows 默认 1（存小写），类 Unix 默认 0。初始化后改它要重建 datadir。
    int lowerCaseTableNames = 1;
    // root 的密码。本地开发默认 root/root，初始化后由驱动负责把它设进系统表。
    QString rootPassword = QStringLiteral("root");

    static MysqlSettings load(const QString& dataDir);
    bool save(const QString& dataDir) const;
};

// MySQL 驱动。和 RedisDriver 同一套实例模型，但要处理三件 MySQL 独有的事：
//  1. datadir 首次为空时必须跑 `mysqld --initialize-insecure` 初始化系统表，
//     否则 mysqld 起不来（规则 10：datadir 按实例隔离，8.0 的目录 5.7 打不开）。
//  2. 优雅停止走 `mysqladmin shutdown`，不是 redis 的 SHUTDOWN 行内命令。
//  3. 5.7 与 8.0 的配置项有差异（8.0 移除了 query_cache 等），渲染时要按版本取舍。
class MysqlDriver : public IServiceDriver {
public:
    MysqlDriver(const ServiceInstance& instance, int port);

    Detection detect() override;
    bool prepareConfig(QString* error) override;
    bool start(QString* error) override;
    bool stop(QString* error) override;
    ServiceStatus status() override;
    LogTail logs(int lines) override;
    ServiceStats stats() override;

    QString iniPath() const;
    QString logPath() const;
    const MysqlSettings& settings() const { return settings_; }

private:
    QString absoluteDataDir() const;
    QString findExecutable(const QString& name) const;   // 在 bin/ 下找 mysqld / mysqladmin
    bool isVersion8() const;                             // 8.0+（含 9.x）与 5.7 的配置项不同
    QString renderBlock() const;
    bool datadirInitialized() const;                     // 看 mysql 系统库目录在不在
    bool initializeDatadir(QString* error);              // mysqld --initialize-insecure
    bool probeHandshake(QString* detail);                // TCP 连上并读 MySQL 握手包
    // 初始化后把 root 密码设成 settings_.rootPassword（默认 root）。只在刚初始化完跑一次。
    bool ensureRootPassword(QString* error);

    MysqlSettings settings_;
    proc::Supervisor supervisor_;
    proc::ResourceSampler sampler_;
    bool stoppedByUs_ = false;
    bool justInitialized_ = false;   // 本次 start 是否刚做完初始化
    QDateTime lastStart_;
};

}
