#include "app/Stage.h"

#include "core/net/PortAllocator.h"
#include "core/process/RunOnce.h"
#include "core/service/Engine.h"
#include "core/service/IServiceDriver.h"
#include "core/service/InstanceRegistry.h"
#include "core/service/ServiceInstance.h"
#include "core/service/ServiceKind.h"
#include "core/util/AppPaths.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QStringConverter>
#include <QStringList>
#include <QTextStream>
#include <QThread>
#include <QVector>

#include <cstdio>

namespace devdatapod {

namespace {

// Only three fields of a ServiceInstance are not derived: which kind, which version, and
// where it lives. P5's installer will read those from a manifest; here they come out of the
// directory name, which is the layout already on disk (Extensions/redis3.0.504,
// Extensions/MySQL8.0.12).
struct Candidate {
    QString kind;
    QString version;
    QString installPath;
};

QString kindKeys() {
    QStringList keys;
    for (const kinds::Meta& meta : kinds::all()) keys << meta.key;
    return keys.join(QLatin1String(", "));
}

// The argument may be relative to the install root or absolute inside it. Escaping the root
// is refused rather than rewritten: installPath is stored relative (AGENTS.md rule 7), and a
// leading ".." only shows up as a service that fails to start on another machine.
bool resolveArgument(const QString& argument, Candidate* out, QString* why) {
    QString path = QDir::fromNativeSeparators(argument.trimmed());
    while (path.length() > 1 && path.endsWith(QLatin1Char('/'))) path.chop(1);
    if (path.isEmpty()) {
        *why = QStringLiteral("empty path");
        return false;
    }
    const QString root = paths::installDir();
    if (QFileInfo(path).isAbsolute()) path = QDir(root).relativeFilePath(path);
    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        *why = QStringLiteral("\"%1\" is the install root itself").arg(argument);
        return false;
    }
    if (parts.first() == QLatin1String("..")) {
        *why = QStringLiteral("\"%1\" is outside the install root %2").arg(argument, root);
        return false;
    }
    if (!QFileInfo(QDir(root).filePath(path)).isDir()) {
        *why = QStringLiteral("no such directory: %1").arg(QDir(root).filePath(path));
        return false;
    }

    // Longest kind key wins, so a "mariadb10.6" directory can never read as mysql.
    const QVector<kinds::Meta> metas = kinds::all();
    const QString name = parts.last();
    const QString lower = name.toLower();
    int best = -1;
    for (int i = 0; i < metas.size(); ++i) {
        if (!lower.startsWith(metas[i].key.toLower())) continue;
        if (best < 0 || metas[i].key.size() > metas[best].key.size()) best = i;
    }
    if (best < 0) {
        *why = QStringLiteral("\"%1\" does not start with a known kind (%2)")
                   .arg(name, kindKeys());
        return false;
    }
    const QString version = proc::extractVersion(name.mid(metas[best].key.size()));
    if (version.isEmpty()) {
        *why = QStringLiteral("\"%1\" carries no version after the kind name").arg(name);
        return false;
    }

    out->kind = metas[best].key;
    out->version = version;
    out->installPath = parts.join(QLatin1Char('/'));
    return true;
}

}    // namespace

int runStage(const QStringList& arguments) {
    QTextStream out(stdout);
    out.setEncoding(QStringConverter::Utf8);

    const QString root = paths::installDir();
    out << "devdatapod stage, install root " << root << '\n';
    if (arguments.isEmpty()) {
        out << "usage: devdatapod-selftest --stage <dir>...\n"
               "  dir is inside the install root and named <kind><version>, e.g.\n"
               "  --stage Extensions/redis3.0.504 Extensions/redis7.2.5\n";
        return 1;
    }

    PortAllocator ports(QDir(root).filePath(QStringLiteral("data/ports.json")));
    if (!ports.load()) {
        out << "[FAIL] ports.json unusable: " << ports.problem() << '\n';
        return 1;
    }
    InstanceRegistry registry(QDir(root).filePath(QStringLiteral("data/services.json")));
    if (!registry.load()) {
        out << "[FAIL] services.json unusable: " << registry.problem() << '\n';
        return 1;
    }
    if (!registry.isHealthy()) {
        out << "[FAIL] services.json contradicts itself, writes are refused: "
            << registry.problem() << '\n';
        return 1;
    }

    QStringList ids;
    int failures = 0;
    for (const QString& argument : arguments) {
        Candidate candidate;
        QString why;
        if (!resolveArgument(argument, &candidate, &why)) {
            out << "[FAIL] " << argument << ": " << why << '\n';
            ++failures;
            continue;
        }
        const QString id = ServiceInstance::makeId(candidate.kind, candidate.version);
        ServiceInstance known;
        if (registry.find(id, &known)) {
            out << "[ok]   " << id << " is already registered at " << known.installPath << '\n';
        } else {
            ServiceInstance instance;
            instance.kind = candidate.kind;
            instance.version = candidate.version;
            instance.installPath = candidate.installPath;
            // add() fills id / portKey / dataDir, makes the first instance of a kind its
            // default, and writes the file - the same path the panel will use in P5.
            if (!registry.add(instance)) {
                out << "[FAIL] " << id << ": " << registry.problem() << '\n';
                ++failures;
                continue;
            }
            out << "[ok]   registered " << id << " from " << candidate.installPath << '\n';
        }
        ids << id;
    }

    // Reconcile before probing: a driver needs the port its role calls for, and this is the
    // same call the panel makes on startup, so the table below is what the panel will show.
    Engine engine(&ports, &registry);
    QString syncError;
    if (!engine.synchronize(&syncError)) {
        out << "[FAIL] reconcile: " << syncError << '\n';
        ++failures;
    }

    for (const QString& id : ids) {
        IServiceDriver* driver = engine.driver(id);
        if (!driver) {
            // P3 kinds register fine but cannot be probed yet. Not a staging failure: the row
            // and its port are exactly what was asked for.
            out << "[skip] " << id << ": " << engine.problem() << '\n';
            continue;
        }
        const Detection found = driver->detect();
        if (!found.installed) {
            out << "[FAIL] " << id << ": " << found.message << '\n';
            ++failures;
            continue;
        }
        ServiceInstance filed;
        registry.find(id, &filed);
        if (!found.version.isEmpty() && found.version != filed.version) {
            out << "[FAIL] " << id << ": filed as " << filed.version
                << " but the binary answers " << found.version << " - rename the directory\n";
            ++failures;
            continue;
        }
        out << "[ok]   " << id << " runs: " << found.executable << '\n';
    }

    out << "\n  " << registry.filePath() << '\n';
    out << "  " << ports.filePath() << "\n\n";
    out << "  id                      role       port    installPath\n";
    for (const ServiceInstance& instance : engine.instances()) {
        out << "  " << instance.id.leftJustified(20) << ' '
            << (instance.isDefault ? QStringLiteral("default") : QStringLiteral("secondary"))
                   .leftJustified(10)
            << ' ' << QString::number(engine.portOf(instance.id)).leftJustified(6)
            << instance.installPath << '\n';
    }
    out.flush();
    return failures == 0 ? 0 : 1;
}

