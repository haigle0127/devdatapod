#include "app/SelfTest.h"

#include "core/net/PortAllocator.h"
#include "core/net/PortProbe.h"
#include "core/service/Engine.h"
#include "core/service/InstanceRegistry.h"
#include "core/service/ServiceInstance.h"
#include "core/util/AtomicFile.h"
#include "core/util/Log.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringConverter>
#include <QStringList>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTextStream>
#include <QVector>

#include <cstdint>
#include <cstdio>

namespace devdatapod {

namespace {

// Inside the 30000-32000 band but outside every slot band, so a test canonical port can
// never collide with a test slot port. The number lives in a fixture file precisely because
// nothing in C++ may hardcode a port (AGENTS.md rule 2) - a test may point a canonical
// anywhere it likes.
constexpr int kTestCanonical = 31789;

const char* kPortsHeader =
    "{\n"
    "  \"schema\": 2,\n"
    "  \"band\": [30000, 32000],\n"
    "  \"bindAddress\": \"127.0.0.1\",\n"
    "  \"canonical\": { \"redis\": 31789 },\n"
    "  \"slots\": { \"redis\": [30600, 30619] },\n"
    "  \"panel\": { \"api\": 30010 },\n";

QString portsFileWith(const QString& allocations) {
    return QString::fromLatin1(kPortsHeader) + QStringLiteral("  \"allocations\": ") + allocations
           + QStringLiteral("\n}\n");
}

QString allocationEntry(int port, const QString& owner, const QString& role) {
    return QStringLiteral("{ \"port\": %1, \"owner\": \"%2\", \"role\": \"%3\" }")
        .arg(port)
        .arg(owner, role);
}

// services.json as the registry writes it: one "default" id per kind plus the instance list.
QString servicesFile(const QString& kind, const QString& defaultId, const QJsonArray& instances) {
    QJsonObject block;
    block.insert(QStringLiteral("default"), defaultId);
    block.insert(QStringLiteral("instances"), instances);
    QJsonObject kinds;
    kinds.insert(kind, block);
    QJsonObject root;
    root.insert(QStringLiteral("schema"), 1);
    root.insert(QStringLiteral("kinds"), kinds);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

QJsonObject instanceEntry(const QString& id, const QString& version, const QString& role,
                          const QString& portKey) {
    QJsonObject entry;
    entry.insert(QStringLiteral("id"), id);
    entry.insert(QStringLiteral("version"), version);
    entry.insert(QStringLiteral("role"), role);
    entry.insert(QStringLiteral("portKey"), portKey);
    entry.insert(QStringLiteral("installPath"), QStringLiteral("Extensions/") + id);
    entry.insert(QStringLiteral("dataDir"), QStringLiteral("data/instances/") + id);
    return entry;
}

class Checks {
public:
    void section(const QString& name) {
        lines_ << QString();
        lines_ << QStringLiteral("-- %1").arg(name);
    }

    void check(bool condition, const QString& what) {
        ++total_;
        lines_ << QStringLiteral("%1 %2")
                      .arg(condition ? QStringLiteral("[ok]  ") : QStringLiteral("[FAIL]"))
                      .arg(what);
        if (!condition) ++failures_;
    }

    bool passed() const { return failures_ == 0; }
    int total() const { return total_; }
    int failures() const { return failures_; }
    const QStringList& lines() const { return lines_; }

private:
    QStringList lines_;
    int total_ = 0;
    int failures_ = 0;
};

bool writeText(const QString& path, const QString& text) {
    return file::writeAtomically(path, text.toUtf8());
}

bool readText(const QString& path, QString* out) {
    QByteArray raw;
    if (!file::readAll(path, &raw)) return false;
    *out = QString::fromUtf8(raw);
    return true;
}

// ------------------------------------------------------------------------ the port store
void checkPortsSeed(Checks& c, const QString& dir) {
    c.section(QStringLiteral("ports.json: seed and band table"));
    const QString path = QDir(dir).filePath(QStringLiteral("ports.json"));
    PortAllocator p(path);
    c.check(p.load(), QStringLiteral("load() seeds a missing ports.json"));
    c.check(QFileInfo::exists(path), QStringLiteral("the seeded file exists on disk"));
    c.check(p.isLoaded() && p.isHealthy(), QStringLiteral("the shipped seed is self-consistent"));
    c.check(p.bandFirst() == 30000 && p.bandLast() == 32000,
            QStringLiteral("band is 30000-32000 (docs/PORT-PLAN.md rule 1)"));
    c.check(p.bindAddress() == QLatin1String("127.0.0.1"),
            QStringLiteral("loopback only (AGENTS.md rule 1)"));
    c.check(p.canonicalPort(QStringLiteral("mysql")) == 3306,
            QStringLiteral("canonical mysql is 3306"));
    c.check(p.canonicalPort(QStringLiteral("redis")) == 6379,
            QStringLiteral("canonical redis is 6379"));
    c.check(p.canonicalPort(QStringLiteral("http")) == 80, QStringLiteral("canonical http is 80"));
    // nginx listens on the http band. Asking the canonical table for "nginx" must find nothing
    // rather than quietly answer through the wrong table.
    c.check(p.canonicalPort(QStringLiteral("nginx")) == -1,
            QStringLiteral("the canonical table is keyed by band, not by kind"));
    c.check(p.canonicalPort(QStringLiteral("php")) == -1,
            QStringLiteral("php owns no canonical port"));

    int first = 0;
    int last = 0;
    c.check(p.slotRange(QStringLiteral("redis"), &first, &last) && first == 30600 && last == 30619,
            QStringLiteral("redis slot band is 30600-30619"));
    c.check(!p.slotRange(QStringLiteral("nope"), &first, &last),
            QStringLiteral("an unknown slot reports no range"));
    c.check(p.slotStride(QStringLiteral("phpFpm")) == 10, QStringLiteral("phpFpm stride is 10"));
    c.check(p.slotStride(QStringLiteral("redis")) == 1, QStringLiteral("redis stride is 1"));
    c.check(p.panelPort(QStringLiteral("api")) == 30010, QStringLiteral("panel.api is 30010"));
    c.check(p.panelPort(QStringLiteral("nope")) == -1, QStringLiteral("an unknown panel port is -1"));
}

void checkPortsLifecycle(Checks& c, const QString& dir) {
    c.section(QStringLiteral("ports.json: allocation, swap, rollback"));
    const QString path = QDir(dir).filePath(QStringLiteral("lifecycle.json"));
    if (!writeText(path, portsFileWith(QStringLiteral("{}")))) {
        c.check(false, QStringLiteral("test fixture could not be written"));
        return;
    }

    PortAllocator p(path);
    c.check(p.load() && p.isHealthy(), QStringLiteral("fixture loads healthy"));

    const QString old = QStringLiteral("redis.3.0.504");
    const QString fresh = QStringLiteral("redis.7.2.5");
    const int slotA = p.allocateSlot(QStringLiteral("redis"), old, QStringLiteral("redis"));
    c.check(slotA == 30600, QStringLiteral("the first slot allocation is the band start"));
    c.check(p.allocateSlot(QStringLiteral("redis"), old, QStringLiteral("redis")) == slotA,
            QStringLiteral("allocateSlot is idempotent, a bound port is never re-drawn"));
    const int slotB = p.allocateSlot(QStringLiteral("redis"), fresh, QStringLiteral("redis"));
    c.check(slotB > slotA && slotB <= 30619, QStringLiteral("a second instance gets a free port"));

    c.check(p.assignCanonical(QStringLiteral("redis"), fresh, QStringLiteral("redis")),
            QStringLiteral("the default instance may claim the canonical port"));
    c.check(p.portOf(fresh) == kTestCanonical && p.roleOf(fresh) == QLatin1String("default"),
            QStringLiteral("claiming the canonical records both port and role"));
    c.check(!p.assignCanonical(QStringLiteral("redis"), old, QStringLiteral("redis")),
            QStringLiteral("a second default of the same kind is refused (rule 10)"));

    c.check(p.retarget(fresh, QStringLiteral("redis")) == 0,
            QStringLiteral("a default never moves off its canonical port (rule 3)"));
    const int moved = p.retarget(old, QStringLiteral("redis"));
    c.check(moved > 30599 && moved <= 30619 && moved != slotA,
            QStringLiteral("a secondary can move inside its own band"));
    c.check(!p.rebind(old, 45555, QStringLiteral("secondary"), QStringLiteral("redis")),
            QStringLiteral("rollback refuses a port that was never planned (rule 1)"));
    c.check(p.rebind(old, slotA, QStringLiteral("secondary"), QStringLiteral("redis")),
            QStringLiteral("rollback can put a planned port back exactly"));

    // The swap is the whole point of 「设为默认」: the canonical *number* stays put and only the
    // key bound to it changes, so a reference by port needs no re-render at all.
    c.check(p.swapRoles(fresh, old), QStringLiteral("swapRoles permutes two allocations of one kind"));
    c.check(p.portOf(old) == kTestCanonical && p.roleOf(old) == QLatin1String("default"),
            QStringLiteral("after the swap the canonical belongs to the new default"));
    c.check(p.portOf(fresh) == slotA && p.roleOf(fresh) == QLatin1String("secondary"),
            QStringLiteral("the demoted instance took the slot port, not a fresh draw"));
    // swapRoles only requires that the two roles differ, so the same call undoes itself.
    c.check(p.swapRoles(fresh, old), QStringLiteral("swapRoles is its own inverse (rollback)"));
    c.check(p.portOf(fresh) == kTestCanonical && p.portOf(old) == slotA,
            QStringLiteral("swapping back restores both original bindings"));

    c.check(!p.swapRoles(fresh, QStringLiteral("redis.missing")),
            QStringLiteral("swap needs two known keys"));
    c.check(p.release(old), QStringLiteral("release drops an allocation"));
    c.check(!p.contains(old), QStringLiteral("the released key is gone"));
    c.check(p.release(old), QStringLiteral("releasing twice is not an error"));

    QString onDiskBefore;
    c.check(readText(path, &onDiskBefore), QStringLiteral("the file is readable for the drift check"));
    PortAllocator reloaded(path);
    c.check(reloaded.load() && reloaded.isHealthy(), QStringLiteral("the file reloads"));
    c.check(reloaded.portOf(fresh) == kTestCanonical,
            QStringLiteral("a bound port survives a restart unchanged"));
    c.check(reloaded.save(), QStringLiteral("saving a reloaded file is allowed"));
    QString onDiskAfter;
    c.check(readText(path, &onDiskAfter), QStringLiteral("the file is readable afterwards"));
    c.check(onDiskBefore == onDiskAfter,
            QStringLiteral("a restart changes nothing in ports.json, not even a timestamp"));
}

void checkPortsFrozen(Checks& c, const QString& dir) {
    c.section(QStringLiteral("ports.json: readable but frozen"));
    const QString path = QDir(dir).filePath(QStringLiteral("dupe.json"));
    const QString both = QStringLiteral("{ \"a\": %1, \"b\": %2 }")
                             .arg(allocationEntry(30600, QStringLiteral("redis"),
                                                  QStringLiteral("secondary")),
                                  allocationEntry(30600, QStringLiteral("redis"),
                                                  QStringLiteral("secondary")));
    if (!writeText(path, portsFileWith(both))) {
        c.check(false, QStringLiteral("test fixture could not be written"));
        return;
    }

    PortAllocator p(path);
    c.check(p.load(), QStringLiteral("a duplicated port still loads (reads must keep working)"));
    c.check(p.isLoaded() && !p.isHealthy(), QStringLiteral("but it is marked frozen"));
    c.check(p.problem().contains(QLatin1String("30600")),
            QStringLiteral("the problem names the contested port"));
    c.check(p.allocateSlot(QStringLiteral("redis"), QStringLiteral("redis.7.2.5"),
                           QStringLiteral("redis"))
                == -1,
            QStringLiteral("allocation refuses while frozen"));
    c.check(!p.assignCanonical(QStringLiteral("redis"), QStringLiteral("redis.7.2.5"),
                               QStringLiteral("redis")),
            QStringLiteral("canonical claim refuses while frozen"));
    c.check(!p.release(QStringLiteral("a")), QStringLiteral("release refuses while frozen"));
    c.check(!p.save(),
            QStringLiteral("saving refuses while frozen, so a bad file cannot be overwritten"));
}

// ---------------------------------------------------------------------- the instance store
void checkRegistry(Checks& c, const QString& dir) {
    c.section(QStringLiteral("services.json: one default per kind"));
    const QString path = QDir(dir).filePath(QStringLiteral("services.json"));
    {
        InstanceRegistry empty(path);
        c.check(empty.load() && empty.isHealthy(),
                QStringLiteral("a missing services.json is an empty registry, not an error"));
        c.check(empty.instances().isEmpty(), QStringLiteral("nothing is installed yet"));
    }

    {
        InstanceRegistry r(path);
        r.load();
        ServiceInstance firstOfKind;
        firstOfKind.kind = QStringLiteral("redis");
        firstOfKind.version = QStringLiteral("7.2.5");
        c.check(r.add(firstOfKind), QStringLiteral("add() registers the first version"));

        ServiceInstance stored;
        c.check(r.find(QStringLiteral("redis-7.2.5"), &stored),
                QStringLiteral("the id is derived as <kind>-<version>"));
        c.check(stored.isDefault && stored.portKey == QLatin1String("redis.7.2.5"),
                QStringLiteral("the first version becomes the default and gets a portKey"));
        c.check(stored.dataDir == QLatin1String("data/instances/redis/7.2.5"),
                QStringLiteral("every instance gets its own datadir (rule 10)"));

        ServiceInstance secondOfKind;
        secondOfKind.kind = QStringLiteral("redis");
        secondOfKind.version = QStringLiteral("3.0.504");
        c.check(r.add(secondOfKind), QStringLiteral("a second version coexists"));
        c.check(r.countOfKind(QStringLiteral("redis")) == 2, QStringLiteral("both are listed"));
        c.check(!r.isDefault(QStringLiteral("redis-3.0.504")),
                QStringLiteral("installing a version never hijacks the default"));
        c.check(r.defaultId(QStringLiteral("redis")) == QLatin1String("redis-7.2.5"),
                QStringLiteral("the kind still names exactly one default"));
        c.check(!r.add(secondOfKind), QStringLiteral("registering the same version twice is refused"));

        ServiceInstance unknownKind;
        unknownKind.kind = QStringLiteral("mongodb");
        unknownKind.version = QStringLiteral("7.0");
        c.check(!r.add(unknownKind),
                QStringLiteral("a kind with no driver table entry is refused"));

        c.check(r.promoteDefault(QStringLiteral("redis-3.0.504")),
                QStringLiteral("promoteDefault moves the default"));
        c.check(r.isDefault(QStringLiteral("redis-3.0.504"))
                    && !r.isDefault(QStringLiteral("redis-7.2.5")),
                QStringLiteral("still exactly one default after the promotion"));
    }

    {
        InstanceRegistry reloaded(path);
        c.check(reloaded.load() && reloaded.isHealthy(), QStringLiteral("services.json reloads"));
        c.check(reloaded.countOfKind(QStringLiteral("redis")) == 2,
                QStringLiteral("both versions are still installed"));
        c.check(reloaded.defaultId(QStringLiteral("redis")) == QLatin1String("redis-3.0.504"),
                QStringLiteral("the promotion persisted"));
        c.check(reloaded.remove(QStringLiteral("redis-3.0.504")),
                QStringLiteral("removing the default instance is allowed"));
        c.check(reloaded.defaultId(QStringLiteral("redis")) == QLatin1String("redis-7.2.5"),
                QStringLiteral("removing the default promotes the earliest remaining version"));
        c.check(reloaded.remove(QStringLiteral("redis-7.2.5")),
                QStringLiteral("removing the last instance works"));
        c.check(reloaded.defaultId(QStringLiteral("redis")).isEmpty(),
                QStringLiteral("an empty kind names no default at all"));
    }
}

void checkRegistryFrozen(Checks& c, const QString& dir) {
    c.section(QStringLiteral("services.json: readable but frozen"));

    {
        const QString path = QDir(dir).filePath(QStringLiteral("two-defaults.json"));
        QJsonArray instances;
        instances.append(instanceEntry(QStringLiteral("redis-7.2.5"), QStringLiteral("7.2.5"),
                                       QStringLiteral("default"),
                                       QStringLiteral("redis.7.2.5")));
        instances.append(instanceEntry(QStringLiteral("redis-3.0.504"), QStringLiteral("3.0.504"),
                                       QStringLiteral("default"),
                                       QStringLiteral("redis.3.0.504")));
        writeText(path, servicesFile(QStringLiteral("redis"), QStringLiteral("redis-7.2.5"),
                                     instances));
        InstanceRegistry r(path);
        c.check(r.load() && r.isLoaded() && !r.isHealthy(),
                QStringLiteral("two instances marked default loads read-only"));
        ServiceInstance fresh;
        fresh.kind = QStringLiteral("redis");
        fresh.version = QStringLiteral("8.0");
        c.check(!r.add(fresh), QStringLiteral("writes refuse until a human fixes the file"));
        c.check(!r.promoteDefault(QStringLiteral("redis-3.0.504")),
                QStringLiteral("promotion refuses while frozen"));
    }

    {
        const QString path = QDir(dir).filePath(QStringLiteral("phantom.json"));
        QJsonArray instances;
        instances.append(instanceEntry(QStringLiteral("redis-7.2.5"), QStringLiteral("7.2.5"),
                                       QStringLiteral("default"),
                                       QStringLiteral("redis.7.2.5")));
        writeText(path, servicesFile(QStringLiteral("redis"), QStringLiteral("redis-9.9.9"),
                                     instances));
        InstanceRegistry r(path);
        c.check(r.load() && !r.isHealthy(),
                QStringLiteral("a default naming an uninstalled version is frozen"));
        c.check(r.problem().contains(QLatin1String("redis-9.9.9")),
                QStringLiteral("the problem names the phantom default"));
        c.check(r.defaultOf(QStringLiteral("redis")) == nullptr,
                QStringLiteral("no default instance is reported for it"));
    }

    {
        // The per-instance "role" string is derived data; the kind's "default" id is the
        // authority, so healing a disagreement between them has one right answer.
        const QString path = QDir(dir).filePath(QStringLiteral("stale-role.json"));
        QJsonArray instances;
        instances.append(instanceEntry(QStringLiteral("redis-7.2.5"), QStringLiteral("7.2.5"),
                                       QStringLiteral("secondary"),
                                       QStringLiteral("redis.7.2.5")));
        instances.append(instanceEntry(QStringLiteral("redis-3.0.504"), QStringLiteral("3.0.504"),
                                       QStringLiteral("default"),
                                       QStringLiteral("redis.3.0.504")));
        writeText(path, servicesFile(QStringLiteral("redis"), QStringLiteral("redis-7.2.5"),
                                     instances));
        InstanceRegistry r(path);
        c.check(r.load() && r.isHealthy(), QStringLiteral("a stale role marker is healable"));
        c.check(r.isDefault(QStringLiteral("redis-7.2.5")),
                QStringLiteral("the declared default wins over the role marker"));
        c.check(!r.isDefault(QStringLiteral("redis-3.0.504")),
                QStringLiteral("exactly one default after healing"));
    }
}

// ---------------------------------------------------------------------- the two together
void checkEngineReconcile(Checks& c, const QString& dir) {
    c.section(QStringLiteral("Engine::synchronize() reconciles the two files"));

    const QString portsPath = QDir(dir).filePath(QStringLiteral("engine-ports.json"));
    const QString servicesPath = QDir(dir).filePath(QStringLiteral("engine-services.json"));
    const QString allocations = QStringLiteral("{ \"redis.3.0.504\": %1, \"redis.7.2.5\": %2 }")
                                    .arg(allocationEntry(kTestCanonical, QStringLiteral("redis"),
                                                         QStringLiteral("default")),
                                         allocationEntry(30600, QStringLiteral("redis"),
                                                         QStringLiteral("secondary")));
    if (!writeText(portsPath, portsFileWith(allocations))) {
        c.check(false, QStringLiteral("test fixture could not be written"));
        return;
    }
    // services.json names 7.2.5 the default while ports.json still gives the canonical port to
    // 3.0.504 - exactly the state a crash between the two writes leaves behind.
    QJsonArray instances;
    instances.append(instanceEntry(QStringLiteral("redis-7.2.5"), QStringLiteral("7.2.5"),
                                   QStringLiteral("default"), QStringLiteral("redis.7.2.5")));
    instances.append(instanceEntry(QStringLiteral("redis-3.0.504"), QStringLiteral("3.0.504"),
                                   QStringLiteral("secondary"), QStringLiteral("redis.3.0.504")));
    writeText(servicesPath,
              servicesFile(QStringLiteral("redis"), QStringLiteral("redis-7.2.5"), instances));

    PortAllocator ports(portsPath);
    InstanceRegistry registry(servicesPath);
    c.check(ports.load() && registry.load(), QStringLiteral("both stores load"));

    Engine engine(&ports, &registry);
    QString error;
    c.check(engine.synchronize(&error),
            QStringLiteral("synchronize() heals the role drift instead of failing"));
    c.check(engine.isReady(), QStringLiteral("the engine reports itself usable"));
    c.check(ports.roleOf(QStringLiteral("redis.7.2.5")) == QLatin1String("default"),
            QStringLiteral("ports.json now agrees with services.json about the default"));
    c.check(engine.portOf(QStringLiteral("redis-7.2.5")) == kTestCanonical,
            QStringLiteral("the default instance holds the canonical port"));
    c.check(engine.portOf(QStringLiteral("redis-3.0.504")) == 30600,
            QStringLiteral("the secondary kept its slot port rather than a fresh draw"));

    // The second pass is what the next launch does; it must not move anything.
    QString second;
    c.check(engine.synchronize(&second), QStringLiteral("synchronize() is idempotent"));
    c.check(engine.portOf(QStringLiteral("redis-7.2.5")) == kTestCanonical
                && engine.portOf(QStringLiteral("redis-3.0.504")) == 30600,
            QStringLiteral("the second pass moved nothing"));
    c.check(engine.defaultOf(QStringLiteral("redis")) != nullptr,
            QStringLiteral("the kind has a default for a kind-level request to route to"));
    c.check(!engine.start(QStringLiteral("redis-7.2.5"), &error),
            QStringLiteral("start() fails on a fixture with no binary instead of lying"));
}

void checkEngineRejectsFrozen(Checks& c, const QString& dir) {
    c.section(QStringLiteral("Engine refuses to run on contradictory data"));
    const QString portsPath = QDir(dir).filePath(QStringLiteral("frozen-ports.json"));
    const QString servicesPath = QDir(dir).filePath(QStringLiteral("frozen-services.json"));
    const QString both = QStringLiteral("{ \"a\": %1, \"b\": %2 }")
                             .arg(allocationEntry(30600, QStringLiteral("redis"),
                                                  QStringLiteral("secondary")),
                                  allocationEntry(30600, QStringLiteral("redis"),
                                                  QStringLiteral("secondary")));
    writeText(portsPath, portsFileWith(both));
    QJsonArray instances;
    instances.append(instanceEntry(QStringLiteral("redis-7.2.5"), QStringLiteral("7.2.5"),
                                   QStringLiteral("default"), QStringLiteral("redis.7.2.5")));
    writeText(servicesPath,
              servicesFile(QStringLiteral("redis"), QStringLiteral("redis-7.2.5"), instances));

    PortAllocator ports(portsPath);
    InstanceRegistry registry(servicesPath);
    ports.load();
    registry.load();
    Engine engine(&ports, &registry);
    QString error;
    c.check(!engine.synchronize(&error), QStringLiteral("a frozen ports.json stops the engine"));
    c.check(!engine.isReady(), QStringLiteral("the engine stays unusable rather than half-working"));
    c.check(error.contains(QLatin1String("ports.json")),
            QStringLiteral("the report names the file at fault"));
}

// ----------------------------------------------------------------------------- port probe
void checkProbe(Checks& c) {
    c.section(QStringLiteral("PortProbe: free, occupied, and who occupies it"));
    QTcpServer server;
    if (!server.listen(QHostAddress(QHostAddress::LocalHost), 0)) {
        c.check(false, QStringLiteral("fixture could not listen on an ephemeral port"));
        return;
    }
    const quint16 port = quint16(server.serverPort());
    c.check(!net::isPortFree(port), QStringLiteral("a bound port reports not free"));

#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    const net::PortOccupier owner = net::identifyPortOwner(port);
    c.check(owner.occupied, QStringLiteral("the occupier is identified, not just \"port busy\""));
    c.check(owner.pid == static_cast<std::uint64_t>(QCoreApplication::applicationPid()),
            QStringLiteral("the identified pid is this process (rule 3 names the holder)"));
    c.check(!owner.processName.isEmpty(), QStringLiteral("the holder is named by process"));
    c.check(owner.address == QLatin1String("127.0.0.1"),
            QStringLiteral("the bound address round-trips out of the system table"));
#endif

    server.close();
    c.check(net::isPortFree(port), QStringLiteral("after close the same port is free again"));
}

}    // namespace

int runSelfTest() {
    QTemporaryDir temp;
    QTextStream out(stdout);
    out.setEncoding(QStringConverter::Utf8);
    if (!temp.isValid()) {
        out << "selftest: cannot create a temporary root\n";
        out.flush();
        return 1;
    }

    // The root is the OS temp directory, so on a machine whose user name is not ASCII every
    // path below also exercises AGENTS.md rule 7 (non-ASCII install directories).
    const QString root = temp.path();
    Checks c;
    out << QStringLiteral("devdatapod selftest, root %1\n").arg(root);

    checkPortsSeed(c, root);
    checkPortsLifecycle(c, root);
    checkPortsFrozen(c, root);
    checkRegistry(c, root);
    checkRegistryFrozen(c, root);
    checkEngineReconcile(c, root);
    checkEngineRejectsFrozen(c, root);
    checkProbe(c);

    for (const QString& line : c.lines()) out << line << '\n';
    out << QStringLiteral("\n%1 checks, %2 failed -> %3\n")
               .arg(c.total())
               .arg(c.failures())
               .arg(c.passed() ? QStringLiteral("PASS") : QStringLiteral("FAIL"));
    out.flush();

    log::info(QStringLiteral("selftest: %1 checks, %2 failed").arg(c.total()).arg(c.failures()));
    return c.passed() ? 0 : 1;
}

}
