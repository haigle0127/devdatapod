#include "core/service/RedisDriver.h"

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

namespace devdatapod {

namespace {
constexpr int kReadyTimeoutMs = 10000;
constexpr int kProbeTimeoutMs = 250;
constexpr int kStopGraceMs = 3000;

QString toUrlPath(const QString& path) {
    QString out = QDir::fromNativeSeparators(path);
    out.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return out;
}

QString serverName() {
#ifdef Q_OS_WIN
    return QStringLiteral("redis-server.exe");
#else
    return QStringLiteral("redis-server");
#endif
}

int versionPart(const QString& version, int index) {
    const QStringList parts = version.split(QLatin1Char('.'));
    if (index >= parts.size()) return 0;
    return parts.at(index).left(3).toInt();    // "504" or "2" - stops at any suffix
}
}

RedisSettings RedisSettings::load(const QString& dataDir) {
    RedisSettings out;
    if (dataDir.isEmpty()) return out;
    const QString file = dataDir + QStringLiteral("/redis.settings.json");
    if (!QFileInfo::exists(file)) return out;

    QByteArray raw;
    QString error;
    if (!file::readAll(file, &raw, &error)) {
        log::warn(QStringLiteral("redis: %1").arg(error));
        return out;
    }
    QJsonParseError parse{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
        log::warn(QStringLiteral("redis: ignoring %1 (%2)").arg(file, parse.errorString()));
        return out;
    }
    // P4 replaces this with the schema engine; until then only known keys are read, so a
    // typo in the file cannot silently change what gets rendered into redis.conf.
    const QJsonObject object = doc.object();
    out.bind = object.value(QStringLiteral("bind")).toString(out.bind);
    out.timeout = object.value(QStringLiteral("timeout")).toInt(out.timeout);
    out.keepAlive = object.value(QStringLiteral("tcpKeepAlive")).toInt(out.keepAlive);
    out.maxClients = object.value(QStringLiteral("maxClients")).toInt(out.maxClients);
    out.databases = object.value(QStringLiteral("databases")).toInt(out.databases);
    out.password = object.value(QStringLiteral("password")).toString(out.password);
    out.maxMemoryMb = object.value(QStringLiteral("maxMemoryMb")).toInt(out.maxMemoryMb);
    out.maxMemoryPolicy =
        object.value(QStringLiteral("maxMemoryPolicy")).toString(out.maxMemoryPolicy);
    out.persistOnShutdown =
        object.value(QStringLiteral("persistOnShutdown")).toBool(out.persistOnShutdown);
    return out;
}

bool RedisSettings::save(const QString& dataDir) const {
    if (dataDir.isEmpty()) return false;
    QJsonObject object;
    object.insert(QStringLiteral("bind"), bind);
    object.insert(QStringLiteral("timeout"), timeout);
    object.insert(QStringLiteral("tcpKeepAlive"), keepAlive);
    object.insert(QStringLiteral("maxClients"), maxClients);
    object.insert(QStringLiteral("databases"), databases);
    object.insert(QStringLiteral("password"), password);
    object.insert(QStringLiteral("maxMemoryMb"), maxMemoryMb);
    object.insert(QStringLiteral("maxMemoryPolicy"), maxMemoryPolicy);
    object.insert(QStringLiteral("persistOnShutdown"), persistOnShutdown);
    const QString file = dataDir + QStringLiteral("/redis.settings.json");
    return file::writeAtomically(file, QJsonDocument(object).toJson(QJsonDocument::Indented));
}

RedisDriver::RedisDriver(const ServiceInstance& instance, int port)
    : IServiceDriver(instance, port),
      settings_(RedisSettings::load(instance.dataDir.isEmpty()
                                        ? QString()
                                        : paths::installDir() + QLatin1Char('/')
                                              + instance.dataDir)),
      supervisor_(instance.id) {}

// ------------------------------------------------------------------------------- paths
QString RedisDriver::absoluteDataDir() const {
    if (instance_.dataDir.isEmpty()) return QString();
    return paths::installDir() + QLatin1Char('/') + instance_.dataDir;
}

QString RedisDriver::confPath() const {
    return absoluteDataDir() + QStringLiteral("/redis.conf");
}

QString RedisDriver::logPath() const {
    return absoluteDataDir() + QStringLiteral("/redis.log");
}

QString RedisDriver::findExecutable() const {
    if (instance_.installPath.isEmpty()) return QString();
    const QString root = paths::installDir() + QLatin1Char('/') + instance_.installPath;
    const QString name = serverName();

    QStringList candidates;
    candidates << root + QLatin1Char('/') + name;
    candidates << root + QStringLiteral("/bin/") + name;
    for (const QString& candidate : candidates) {
        if (QFileInfo::exists(candidate)) return QFileInfo(candidate).absoluteFilePath();
    }
    return QString();
}

bool RedisDriver::versionAtLeast(int major, int minor) const {
    const int self = versionPart(instance_.version, 0);
    const int selfMinor = versionPart(instance_.version, 1);
    if (self == 0) return true;    // unknown version: assume the newest template
    if (self != major) return self > major;
    return selfMinor >= minor;
}

// ----------------------------------------------------------------------------- lifecycle
Detection RedisDriver::detect() {
    Detection out;
    out.executable = findExecutable();
    if (out.executable.isEmpty()) {
        out.message = QStringLiteral("redis-server not under %1")
                          .arg(instance_.installPath.isEmpty()
                                   ? QStringLiteral("<no install path>")
                                   : instance_.installPath);
        return out;
    }
    const proc::CommandResult result =
        proc::runOnce(out.executable, {QStringLiteral("--version")});
    out.installed = result.ran;
    out.version = proc::extractVersion(result.output);
    if (out.version.isEmpty()) out.version = instance_.version;
    out.message = result.ran ? result.output.trimmed()
                             : QStringLiteral("%1 did not run").arg(out.executable);
    return out;
}

QString RedisDriver::renderBlock() const {
    const QString dataDir = toUrlPath(absoluteDataDir());
    QStringList lines;
    lines << QStringLiteral("# rendered by devdatapod from the instance settings");
    lines << QStringLiteral("bind %1").arg(settings_.bind);
    lines << QStringLiteral("port %1").arg(port_);
    lines << QStringLiteral("timeout %1").arg(settings_.timeout);
    if (versionAtLeast(3, 2)) {
        // tcp-keepalive arrived in 3.2.1; 3.0 rejects the unknown key.
        lines << QStringLiteral("tcp-keepalive %1").arg(settings_.keepAlive);
    }
    lines << QStringLiteral("maxclients %1").arg(settings_.maxClients);
    // Never daemonize: the panel must stay the parent, or the job object has nothing to
    // hold and an orphan redis survives a crash (rule 5).
    lines << QStringLiteral("daemonize no");
    if (versionAtLeast(3, 2)) {
        // `supervised` appeared in 3.2; writing it into a 3.0 conf refuses startup.
        lines << QStringLiteral("supervised no");
    }
    lines << QStringLiteral("loglevel notice");
    lines << QStringLiteral("logfile \"\"");    // stdout, captured into redis.log
    lines << QStringLiteral("databases %1").arg(settings_.databases);
    if (versionAtLeast(5, 0)) {
        // always-show-logo is a 5.0 addition.
        lines << QStringLiteral("always-show-logo no");
    }
    lines << QStringLiteral("dir %1").arg(dataDir);
    if (versionAtLeast(3, 2)) {
        // 3.0.504 has no protected-mode at all; writing it would refuse to start.
        lines << QStringLiteral("protected-mode %1")
                     .arg(settings_.password.isEmpty() ? QStringLiteral("no")
                                                       : QStringLiteral("yes"));
    }
    if (settings_.maxMemoryMb > 0) {
        lines << QStringLiteral("maxmemory %1mb").arg(settings_.maxMemoryMb);
        if (versionAtLeast(4, 0) && !settings_.maxMemoryPolicy.isEmpty()) {
            lines << QStringLiteral("maxmemory-policy %1").arg(settings_.maxMemoryPolicy);
        }
    }
    if (!settings_.password.isEmpty()) {
        lines << QStringLiteral("requirepass %1").arg(settings_.password);
    }
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

bool RedisDriver::prepareConfig(QString* error) {
    const QString dataDir = absoluteDataDir();
    if (dataDir.isEmpty()) {
        if (error) *error = QStringLiteral("%1 has no data directory").arg(instance_.id);
        return false;
    }
    QDir dir;
    if (!dir.mkpath(dataDir)) {
        if (error) *error = QStringLiteral("cannot create %1").arg(dataDir);
        return false;
    }
    const QString conf = confPath();
    switch (config::managed::apply(conf, renderBlock(), error)) {
    case config::BlockOutcome::Written:
        return true;
    case config::BlockOutcome::Preserved:
        // The file is the user's, so the port in it may not be ours. Starting is still
        // allowed - status() will report the mismatch instead of pretending.
        log::warn(QStringLiteral("redis/%1: %2 has no devdatapod block, using it as is")
                      .arg(instance_.id, conf));
        return true;
    case config::BlockOutcome::Failed:
        return false;
    }
    return false;
}

bool RedisDriver::start(QString* error) {
    // Checked first so an uninstalled version cannot leave a rendered redis.conf behind.
    const QString program = findExecutable();
    if (program.isEmpty()) {
        if (error) *error = QStringLiteral("%1 is not installed").arg(instance_.id);
        return false;
    }
    // Always re-render: `port` comes from the allocator and a default switch changes it, so
    // an existing redis.conf may well name the old one (rule 2).
    if (!prepareConfig(error)) return false;
    if (!supervisor_.isValid()) {
        if (error) *error = QStringLiteral("%1").arg(supervisor_.problem());
        return false;
    }
    supervisor_.setWorkingDirectory(absoluteDataDir());
    supervisor_.setLogPath(logPath());
    stoppedByUs_ = false;
    // Relative on purpose: the working directory IS the datadir, and the msys-built
    // redis-server mangles an absolute D:/... path into a doubled Unix-style one.
    if (!supervisor_.start(program, {QStringLiteral("redis.conf")}, error)) return false;
    lastStart_ = QDateTime::currentDateTime();
    sampler_.reset();
    return true;
}

bool RedisDriver::probe(const QStringList& verbs, QString* detail) {
    QTcpSocket socket;
    QHostAddress address;
    if (!address.setAddress(settings_.bind)) address.setAddress(QHostAddress::LocalHost);
    socket.connectToHost(address, quint16(port_));
    if (!socket.waitForConnected(kProbeTimeoutMs)) {
        if (detail) *detail = QStringLiteral("no listener on %1:%2 (%3)")
                                  .arg(settings_.bind)
                                  .arg(port_)
                                  .arg(socket.errorString());
        return false;
    }
    QByteArray request;
    if (!settings_.password.isEmpty()) {
        request += "AUTH " + settings_.password.toUtf8() + "\r\n";
    }
    for (const QString& verb : verbs) {
        request += verb.toUtf8() + "\r\n";
    }
    socket.write(request);
    if (!socket.waitForBytesWritten(kProbeTimeoutMs)) {
        if (detail) *detail = QStringLiteral("write to %1:%2 timed out").arg(settings_.bind).arg(port_);
        return false;
    }
    // RESP2 inline commands: any reply at all proves the event loop is serving. A -NOAUTH
    // still counts as up, which is what "ready" means for a password-protected instance.
    if (!socket.waitForReadyRead(kProbeTimeoutMs)) {
        if (detail) *detail = QStringLiteral("%1:%2 accepted the connection but stayed silent")
                                  .arg(settings_.bind)
                                  .arg(port_);
        return false;
    }
    const QByteArray reply = socket.readAll();
    if (detail) *detail = QString::fromUtf8(reply).trimmed();
    return !reply.isEmpty();
}

bool RedisDriver::stop(QString* error) {
    stoppedByUs_ = true;
    if (!supervisor_.isRunning()) return true;

    // The service's own shutdown verb first, so RDB and AOF survive (docs/ARCHITECTURE.md).
    // probe() prepends the inline AUTH itself; adding it here would be a second, wrong one.
    const QStringList verbs{
        QStringLiteral("SHUTDOWN %1")
            .arg(settings_.persistOnShutdown ? QStringLiteral("SAVE") : QStringLiteral("NOSAVE"))};
    QString detail;
    if (probe(verbs, &detail) && settings_.persistOnShutdown && detail.contains("ERR")) {
        // Redis refuses SAVE-with-pending-writes in a few states; the escape hatch is
        // NOSAVE, not a kill signal, so the process still exits cleanly.
        probe({QStringLiteral("SHUTDOWN NOSAVE")}, &detail);
    }
    if (supervisor_.waitFinished(kStopGraceMs)) return true;

    supervisor_.signalGraceful();
    if (supervisor_.waitFinished(2000)) return true;
    if (supervisor_.killForced()) {
        log::warn(QStringLiteral("redis/%1: had to be killed, SHUTDOWN did not take (%2)")
                      .arg(instance_.id, detail));
        return true;
    }
    if (error) {
        *error = QStringLiteral("%1 is still running after SHUTDOWN, SIGTERM and SIGKILL")
                     .arg(instance_.id);
    }
    return false;
}

ServiceStatus RedisDriver::status() {
    ServiceStatus status;
    if (!supervisor_.isRunning()) {
        if (stoppedByUs_ || !lastStart_.isValid()) {
            status.state = RunState::Stopped;
            return status;
        }
        status.state = RunState::Crashed;
        status.detail = QStringLiteral("exited with code %1").arg(supervisor_.exitCode());
        return status;
    }
    status.pid = supervisor_.pid();
    QString detail;
    if (probe({QStringLiteral("PING")}, &detail)) {
        status.state = RunState::Running;
        return status;
    }
    status.detail = detail;
    const qint64 waited =
        lastStart_.isValid() ? lastStart_.msecsTo(QDateTime::currentDateTime())
                             : qint64(kReadyTimeoutMs) + 1;
    // Alive but not answering is only "starting" for as long as it takes Redis to load its
    // dump; past that it is broken (wrong password, conf pointing at another port, ...).
    status.state = waited < kReadyTimeoutMs ? RunState::Starting : RunState::Crashed;
    return status;
}

LogTail RedisDriver::logs(int lines) {
    return text::readTail(logPath(), lines);
}

ServiceStats RedisDriver::stats() {
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
