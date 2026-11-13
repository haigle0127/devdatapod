#pragma once

#include "core/process/ProcessStat.h"
#include "core/process/Supervisor.h"
#include "core/service/IServiceDriver.h"

namespace devdatapod {

// 设计稿里 Nginx 配置弹窗要展示的字段。port 不在这里：端口归 PortAllocator 管，
// 不属于单个实例的设置文件（AGENTS.md 规则 2）。
struct NginxSettings {
    int workerProcesses = 1;
    int workerConnections = 1024;
    // 站点根目录。默认 WWW（安装根目录下的共享站点目录，和小皮的用法一致）；
    // 填绝对路径时按绝对路径用，填相对路径时相对安装根目录解析。
    QString rootDir = QStringLiteral("WWW");
    QString indexFiles = QStringLiteral("index.html index.htm");
    bool accessLog = true;
    bool gzip = true;

    static NginxSettings load(const QString& dataDir);
    bool save(const QString& dataDir) const;
};

// Nginx 驱动。和 Redis / MySQL 同一套实例模型，Nginx 独有的三件事：
//  1. 配置是 nginx.conf 的完整语法块（http/server 嵌套），不能像 redis.conf 那样拍平，
//     托管块标记之外的内容原样保留（规则 6）。
//  2. 停止走 `nginx -s quit`（优雅关闭，等 worker 收尾），不是信号。
//  3. Windows 版 nginx 起停都靠 `-p <prefix>` 定位 conf/logs/pid，prefix 固定用实例
//     datadir，Extensions 目录保持只读的组件实物（规则 10：datadir 按实例隔离）。
class NginxDriver : public IServiceDriver {
public:
    NginxDriver(const ServiceInstance& instance, int port);

    Detection detect() override;
    bool prepareConfig(QString* error) override;
    bool start(QString* error) override;
    bool stop(QString* error) override;
    ServiceStatus status() override;
    LogTail logs(int lines) override;
    ServiceStats stats() override;
    bool reload(QString* error) override;

    QString confPath() const;
    QString logPath() const;          // error.log，面板日志视图读它
    QString accessLogPath() const;
    QString absoluteRootDir() const;
    const NginxSettings& settings() const { return settings_; }

private:
    QString absoluteDataDir() const;
    QString prefixDir() const;        // `-p` 的落点：实例 datadir
    QString findExecutable() const;
    QString renderBlock() const;
    void appendSiteServers(QStringList* lines) const;    // 网站页的 vhost 渲染进托管块
    bool probeHttp(QString* detail) const;
    // 站点目录里一个 index 都没有时放一张欢迎页，第一次启动就能看到「已经通了」。
    void ensureWelcomePage() const;

    NginxSettings settings_;
    proc::Supervisor supervisor_;
    proc::ResourceSampler sampler_;
    bool stoppedByUs_ = false;
    QDateTime lastStart_;
};

} // namespace devdatapod
