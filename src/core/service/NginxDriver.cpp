#include "core/service/NginxDriver.h"

#include "core/config/ManagedBlock.h"
#include "core/process/RunOnce.h"
#include "core/service/SiteStore.h"
#include "core/util/AppPaths.h"
#include "core/util/AtomicFile.h"
#include "core/util/Log.h"
#include "core/util/LogTailReader.h"

#include <QDateTime>
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
constexpr int kReadyTimeoutMs = 8000;   // nginx 起得很快，慢只可能是端口/配置问题
constexpr int kProbeTimeoutMs = 400;
constexpr int kStopGraceMs = 4000;      // worker 收尾比 redis 干脆
constexpr int kStartCheckMs = 1500;     // 配置错时 nginx 秒退，留一点时间读错误日志

// nginx 在 Windows 上接受正斜杠路径，而且反斜杠在配置里是转义符，一律转掉。
QString toConfPath(const QString& path) {
    QString out = QDir::fromNativeSeparators(path);
    out.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return out;
}

QString executableName() {
#ifdef Q_OS_WIN
    return QStringLiteral("nginx.exe");
#else
    return QStringLiteral("nginx");
#endif
}

// 进程读到的最后几行错误，优先 error.log，没有就读 Supervisor 捕获的 stdout/stderr。
QString startupFailure(const QString& errorLog, const QString& consoleLog) {
    // error.log 是 nginx 自己写的，优先；它是空的才退回 Supervisor 捕获的 stdout/stderr。
    QStringList lines = text::readTail(errorLog, 12).text.split(QLatin1Char('\n'));
    if (lines.join(QString()).trimmed().isEmpty()) {
        lines = text::readTail(consoleLog, 12).text.split(QLatin1Char('\n'));
    }
    while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) lines.removeLast();
    return lines.join(QLatin1Char('\n')).trimmed();
}
}

NginxSettings NginxSettings::load(const QString& dataDir) {
    NginxSettings out;
    if (dataDir.isEmpty()) return out;
    const QString file = dataDir + QStringLiteral("/nginx.settings.json");
    if (!QFileInfo::exists(file)) return out;

    QByteArray raw;
    QString error;
    if (!file::readAll(file, &raw, &error)) {
        log::warn(QStringLiteral("nginx: %1").arg(error));
        return out;
    }
    QJsonParseError parse{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
        log::warn(QStringLiteral("nginx: 忽略 %1（%2）").arg(file, parse.errorString()));
        return out;
    }
    // 只认已知的键：拼错的键不会悄悄改变渲染进 nginx.conf 的内容（P4 换成 schema 引擎）。
    const QJsonObject object = doc.object();
    out.workerProcesses = object.value(QStringLiteral("workerProcesses")).toInt(out.workerProcesses);
    out.workerConnections =
        object.value(QStringLiteral("workerConnections")).toInt(out.workerConnections);
    out.rootDir = object.value(QStringLiteral("rootDir")).toString(out.rootDir);
    out.indexFiles = object.value(QStringLiteral("indexFiles")).toString(out.indexFiles);
    out.accessLog = object.value(QStringLiteral("accessLog")).toBool(out.accessLog);
    out.gzip = object.value(QStringLiteral("gzip")).toBool(out.gzip);
    return out;
}

bool NginxSettings::save(const QString& dataDir) const {
    if (dataDir.isEmpty()) return false;
    QJsonObject object;
    object.insert(QStringLiteral("workerProcesses"), workerProcesses);
    object.insert(QStringLiteral("workerConnections"), workerConnections);
    object.insert(QStringLiteral("rootDir"), rootDir);
    object.insert(QStringLiteral("indexFiles"), indexFiles);
    object.insert(QStringLiteral("accessLog"), accessLog);
    object.insert(QStringLiteral("gzip"), gzip);
    const QString file = dataDir + QStringLiteral("/nginx.settings.json");
    return file::writeAtomically(file, QJsonDocument(object).toJson(QJsonDocument::Indented));
}