int runInstance(const QString& id) {
    QTextStream out(stdout);
    out.setEncoding(QStringConverter::Utf8);

    const QString root = paths::installDir();
    out << "devdatapod run, install root " << root << '\n';

    PortAllocator ports(QDir(root).filePath(QStringLiteral("data/ports.json")));
    if (!ports.load()) {
        out << "[FAIL] ports.json unusable: " << ports.problem() << '\n';
        return 1;
    }
    InstanceRegistry registry(QDir(root).filePath(QStringLiteral("data/services.json")));
    if (!registry.load() || !registry.isHealthy()) {
        out << "[FAIL] services.json unusable: " << registry.problem() << '\n';
        return 1;
    }

    Engine engine(&ports, &registry);
    QString syncError;
    if (!engine.synchronize(&syncError)) {
        out << "[FAIL] reconcile: " << syncError << '\n';
        return 1;
    }

    ServiceInstance instance;
    if (!registry.find(id, &instance)) {
        out << "[FAIL] no instance \"" << id << "\" in " << registry.filePath() << '\n';
        return 1;
    }

    IServiceDriver* driver = engine.driver(id);
    if (!driver) {
        out << "[FAIL] no driver for " << id << ": " << engine.problem() << '\n';
        return 1;
    }
    const Detection found = driver->detect();
    out << "  detect   " << (found.installed ? "installed" : "missing") << "  "
        << found.executable << "  " << found.version << '\n';
    if (!found.installed) {
        out << "[FAIL] " << found.message << '\n';
        return 1;
    }

    QString error;
    if (!engine.start(id, &error)) {
        out << "[FAIL] start: " << error << '\n';
        return 1;
    }

    // status() is the only honest "it is up": every driver answers Running only after its
    // protocol probe succeeded, so this loop is the acceptance test, not a sleep.
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + 20000;
    RunState state = RunState::Starting;
    QString lastLine;
    while (QDateTime::currentMSecsSinceEpoch() < deadline) {
        const ServiceStatus status = engine.statusOf(id);
        state = status.state;
        const QString line = runStateKey(state)
                             + (status.detail.isEmpty() ? QString()
                                                        : QStringLiteral("  ") + status.detail);
        if (line != lastLine) {
            out << "  state    " << line << '\n';
            lastLine = line;
        }
        if (state == RunState::Running || state == RunState::Crashed
            || state == RunState::Stopped) {
            break;
        }
        QThread::msleep(200);
    }
    if (state != RunState::Running) {
        out << "[FAIL] never reached Running\n";
        const LogTail failed = driver->logs(15);
        if (!failed.text.trimmed().isEmpty()) out << failed.text << '\n';
        engine.stop(id, nullptr);
        return 1;
    }

    const int port = engine.portOf(id);
    out << "  pid      " << engine.statusOf(id).pid << "  port " << port << '\n';
    const LogTail tail = driver->logs(12);
    if (!tail.text.trimmed().isEmpty()) {
        out << "-- " << tail.path << " (" << tail.encoding << ")\n" << tail.text << '\n';
    }

    QString stopError;
    if (!engine.stop(id, &stopError)) {
        out << "[FAIL] stop: " << stopError << '\n';
        return 1;
    }
    out << "[ok]   " << id << " reached Running on 127.0.0.1:" << port
        << " and stopped cleanly\n";
    out.flush();
    return 0;
}

}
