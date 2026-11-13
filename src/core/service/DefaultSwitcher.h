#pragma once

#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

namespace devdatapod {

class Engine;

// The transaction behind 「设为默认」 (AGENTS.md rule 10). Flipping a string is not a switch:
// the canonical port has to move with the role, everything that references that port has to
// follow, and a failure half-way puts back what was there - including restarting the
// instance this call stopped.
//
// Order is the whole design: stop first, so both ports are genuinely free and one
// swapRoles() write is enough (no temporary third port, no window where 6379 is held twice).
// Then role, then start-and-prove, then re-render. A step that fails runs the steps before
// it backwards.
//
// Killing the panel *during* a switch is not this class's problem: services.json and
// ports.json are each written atomically, so the worst case is two files that disagree, and
// Engine::synchronize() heals exactly that on the next launch.
class DefaultSwitcher {
public:
    explicit DefaultSwitcher(Engine* engine);

    // Something that must be rewritten because one instance's port moved. nginx's
    // fastcgi_pass, php's pdo_mysql.default_port, a site pinned to a specific php-fpm, the
    // Adminer link. P3/P4 register them.
    //
    // Note what is *not* here: a canonical port never changes number during a switch, so a
    // vhost that says "127.0.0.1:3306" needs no re-render at all - only the instance behind
    // it does. What goes stale is a reference naming one of the two permuted keys, hence one
    // call per moved key rather than one per switch.
    //
    // Must be deterministic and idempotent: on rollback the switcher re-invokes it with
    // from and to swapped, which is the compensation that puts a rendered file back.
    // Returning false rolls the whole switch back.
    struct PortMove {
        QString kind;
        QString portKey;
        int from = 0;
        int to = 0;
        bool becameDefault = false;
    };
    using Cascade = std::function<bool(const PortMove&, QString* error)>;
    void addCascade(Cascade cascade);

    // Makes `id` the default of its kind. Synchronous: the call does not return until the
    // new default answers its protocol or the attempt has been rolled back. Re-entrant calls
    // are refused outright rather than queued, because two interleaved switches have no
    // defined end state.
    bool promote(const QString& id, QString* error = nullptr);

    bool isSwitching() const { return switching_; }

private:
    // The transaction itself. promote() owns the re-entrancy flag around it.
    bool promoteNow(const QString& id, QString* error);
    // Poll status() the way the acceptance test needs it: Running only after the driver has
    // actually spoken the protocol, and the wait must not freeze the window.
    bool waitReady(const QString& id, int timeoutMs);

    Engine* engine_;
    std::vector<Cascade> cascades_;
    bool switching_ = false;
};

}