NginxDriver::NginxDriver(const ServiceInstance& instance, int port)
    : IServiceDriver(instance, port),
      settings_(NginxSettings::load(instance.dataDir.isEmpty()
                                        ? QString()
                                        : paths::installDir() + QLatin1Char('/') + instance.dataDir)),
      supervisor_(instance.id) {}

// ----------------------------------------------------------------------------- 路径
QString NginxDriver::absoluteDataDir() const {
    if (instance_.dataDir.isEmpty()) return QString();
    return paths::installDir() + QLatin1Char('/') + instance_.dataDir;
}

QString NginxDriver::prefixDir() const {
    return absoluteDataDir() + QLatin1Char('/');
}

QString NginxDriver::confPath() const {
    return absoluteDataDir() + QStringLiteral("/nginx.conf");
}

QString NginxDriver::logPath() const {
    return absoluteDataDir() + QStringLiteral("/logs/error.log");
}

QString NginxDriver::accessLogPath() const {
    return absoluteDataDir() + QStringLiteral("/logs/access.log");
}

QString NginxDriver::absoluteRootDir() const {
    const QString root = settings_.rootDir.trimmed();
    if (root.isEmpty()) return paths::wwwDir();
    if (QDir::isAbsolutePath(root)) return QDir::cleanPath(root);
    return QDir::cleanPath(paths::installDir() + QLatin1Char('/') + root);
}

QString NginxDriver::findExecutable() const {
    if (instance_.installPath.isEmpty()) return QString();
    const QString root = paths::installDir() + QLatin1Char('/') + instance_.installPath;
    const QString name = executableName();
    // 官方 zip 摊平后 nginx.exe 就在根目录；bin/ 兼容别家打包。
    const QStringList candidates{root + QLatin1Char('/') + name,
                                 root + QStringLiteral("/bin/") + name};
    for (const QString& candidate : candidates) {
        if (QFileInfo::exists(candidate)) return QFileInfo(candidate).absoluteFilePath();
    }
    return QString();
}

// ----------------------------------------------------------------------------- 生命周期
Detection NginxDriver::detect() {
    Detection out;
    out.executable = findExecutable();
    if (out.executable.isEmpty()) {
        out.message = QStringLiteral("%1 下找不到 %2")
                          .arg(instance_.installPath.isEmpty()
                                   ? QStringLiteral("<无安装路径>")
                                   : instance_.installPath,
                               executableName());
        return out;
    }
    // `nginx -v` 把版本写到 stderr，stdout 是空的。
    const proc::CommandResult result =
        proc::runOnce(out.executable, {QStringLiteral("-v")});
    const QString text = (result.output + QLatin1Char(' ') + result.errorOutput).trimmed();
    out.version = proc::extractVersion(text);
    out.installed = result.ran && !out.version.isEmpty();
    if (!out.installed) {
        out.message = result.ran ? QStringLiteral("%1 没有报出版本：%2").arg(out.executable, text)
                                 : QStringLiteral("%1 无法运行").arg(out.executable);
        return out;
    }
    // 目录名写错版本时 selftest --stage 要靠这里抓出来。
    out.message = text;
    return out;
}

