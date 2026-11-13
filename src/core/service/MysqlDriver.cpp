#include "core/service/MysqlDriver.h"

#include "core/config/ManagedBlock.h"
#include "core/process/RunOnce.h"
#include "core/util/AppPaths.h"
#include "core/util/AtomicFile.h"
#include "core/util/Log.h"
#include "core/util/LogTailReader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>
#include <QThread>

namespace devdatapod {

namespace {
constexpr int kReadyTimeoutMs = 15000;   // 初始化 + InnoDB 恢复比 redis 慢，多给点时间
constexpr int kProbeTimeoutMs = 500;
constexpr int kStopGraceMs = 8000;       // InnoDB 刷盘比 redis 慢，优雅停止多等一会

// 统一转成 forward-slash 路径：my.ini 里写反斜杠要转义，正斜杠 MySQL 直接认。
QString toIniPath(const QString& path) {
    QString out = QDir::fromNativeSeparators(path);
    out.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return out;
}
}

MysqlSettings MysqlSettings::load(const QString& dataDir) {
    MysqlSettings out;
    if (dataDir.isEmpty()) return out;
    const QString file = dataDir + QStringLiteral("/mysql.settings.json");
    if (!QFileInfo::exists(file)) return out;

    QByteArray raw;
    QString error;
    if (!file::readAll(file, &raw, &error)) {
        log::warn(QStringLiteral("mysql: %1").arg(error));
        return out;
    }
    QJsonParseError parse{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
        log::warn(QStringLiteral("mysql: 忽略 %1（%2）").arg(file, parse.errorString()));
        return out;
    }
    // 只认已知的键：文件里写错名字不会悄悄改变渲染进 my.ini 的内容（P4 换成 schema 引擎）。
    const QJsonObject object = doc.object();
    out.bind = object.value(QStringLiteral("bind")).toString(out.bind);
    out.maxConnections = object.value(QStringLiteral("maxConnections")).toInt(out.maxConnections);
    out.charset = object.value(QStringLiteral("charset")).toString(out.charset);
    out.storageEngine = object.value(QStringLiteral("storageEngine")).toString(out.storageEngine);
    out.innodbBufferPoolMb =
        object.value(QStringLiteral("innodbBufferPoolMb")).toInt(out.innodbBufferPoolMb);
    out.maxAllowedPacketMb =
        object.value(QStringLiteral("maxAllowedPacketMb")).toInt(out.maxAllowedPacketMb);
    out.lowerCaseTableNames =
        object.value(QStringLiteral("lowerCaseTableNames")).toInt(out.lowerCaseTableNames);
    out.rootPassword = object.value(QStringLiteral("rootPassword")).toString(out.rootPassword);
    return out;
}

bool MysqlSettings::save(const QString& dataDir) const {
    if (dataDir.isEmpty()) return false;
    QJsonObject object;
    object.insert(QStringLiteral("bind"), bind);
    object.insert(QStringLiteral("maxConnections"), maxConnections);
    object.insert(QStringLiteral("charset"), charset);
    object.insert(QStringLiteral("storageEngine"), storageEngine);
    object.insert(QStringLiteral("innodbBufferPoolMb"), innodbBufferPoolMb);
    object.insert(QStringLiteral("maxAllowedPacketMb"), maxAllowedPacketMb);
    object.insert(QStringLiteral("lowerCaseTableNames"), lowerCaseTableNames);
    object.insert(QStringLiteral("rootPassword"), rootPassword);
    const QString file = dataDir + QStringLiteral("/mysql.settings.json");
    return file::writeAtomically(file, QJsonDocument(object).toJson(QJsonDocument::Indented));
}

MysqlDriver::MysqlDriver(const ServiceInstance& instance, int port)
    : IServiceDriver(instance, port),
      settings_(MysqlSettings::load(instance.dataDir.isEmpty()
                                        ? QString()
                                        : paths::installDir() + QLatin1Char('/')
                                              + instance.dataDir)),
      supervisor_(instance.id) {}

// ----------------------------------------------------------------------------- 路径
QString MysqlDriver::absoluteDataDir() const {
    if (instance_.dataDir.isEmpty()) return QString();
    return paths::installDir() + QLatin1Char('/') + instance_.dataDir;
}

QString MysqlDriver::iniPath() const {
    return absoluteDataDir() + QStringLiteral("/my.ini");
}

QString MysqlDriver::logPath() const {
    return absoluteDataDir() + QStringLiteral("/mysql-error.log");
}

QString MysqlDriver::findExecutable(const QString& name) const {
    if (instance_.installPath.isEmpty()) return QString();
    const QString root = paths::installDir() + QLatin1Char('/') + instance_.installPath;
#ifdef Q_OS_WIN
    const QString file = name + QStringLiteral(".exe");
#else
    const QString file = name;
#endif
    // 组件实物里可执行文件都在 bin/ 下；根目录也试一下，兼容别的打包方式。
    const QStringList candidates{root + QStringLiteral("/bin/") + file,
                                 root + QLatin1Char('/') + file};
    for (const QString& candidate : candidates) {
        if (QFileInfo::exists(candidate)) return QFileInfo(candidate).absoluteFilePath();
    }
    return QString();
}

bool MysqlDriver::isVersion8() const {
    // 8.0 及以上（含 9.x）默认认证插件都是 caching_sha2_password，统一退回 mysql_native_password
    const QString v = instance_.version;
    return v.startsWith(QLatin1Char('8')) || v.startsWith(QLatin1Char('9'));
}

// ----------------------------------------------------------------------------- 生命周期
Detection MysqlDriver::detect() {
    Detection out;
    out.executable = findExecutable(QStringLiteral("mysqld"));
    if (out.executable.isEmpty()) {
        out.message = QStringLiteral("%1 下找不到 mysqld")
                          .arg(instance_.installPath.isEmpty()
                                   ? QStringLiteral("<无安装路径>")
                                   : instance_.installPath);
        return out;
    }
    const proc::CommandResult result =
        proc::runOnce(out.executable, {QStringLiteral("--version")});
    out.installed = result.ran;
    out.version = proc::extractVersion(result.output);
    if (out.version.isEmpty()) out.version = instance_.version;
    out.message = result.ran ? result.output.trimmed()
                             : QStringLiteral("%1 无法运行").arg(out.executable);
    return out;
}

QString MysqlDriver::renderBlock() const {
    const QString dataDir = toIniPath(absoluteDataDir());
    const QString baseDir = toIniPath(paths::installDir() + QLatin1Char('/')
                                      + instance_.installPath);
    QStringList lines;
    lines << QStringLiteral("[mysqld]");
    lines << QStringLiteral("# 由 devdatapod 按实例设置渲染");
    lines << QStringLiteral("bind-address=%1").arg(settings_.bind);
    lines << QStringLiteral("port=%1").arg(port_);
    lines << QStringLiteral("basedir=%1").arg(baseDir);
    lines << QStringLiteral("datadir=%1").arg(dataDir);
    lines << QStringLiteral("character-set-server=%1").arg(settings_.charset);
    lines << QStringLiteral("default-storage-engine=%1").arg(settings_.storageEngine);
    lines << QStringLiteral("max_connections=%1").arg(settings_.maxConnections);
    lines << QStringLiteral("max_allowed_packet=%1M").arg(settings_.maxAllowedPacketMb);
    lines << QStringLiteral("lower_case_table_names=%1").arg(settings_.lowerCaseTableNames);
    lines << QStringLiteral("innodb_buffer_pool_size=%1M").arg(settings_.innodbBufferPoolMb);
    // 错误日志落到实例自己的 datadir，面板日志视图才有东西可读。
    lines << QStringLiteral("log-error=%1").arg(toIniPath(logPath()));
    // 认证插件按版本区分：5.7 默认就是 mysql_native_password 不用管；
    // 8.0.x 用 default_authentication_plugin（8.4 起移除该选项）；
    // 8.4 不再默认加载 mysql_native_password，需要显式开启插件；
    // 9.x 彻底移除该插件，只能走 caching_sha2_password，不写任何配置。
    if (isVersion8()) {
        const QString v = instance_.version;
        if (v.startsWith(QLatin1String("8.0"))) {
            lines << QStringLiteral("default_authentication_plugin=mysql_native_password");
        } else if (v.startsWith(QLatin1String("8"))) {
            lines << QStringLiteral("mysql_native_password=ON");
        }
    }
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

bool MysqlDriver::prepareConfig(QString* error) {
    const QString dataDir = absoluteDataDir();
    if (dataDir.isEmpty()) {
        if (error) *error = QStringLiteral("%1 没有数据目录").arg(instance_.id);
        return false;
    }
    QDir dir;
    if (!dir.mkpath(dataDir)) {
        if (error) *error = QStringLiteral("无法创建 %1").arg(dataDir);
        return false;
    }
    const QString ini = iniPath();
    switch (config::managed::apply(ini, renderBlock(), error)) {
    case config::BlockOutcome::Written:
        return true;
    case config::BlockOutcome::Preserved:
        // 文件是用户的，端口可能不是我们的。仍允许启动 —— status() 会把不一致报出来。
        log::warn(QStringLiteral("mysql/%1: %2 没有 devdatapod 托管块，按原样使用")
                      .arg(instance_.id, ini));
        return true;
    case config::BlockOutcome::Failed:
        return false;
    }
    return false;
}

bool MysqlDriver::datadirInitialized() const {
    // 初始化过的标志是 mysql 系统库目录存在（里面有 user 表等）。
    return QFileInfo(absoluteDataDir() + QStringLiteral("/mysql")).isDir();
}

bool MysqlDriver::initializeDatadir(QString* error) {
    const QString program = findExecutable(QStringLiteral("mysqld"));
    if (program.isEmpty()) {
        if (error) *error = QStringLiteral("%1 未安装").arg(instance_.id);
        return false;
    }
    log::info(QStringLiteral("mysql/%1: 初始化数据目录 %2").arg(instance_.id, absoluteDataDir()));
    // mysqld --initialize 要求 datadir 完全为空，而我们的 my.ini 就写在 datadir 里，
    // 所以初始化不能用 --defaults-file 指向它，改成命令行直接传参。
    // lower_case_table_names 必须在初始化时定死（事后改它要重建整个 datadir）。
    const QString baseDir = toIniPath(paths::installDir() + QLatin1Char('/')
                                      + instance_.installPath);
    const proc::CommandResult result = proc::runOnce(
        program,
        {QStringLiteral("--basedir=%1").arg(baseDir),
         QStringLiteral("--datadir=%1").arg(toIniPath(absoluteDataDir())),
         QStringLiteral("--lower_case_table_names=%1").arg(settings_.lowerCaseTableNames),
         QStringLiteral("--initialize-insecure"), QStringLiteral("--console")},
        60000);    // 初始化要建 InnoDB 系统表，远超 runOnce 默认的 5 秒
    if (!result.ran || result.exitCode != 0) {
        const QString why = result.errorOutput.trimmed().isEmpty()
                                ? result.output.trimmed()
                                : result.errorOutput.trimmed();
        if (error) *error = QStringLiteral("初始化失败：%1").arg(why);
        return false;
    }
    if (!datadirInitialized()) {
        if (error) {
            *error = QStringLiteral("初始化后 mysql 系统库仍不存在：%1")
                         .arg(result.output.trimmed());
        }
        return false;
    }
    justInitialized_ = true;
    return true;
}

bool MysqlDriver::start(QString* error) {
    const QString program = findExecutable(QStringLiteral("mysqld"));
    if (program.isEmpty()) {
        if (error) *error = QStringLiteral("%1 未安装").arg(instance_.id);
        return false;
    }
    // 先初始化再写 my.ini：--initialize 要求 datadir 为空，而 my.ini 就放在 datadir 里。
    if (!datadirInitialized() && !initializeDatadir(error)) return false;
    // 每次启动都重渲染：端口来自分配器，切默认会变，旧 my.ini 可能写着旧端口（规则 2）。
    if (!prepareConfig(error)) return false;
    if (!supervisor_.isValid()) {
        if (error) *error = QStringLiteral("%1").arg(supervisor_.problem());
        return false;
    }
    supervisor_.setWorkingDirectory(absoluteDataDir());
    supervisor_.setLogPath(logPath());
    stoppedByUs_ = false;
    // --console：前台运行并输出到 stdout。绝不能让它后台化，否则 Job Object 管不住，
    // 面板崩了就留下孤儿 mysqld（规则 5）。
    if (!supervisor_.start(program,
                           {QStringLiteral("--defaults-file=%1").arg(iniPath()),
                            QStringLiteral("--console")},
                           error)) {
        return false;
    }
    lastStart_ = QDateTime::currentDateTime();
    sampler_.reset();
    // 刚初始化完的实例 root 是空密码，趁这次启动把它设成 settings_.rootPassword。
    if (justInitialized_) {
        justInitialized_ = false;
        if (!ensureRootPassword(error)) {
            QString stopError;
            stop(&stopError);    // 设不了密码的裸实例不留着
            return false;
        }
    }
    return true;
}

// 等服务真正能答握手（初始化 + InnoDB 恢复要几秒），然后用空密码 root 连上设密码。
bool MysqlDriver::ensureRootPassword(QString* error) {
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + kReadyTimeoutMs;
    QString detail;
    bool up = false;
    while (QDateTime::currentMSecsSinceEpoch() < deadline) {
        if (probeHandshake(&detail)) { up = true; break; }
        QThread::msleep(200);
    }
    if (!up) {
        if (error) *error = QStringLiteral("服务没在 %1 秒内就绪：%2").arg(kReadyTimeoutMs / 1000).arg(detail);
        return false;
    }
    const QString mysql = findExecutable(QStringLiteral("mysql"));
    if (mysql.isEmpty()) {
        if (error) *error = QStringLiteral("找不到 mysql 客户端，无法设置 root 密码");
        return false;
    }
    // 此刻 root 还是空密码（--initialize-insecure），直接连。设完密码再连就要带密码了。
    // 8.0 把 root 建成 caching_sha2_password，老客户端（Adminer / 老 PHP）连不上，
    // 连 plugin 一起改成 mysql_native_password。9.x 移除了该插件，只能用 caching_sha2_password。
    const QString plugin = isVersion8() && !instance_.version.startsWith(QLatin1String("9"))
                               ? QStringLiteral("mysql_native_password")
                               : QStringLiteral("caching_sha2_password");
    const QString sql =
        QStringLiteral("ALTER USER 'root'@'localhost' IDENTIFIED WITH %1 BY '%2'; "
                       "CREATE USER IF NOT EXISTS 'root'@'127.0.0.1' IDENTIFIED WITH %1 BY '%2'; "
                       "GRANT ALL PRIVILEGES ON *.* TO 'root'@'127.0.0.1' WITH GRANT OPTION; "
                       "FLUSH PRIVILEGES;")
            .arg(plugin, settings_.rootPassword);
    const proc::CommandResult result = proc::runOnce(
        mysql, {QStringLiteral("--port=%1").arg(port_), QStringLiteral("--user=root"),
                QStringLiteral("--host=127.0.0.1"), QStringLiteral("-e"), sql});
    // runOnce 的 ran 只表示进程起没起来，SQL 错不错要看退出码；mysql 的报错在 stderr。
    if (!result.ran || result.exitCode != 0) {
        if (error) {
            *error = QStringLiteral("设置 root 密码失败：%1")
                         .arg(result.errorOutput.trimmed().isEmpty()
                                  ? result.output.trimmed()
                                  : result.errorOutput.trimmed());
        }
        return false;
    }
    log::info(QStringLiteral("mysql/%1: root 密码已设置").arg(instance_.id));
    return true;
}

bool MysqlDriver::probeHandshake(QString* detail) {
    QTcpSocket socket;
    QHostAddress address;
    if (!address.setAddress(settings_.bind)) address.setAddress(QHostAddress::LocalHost);
    socket.connectToHost(address, quint16(port_));
    if (!socket.waitForConnected(kProbeTimeoutMs)) {
        if (detail) {
            *detail = QStringLiteral("%1:%2 没有监听（%3）")
                          .arg(settings_.bind)
                          .arg(port_)
                          .arg(socket.errorString());
        }
        return false;
    }
    // MySQL 服务器一连上就主动发握手包，不用我们先说话。能读到就证明协议层活着。
    if (!socket.waitForReadyRead(kProbeTimeoutMs)) {
        if (detail) *detail = QStringLiteral("%1:%2 接受了连接但没发握手包").arg(settings_.bind).arg(port_);
        return false;
    }
    const QByteArray greeting = socket.readAll();
    // 握手包：3 字节长度 + 1 字节序号 + 1 字节协议版本（应为 10）。
    if (greeting.size() < 5 || quint8(greeting.at(4)) != 10) {
        if (detail) *detail = QStringLiteral("%1:%2 发的不是 MySQL 握手包").arg(settings_.bind).arg(port_);
        return false;
    }
    if (detail) *detail = QString::fromUtf8(greeting.mid(5).split('\0').first());
    return true;
}

bool MysqlDriver::stop(QString* error) {
    stoppedByUs_ = true;
    if (!supervisor_.isRunning()) return true;

    // 先用 mysqladmin 优雅关闭，让 InnoDB 把脏页刷盘（docs/ARCHITECTURE.md）。
    // root 已设密码，不带密码 mysqladmin 连不上，shutdown 会静默失败 → 等超时强杀，很慢。
    const QString admin = findExecutable(QStringLiteral("mysqladmin"));
    if (!admin.isEmpty()) {
        proc::runOnce(admin, {QStringLiteral("--port=%1").arg(port_),
                              QStringLiteral("--user=root"),
                              QStringLiteral("--password=%1").arg(settings_.rootPassword),
                              QStringLiteral("shutdown")});
    }
    if (supervisor_.waitFinished(kStopGraceMs)) return true;

    // mysqladmin 不在或没生效，再升级到信号 / 强杀。
    supervisor_.signalGraceful();
    if (supervisor_.waitFinished(2000)) return true;
    if (supervisor_.killForced()) {
        log::warn(QStringLiteral("mysql/%1: mysqladmin 没停掉，已强杀").arg(instance_.id));
        return true;
    }
    if (error) {
        *error = QStringLiteral("%1 在 mysqladmin、信号、强杀之后仍在运行").arg(instance_.id);
    }
    return false;
}

ServiceStatus MysqlDriver::status() {
    ServiceStatus status;
    if (!supervisor_.isRunning()) {
        if (stoppedByUs_ || !lastStart_.isValid()) {
            status.state = RunState::Stopped;
            return status;
        }
        status.state = RunState::Crashed;
        status.detail = QStringLiteral("退出码 %1").arg(supervisor_.exitCode());
        return status;
    }
    status.pid = supervisor_.pid();
    QString detail;
    if (probeHandshake(&detail)) {
        status.state = RunState::Running;
        return status;
    }
    status.detail = detail;
    const qint64 waited =
        lastStart_.isValid() ? lastStart_.msecsTo(QDateTime::currentDateTime())
                             : qint64(kReadyTimeoutMs) + 1;
    // 进程在但不发握手包，只在初始化 / InnoDB 恢复的时间内算「启动中」，
    // 超时就是坏了（端口被占、datadir 版本不对、配置错误……）。
    status.state = waited < kReadyTimeoutMs ? RunState::Starting : RunState::Crashed;
    return status;
}

LogTail MysqlDriver::logs(int lines) {
    return text::readTail(logPath(), lines);
}

ServiceStats MysqlDriver::stats() {
    ServiceStats out;
    const proc::ResourceSample sample = sampler_.sample(supervisor_.members());
    out.valid = sample.valid;
    out.cpuPercent = sample.cpuPercent;
    out.rssBytes = sample.rssBytes;
    if (lastStart_.isValid() && supervisor_.isRunning()) {
        out.uptimeMs = lastStart_.msecsTo(QDateTime::currentDateTime());
    }
    return out;
}

}
