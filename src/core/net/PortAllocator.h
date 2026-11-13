#pragma once

#include <QDateTime>
#include <QHash>
#include <QString>
#include <QVector>

namespace devdatapod {

// The only place that turns a service key into a TCP port. Every number comes out of
// data/ports.json - nothing in C++ may carry a port literal (AGENTS.md rule 2).
//
// Two kinds of keys live in the same map:
//   "mysql.8.0.12"   instance ports, role = default (canonical) or secondary (slot)
//   "panel.api"      ports devdatapod itself owns, fixed, never migrated
//
// Once a slot port is handed out it never moves. Restart must not reshuffle it or every
// rendered vhost pointing at it goes stale (docs/PORT-PLAN.md rule 1).
class PortAllocator {
public:
    struct Allocation {
        QString key;
        int port = 0;
        QString owner;    // the kind ("mysql", "redis") or "panel"
        QString role;     // "" | "default" | "secondary"
        QDateTime boundAt;
    };

    struct Slot {
        QString name;
        int first = 0;
        int last = 0;
        int stride = 1;
    };

    explicit PortAllocator(const QString& filePath);

    // Missing file is seeded from the shipped :/devdatapod/config/ports.default.json.
    // A file that exists but cannot be trusted leaves the allocator unhealthy: reads keep
    // working, every write path refuses until the conflict is resolved by the user.
    bool load();
    bool save();

    bool isLoaded() const { return loaded_; }
    bool isHealthy() const { return healthy_; }
    QString problem() const { return problem_; }
    QString filePath() const { return path_; }

    // --------------------------------------------------------------- band table
    // Keyed by band name, not kind: nginx asks for "http", php asks for nothing.
    int canonicalPort(const QString& band) const;      // -1 = the band does not exist
    bool slotRange(const QString& slot, int* first, int* last) const;
    int slotStride(const QString& slot) const;         // 1 unless the plan pins a step
    bool reservedRange(const QString& name, int* first, int* last) const;
    int panelPort(const QString& name) const;          // -1 when unknown
    QString bindAddress() const { return bind_; }
    int bandFirst() const { return bandFirst_; }
    int bandLast() const { return bandLast_; }

    // ------------------------------------------------------------ read the state
    bool contains(const QString& key) const;
    int portOf(const QString& key) const;              // 0 when unknown
    QString roleOf(const QString& key) const;
    QVector<Allocation> allocations() const { return items_; }
    QVector<Allocation> allocationsOf(const QString& owner) const;

    // ------------------------------------------------------------- change the state
    // Idempotent: an already allocated key returns its stored port. -1 means the slot
    // band is full or the file is unhealthy.
    int allocateSlot(const QString& slot, const QString& key, const QString& owner);
    // Only one default per kind, and a canonical port is never stolen from a live key
    // (docs/PORT-PLAN.md rule 3: an external holder must be reported, not routed around).
    // `band` is the canonical name in ports.json, `owner` the kind: nginx is ("http", ..., "nginx").
    bool assignCanonical(const QString& band, const QString& key, const QString& owner);
    bool release(const QString& key);
    // Move an existing *secondary* allocation inside its own band after a conflict. Returns
    // the new port or 0 when the band is exhausted; this is what triggers rule 2's cascade.
    // `slot` is the band name, not the kind: "php" listens on the "phpFpm" band. A default
    // never moves - the canonical port is either held by us or reported (rule 3).
    int retarget(const QString& key, const QString& slot);

    // Role *and* port swap between one default and one secondary of the same kind, in a
    // single write. Both ports are free by then, so no temporary port is needed (rule 4).
    bool swapRoles(const QString& defaultKey, const QString& otherKey);

    // Put a known allocation back exactly where it was. Only a rollback has this need: the
    // normal paths derive the port from a band, while a rollback must restore the binding
    // that was there before, whatever it was. Refuses when a different key holds `port`.
    bool rebind(const QString& key, int port, const QString& role, const QString& owner);

    // 用户手动改端口。新端口必须在规划内（canonical 或某个 slot 段），且不能被其他
    // key 占用。如果实例原来是 default 且新端口不是 canonical，角色降级为 secondary
    // （默认实例必须站在规范端口上，规则 3）。返回 true 表示写入成功。
    bool setPort(const QString& key, int port);

private:
    bool parse(const QByteArray& raw);
    QByteArray toJson() const;
    bool hasPort(int port, const QString& exceptKey = QString()) const;
    int freePortIn(const Slot& slot, int skip = 0) const;
    const Slot* findSlot(const QString& name) const;
    int indexOf(const QString& key) const;
    void setProblem(const QString& text);

    QString path_;
    QString bind_ = QStringLiteral("127.0.0.1");
    int bandFirst_ = 0;
    int bandLast_ = 0;

    QHash<QString, int> canonical_;
    QVector<Slot> slots_;
    QHash<QString, Slot> ranges_;
    QHash<QString, int> panel_;

    QVector<Allocation> items_;
    bool loaded_ = false;
    bool healthy_ = false;
    QString problem_;
};

}