QString NginxDriver::renderBlock() const {
    const QString dataDir = toConfPath(absoluteDataDir());
    // 官方包自带 conf/mime.types，绝对包含，避免依赖 -p 之外的相对位置。
    const QString confRoot = toConfPath(paths::installDir() + QLatin1Char('/')
                                        + instance_.installPath);
    QStringList lines;
    lines << QStringLiteral("worker_processes %1;").arg(settings_.workerProcesses);
    // 日志与 pid 全部落在实例 datadir，Extensions 里的组件实物保持原样。
    lines << QStringLiteral("error_log \"%1/logs/error.log\" warn;").arg(dataDir);
    lines << QStringLiteral("pid \"%1/logs/nginx.pid\";").arg(dataDir);

    lines << QStringLiteral("events {");
    lines << QStringLiteral("    worker_connections %1;").arg(settings_.workerConnections);
    lines << QStringLiteral("}");

    lines << QStringLiteral("http {");
    // 官方包自带 conf/mime.types；绝对包含，避免依赖 -p 之外的相对位置。
    lines << QStringLiteral("    include \"%1/conf/mime.types\";").arg(confRoot);
    lines << QStringLiteral("    default_type application/octet-stream;");
    lines << QStringLiteral("    sendfile on;");
    lines << QStringLiteral("    keepalive_timeout 65;");
    lines << QStringLiteral("    server_tokens off;");
    if (settings_.gzip) {
        lines << QStringLiteral("    gzip on;");
        lines << QStringLiteral("    gzip_min_length 1k;");
        lines << QStringLiteral("    gzip_types text/css application/javascript "
                                "application/json text/plain text/xml;");
    }
    if (settings_.accessLog) {
        lines << QStringLiteral("    access_log \"%1\";").arg(toConfPath(accessLogPath()));
    } else {
        lines << QStringLiteral("    access_log off;");
    }
    // 临时目录也放 datadir：默认会落到 nginx.exe 旁边，等于把运行态写进代码目录。
    lines << QStringLiteral("    client_body_temp_path \"%1/temp/client_body\";").arg(dataDir);
    lines << QStringLiteral("    proxy_temp_path \"%1/temp/proxy\";").arg(dataDir);
    lines << QStringLiteral("    fastcgi_temp_path \"%1/temp/fastcgi\";").arg(dataDir);
    lines << QStringLiteral("    uwsgi_temp_path \"%1/temp/uwsgi\";").arg(dataDir);
    lines << QStringLiteral("    scgi_temp_path \"%1/temp/scgi\";").arg(dataDir);

    lines << QStringLiteral("    server {");
    // 规则 1：只绑 127.0.0.1；端口来自 PortAllocator，绝不在这里写死。
    lines << QStringLiteral("        listen 127.0.0.1:%1;").arg(port_);
    lines << QStringLiteral("        server_name localhost;");
    lines << QStringLiteral("        root \"%1\";").arg(toConfPath(absoluteRootDir()));
    lines << QStringLiteral("        index %1;").arg(settings_.indexFiles);
    lines << QStringLiteral("        location / {");
    lines << QStringLiteral("            try_files $uri $uri/ =404;");
    lines << QStringLiteral("        }");
    lines << QStringLiteral("    }");

    appendSiteServers(&lines);

    lines << QStringLiteral("}");

    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

// 网站页的每条记录 = 一个 vhost。端口由用户指定（站点端口不受面板 30000-32000 那条
// 约束，它属于用户的网站），域名写进 server_name，目录原样挂上去。
// 关掉一个站点只是让它不出现在这里——不删磁盘文件。
void NginxDriver::appendSiteServers(QStringList* lines) const {
    QString problem;
    const QVector<Site> items = sites::load(&problem);
    if (!problem.isEmpty()) {
        log::warn(QStringLiteral("nginx/%1: 读取站点失败：%2").arg(instance_.id, problem));
    }
    for (const Site& site : items) {
        if (site.port <= 0) continue;
        // 和 Web 服务自身端口撞了就是配置错，宁可跳过并留痕，也别让 nginx -t 直接失败。
        if (site.port == port_) {
            log::warn(QStringLiteral("nginx/%1: 站点 %2 想用 %3，和 Web 服务端口冲突，已跳过")
                          .arg(instance_.id, site.domain)
                          .arg(site.port));
            continue;
        }
        QStringList names{site.domain};
        for (const QString& alias : site.aliases) {
            if (!names.contains(alias)) names << alias;
        }
        *lines << QStringLiteral("    # 站点 %1").arg(site.domain);
        *lines << QStringLiteral("    server {");
        *lines << QStringLiteral("        listen 127.0.0.1:%1;").arg(site.port);
        *lines << QStringLiteral("        server_name %1;").arg(names.join(QLatin1Char(' ')));
        *lines << QStringLiteral("        root \"%1\";").arg(toConfPath(site.rootDir));
        *lines << QStringLiteral("        index %1;").arg(settings_.indexFiles);
        *lines << QStringLiteral("        location / {");
        *lines << QStringLiteral("            try_files $uri $uri/ =404;");
        *lines << QStringLiteral("        }");
        *lines << QStringLiteral("    }");
    }
}

bool NginxDriver::reload(QString* error) {
    const QString program = findExecutable();
    if (program.isEmpty()) {
        if (error) *error = QStringLiteral("%1 未安装").arg(instance_.id);
        return false;
    }
    if (!supervisor_.isRunning()) {
        if (error) *error = QStringLiteral("%1 没有运行，无法热重载").arg(instance_.id);
        return false;
    }
    if (!prepareConfig(error)) return false;

    const QStringList base{QStringLiteral("-p"), prefixDir(), QStringLiteral("-c"), confPath()};
    // 先 -t 再 reload：配置写坏时让用户拿到具体行号，而不是一个「服务还在跑但站点没生效」。
    const proc::CommandResult test = proc::runOnce(program, base + QStringList{QStringLiteral("-t")},
                                                   5000, absoluteDataDir());
    const QString testText = (test.errorOutput + QLatin1Char('\n') + test.output).trimmed();
    if (!test.ran || test.exitCode != 0) {
        if (error) {
            *error = testText.isEmpty() ? QStringLiteral("nginx -t 失败") : testText;
        }
        return false;
    }

    const proc::CommandResult sent =
        proc::runOnce(program, base + QStringList{QStringLiteral("-s"), QStringLiteral("reload")},
                      5000, absoluteDataDir());
    if (!sent.ran || sent.exitCode != 0) {
        if (error) {
            const QString text = (sent.errorOutput + QLatin1Char('\n') + sent.output).trimmed();
            *error = text.isEmpty() ? QStringLiteral("nginx -s reload 失败") : text;
        }
        return false;
    }
    return true;
}

void NginxDriver::ensureWelcomePage() const {
    const QString root = absoluteRootDir();
    QDir dir;
    if (!dir.mkpath(root)) {
        log::warn(QStringLiteral("nginx/%1: 无法创建站点目录 %2").arg(instance_.id, root));
        return;
    }
    // index 列表里任何一个已存在就不动，用户放的页面永远优先。
    const QStringList indexes = settings_.indexFiles.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString& index : indexes) {
        if (QFileInfo::exists(root + QLatin1Char('/') + index)) return;
    }
    const QString page = QStringLiteral(
        "<!doctype html>\n"
        "<html lang=\"zh-CN\">\n"
        "<head>\n"
        "  <meta charset=\"utf-8\">\n"
        "  <title>devdatapod · Nginx</title>\n"
        "  <style>\n"
        "    body { margin: 0; min-height: 100vh; display: flex; align-items: center;\n"
        "           justify-content: center; background: #F6F5FB; color: #26203F;\n"
        "           font-family: 'MiSans', 'Microsoft YaHei', sans-serif; }\n"
        "    .card { padding: 40px 48px; background: #FFFFFF; border-radius: 16px;\n"
        "            box-shadow: 0 12px 32px rgba(38, 32, 63, 0.08); text-align: center; }\n"
        "    h1 { margin: 0 0 10px; font-size: 22px; font-weight: 600; }\n"
        "    p { margin: 6px 0 0; font-size: 13px; color: #6F6A85; }\n"
        "    code { color: #6C5CE7; }\n"
        "  </style>\n"
        "</head>\n"
        "<body>\n"
        "  <div class=\"card\">\n"
        "    <h1>Nginx 已经跑起来了</h1>\n"
        "    <p>这是 devdatapod 放的欢迎页，把站点文件放进 <code>%1</code> 即可。</p>\n"
        "    <p>把这里换成你自己的 <code>index.html</code>，刷新就是你的站点了。</p>\n"
        "  </div>\n"
        "</body>\n"
        "</html>\n")
        .arg(settings_.rootDir);
    const QString file = root + QStringLiteral("/index.html");
    if (!file::writeAtomically(file, page.toUtf8())) {
        log::warn(QStringLiteral("nginx/%1: 无法写入欢迎页 %2").arg(instance_.id, file));
    }
}

bool NginxDriver::prepareConfig(QString* error) {
    const QString dataDir = absoluteDataDir();
    if (dataDir.isEmpty()) {
        if (error) *error = QStringLiteral("%1 没有数据目录").arg(instance_.id);
        return false;
    }
    QDir dir;
    if (!dir.mkpath(dataDir + QStringLiteral("/logs"))
        || !dir.mkpath(dataDir + QStringLiteral("/temp")) || !dir.mkpath(absoluteRootDir())) {
        if (error) *error = QStringLiteral("无法创建 %1 下的运行时目录").arg(dataDir);
        return false;
    }
    ensureWelcomePage();
    // nginx 的临时目录由自己创建；父目录存在就够，但提前建好省得首启报错。
    for (const QString& sub : {QStringLiteral("client_body"), QStringLiteral("proxy"),
                               QStringLiteral("fastcgi"), QStringLiteral("uwsgi"),
                               QStringLiteral("scgi")}) {
        dir.mkpath(dataDir + QStringLiteral("/temp/") + sub);
    }

    const QString conf = confPath();
    switch (config::managed::apply(conf, renderBlock(), error)) {
    case config::BlockOutcome::Written:
        return true;
    case config::BlockOutcome::Preserved:
        // 文件是用户手写的：端口可能不是我们的，仍允许启动，status() 会报不一致。
        log::warn(QStringLiteral("nginx/%1: %2 没有 devdatapod 托管块，按原样使用")
                      .arg(instance_.id, conf));
        return true;
    case config::BlockOutcome::Failed:
        return false;
    }
    return false;
}

bool NginxDriver::start(QString* error) {
    const QString program = findExecutable();
    if (program.isEmpty()) {
        if (error) *error = QStringLiteral("%1 未安装").arg(instance_.id);
        return false;
    }
    // 每次启动都重渲染：端口来自分配器，切默认会变（规则 2）。
    if (!prepareConfig(error)) return false;
    if (!supervisor_.isValid()) {
        if (error) *error = QStringLiteral("%1").arg(supervisor_.problem());
        return false;
    }

    const QString consoleLog = absoluteDataDir() + QStringLiteral("/logs/console.log");
    supervisor_.setWorkingDirectory(absoluteDataDir());
    supervisor_.setLogPath(consoleLog);
    stoppedByUs_ = false;

    // -p 固定到实例 datadir，nginx 才不会把 pid / 临时目录写进 Extensions。
    if (!supervisor_.start(program,
                           {QStringLiteral("-p"), prefixDir(), QStringLiteral("-c"), confPath()},
                           error)) {
        return false;
    }
    lastStart_ = QDateTime::currentDateTime();
    sampler_.reset();

    // nginx 先起进程再读配置：配置错时它秒退，原因只写在 error.log 里。
    // 短暂等一下，把「起来就死」变成带原因的启动失败，而不是首页上一个 Crash 红点。
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + kStartCheckMs;
    while (QDateTime::currentMSecsSinceEpoch() < deadline) {
        if (!supervisor_.isRunning()) {
            const QString why = startupFailure(logPath(), consoleLog);
            if (error) {
                *error = why.isEmpty()
                             ? QStringLiteral("nginx 启动后立即退出（退出码 %1）")
                                   .arg(supervisor_.exitCode())
                             : why;
            }
            return false;
        }
        QString detail;
        if (probeHttp(&detail)) return true;
        QThread::msleep(100);
    }
    return true;
}

bool NginxDriver::probeHttp(QString* detail) const {
    QTcpSocket socket;
    socket.connectToHost(QHostAddress(QHostAddress::LocalHost), quint16(port_));
    if (!socket.waitForConnected(kProbeTimeoutMs)) {
        if (detail) {
            *detail = QStringLiteral("127.0.0.1:%1 没有监听（%2）")
                          .arg(port_)
                          .arg(socket.errorString());
        }
        return false;
    }
    socket.write("GET / HTTP/1.0\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    if (!socket.waitForBytesWritten(kProbeTimeoutMs) || !socket.waitForReadyRead(kProbeTimeoutMs)) {
        if (detail) *detail = QStringLiteral("127.0.0.1:%1 接受连接但没有响应").arg(port_);
        return false;
    }
    const QByteArray reply = socket.readAll();
    const QString firstLine = QString::fromUtf8(reply.left(reply.indexOf("\r\n")));
    if (!firstLine.startsWith(QLatin1String("HTTP/"))) {
        if (detail) *detail = QStringLiteral("127.0.0.1:%1 回的不是 HTTP 响应").arg(port_);
        return false;
    }
    if (detail) *detail = firstLine;
    return true;
}

bool NginxDriver::stop(QString* error) {
    stoppedByUs_ = true;
    const QString program = findExecutable();
    if (program.isEmpty()) return true;

    // 即使 Supervisor 认为进程已退出（Windows 版 nginx 有 daemon 化行为），
    // 也先发一次 quit，把可能残留的 master 收掉。
    proc::runOnce(program,
                  {QStringLiteral("-p"), prefixDir(), QStringLiteral("-c"), confPath(),
                   QStringLiteral("-s"), QStringLiteral("quit")},
                  5000);
    if (supervisor_.waitFinished(kStopGraceMs)) return true;

    supervisor_.signalGraceful();
    if (supervisor_.waitFinished(2000)) return true;
    if (supervisor_.killForced()) {
        log::warn(QStringLiteral("nginx/%1: nginx -s quit 没停掉，已强杀").arg(instance_.id));
        return true;
    }
    if (error) {
        *error = QStringLiteral("%1 在 nginx -s quit、信号、强杀之后仍在运行").arg(instance_.id);
    }
    return false;
}

ServiceStatus NginxDriver::status() {
    ServiceStatus status;
    QString detail;
    const bool serving = probeHttp(&detail);

    if (!supervisor_.isRunning()) {
        if (stoppedByUs_ || !lastStart_.isValid()) {
            status.state = RunState::Stopped;
            return status;
        }
        // 进程句柄没了但端口还在答话：Windows 版 nginx 自己托管了 master。
        // 这不假装成正常：detail 里点名，stop() 仍会走 -s quit。
        if (serving) {
            status.state = RunState::Running;
            status.detail = QStringLiteral("master 已脱离父进程，仍在 127.0.0.1:%1 提供服务")
                                .arg(port_);
            return status;
        }
        status.state = RunState::Crashed;
        status.detail = QStringLiteral("退出码 %1").arg(supervisor_.exitCode());
        return status;
    }

    status.pid = supervisor_.pid();
    if (serving) {
        status.state = RunState::Running;
        return status;
    }
    status.detail = detail;
    const qint64 waited =
        lastStart_.isValid() ? lastStart_.msecsTo(QDateTime::currentDateTime())
                             : qint64(kReadyTimeoutMs) + 1;
    status.state = waited < kReadyTimeoutMs ? RunState::Starting : RunState::Crashed;
    return status;
}

LogTail NginxDriver::logs(int lines) {
    return text::readTail(logPath(), lines);
}

ServiceStats NginxDriver::stats() {
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

} // namespace devdatapod
