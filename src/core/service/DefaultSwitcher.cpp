#include "core/service/DefaultSwitcher.h"

#include "core/net/PortAllocator.h"
#include "core/service/Engine.h"
#include "core/service/ServiceKind.h"
#include "core/util/Log.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <QVector>

#include <utility>

namespace devdatapod {

namespace {
constexpr int kReadyTimeoutMs = 15000;
constexpr int kPollIntervalMs = 100;
constexpr const char* kRoleDefault = "default";

// One completed (move, cascade) pair, kept so a failure can run exactly those renderers
// backwards in the order they were applied.
struct Rendered {
    int move;
    int cascade;
};
}

DefaultSwitcher::DefaultSwitcher(Engine* engine) : engine_(engine) {}

void DefaultSwitcher::addCascade(Cascade cascade) {
    if (cascade) cascades_.push_back(std::move(cascade));
}

bool DefaultSwitcher::waitReady(const QString& id, int timeoutMs) {
    // A QEventLoop rather than a sleep: the window has to keep painting and its buttons keep
    // working while the new default loads, and promote() is deliberately synchronous.
    QElapsedTimer clock;
    clock.start();
    QEventLoop loop;
    QTimer ticker;
    ticker.setInterval(kPollIntervalMs);
    bool ready = false;
    QObject::connect(&ticker, &QTimer::timeout, &loop, [&]() {
        if (clock.elapsed() >= timeoutMs) {
            loop.quit();
            return;
        }
        const ServiceStatus status = engine_->statusOf(id);
        if (status.state == RunState::Running) {
            ready = true;
            loop.quit();
        } else if (status.state == RunState::Crashed || status.state == RunState::Stopped) {
            loop.quit();    // it is not going to become ready
        }
    });
    ticker.start();
    loop.exec();
    return ready;
}

bool DefaultSwitcher::promote(const QString& id, QString* error) {
    // Held for the whole transaction, including the nested event loop waitReady() runs:
    // a click on 「设为默认」 landing there would start a second switch whose end state
    // depends on which one finishes last.
    if (switching_) {
        if (error) *error = QStringLiteral("another default switch is still running");
        return false;
    }
    switching_ = true;
    const bool ok = promoteNow(id, error);
    switching_ = false;
    return ok;
}

bool DefaultSwitcher::promoteNow(const QString& id, QString* error) {
    auto fail = [&](const QString& text) {
        log::error(QStringLiteral("switch: %1").arg(text));
        if (error) *error = text;
        return false;
    };

    InstanceRegistry* registry = engine_->registry();
    PortAllocator* ports = engine_->ports();

    ServiceInstance target;
    if (!registry->find(id, &target)) {
        return fail(QStringLiteral("%1 is not an installed instance").arg(id));
    }
    const QString kind = target.kind;
    const kinds::Meta* meta = kinds::find(kind);
    if (!meta) {
        return fail(QStringLiteral("%1 belongs to unknown kind \"%2\"").arg(id, kind));
    }

    const QString previousId = registry->defaultId(kind);
    if (previousId == id) return true;    // already the default: nothing to move
    ServiceInstance previous;
    const bool hadPrevious = !previousId.isEmpty() && registry->find(previousId, &previous);

    // A kind with no canonical port (php, python) has no port to move; the switch only
    // changes which instance the renderers should point at.
    const bool ownsPort = !meta->canonical.isEmpty() && !target.portKey.isEmpty();
    int canonical = 0;
    if (ownsPort) {
        canonical = ports->canonicalPort(meta->canonical);
        if (canonical <= 0) {
            return fail(QStringLiteral("ports.json has no canonical port for band \"%1\"")
                            .arg(meta->canonical));
        }
    }
    const int targetWas = ports->portOf(target.portKey);
    const QString targetWasRole = ports->roleOf(target.portKey);
    const bool previousHoldsCanonical =
        hadPrevious && ports->portOf(previous.portKey) == canonical
        && ports->roleOf(previous.portKey) == QLatin1String(kRoleDefault);

    // ------------------------------------------------------------- 1. take them down
    QStringList stopped;
    auto stopOne = [&](const QString& which) {
        if (which.isEmpty() || !engine_->isRunning(which)) return true;
        QString reason;
        if (engine_->stop(which, &reason)) {
            stopped << which;
            return true;
        }
        fail(QStringLiteral("%1 did not stop (%2), the switch was not attempted")
                 .arg(which, reason));
        return false;
    };
    auto bringBack = [&]() {
        for (int i = stopped.size() - 1; i >= 0; --i) {
            QString reason;
            if (!engine_->start(stopped.at(i), &reason)) {
                // This service was up before the call and is down now. Loud, because the
                // only way out is for a human to start it.
                log::error(QStringLiteral("switch: CRITICAL, %1 could not be brought back: %2")
                               .arg(stopped.at(i), reason));
            }
        }
    };
    if (!stopOne(previousId)) return false;    // nothing structural changed yet
    if (!stopOne(id)) { bringBack(); return false; }

    // ---------------------------------------------------------- 2. move the port
    bool portsMoved = false;
    bool usedSwap = false;
    if (ownsPort) {
        usedSwap = previousHoldsCanonical;
        portsMoved = usedSwap ? ports->swapRoles(previous.portKey, target.portKey)
                              : ports->assignCanonical(meta->canonical, target.portKey, kind);
        if (!portsMoved) {
            fail(QStringLiteral("%1 could not take port %2; rolled back")
                     .arg(id)
                     .arg(canonical));
            bringBack();
            return false;
        }
    }
    auto undoPorts = [&]() {
        if (!portsMoved) return;
        bool ok = true;
        if (usedSwap) {
            // swapRoles is its own inverse: the target now holds the default role, so passing
            // it first puts both the ports and the roles back.
            ok = ports->swapRoles(target.portKey, previous.portKey);
        } else {
            ok = ports->release(target.portKey);
            if (ok && targetWas > 0) {
                ok = ports->rebind(target.portKey, targetWas, targetWasRole, kind);
            }
        }
        if (ok) {
            portsMoved = false;
        } else {
            log::error(QStringLiteral("switch: CRITICAL, ports.json was not restored for %1")
                           .arg(id));
        }
    };

    // Back to the state this call found: new default down, role restored, ports restored,
    // drivers rebuilt, whatever was running up again.
    auto undoSwitch = [&]() {
        engine_->stop(id, nullptr);
        if (!previousId.isEmpty() && !registry->promoteDefault(previousId)) {
            log::error(QStringLiteral("switch: CRITICAL, services.json still calls %1 the "
                                      "default of %2")
                           .arg(id, kind));
        }
        undoPorts();
        engine_->invalidate(id);
        if (hadPrevious) engine_->invalidate(previousId);
        bringBack();
    };

    // ------------------------------------------------------------ 3. flip the role
    if (!registry->promoteDefault(id)) {
        undoPorts();
        fail(QStringLiteral("%1 could not be written as the default of %2; rolled back")
                 .arg(id, kind));
        bringBack();
        return false;
    }
    // Both cached drivers were built against the old binding and the old port.
    engine_->invalidate(id);
    if (hadPrevious) engine_->invalidate(previousId);

    // ---------------------------------------------------- 4. start and prove the new one
    // Only when this kind was actually serving: switching the default of a stopped stack must
    // not start it as a side effect.
    const bool wasServing = stopped.contains(previousId) || stopped.contains(id);
    QString reason;
    if (wasServing) {
        if (!engine_->start(id, &reason)) {
            reason = QStringLiteral("%1 did not start: %2").arg(id, reason);
        } else if (!waitReady(id, kReadyTimeoutMs)) {
            const ServiceStatus status = engine_->statusOf(id);
            reason = QStringLiteral("%1 does not answer on %2 (%3)")
                         .arg(id)
                         .arg(ownsPort ? canonical : 0)
                         .arg(status.detail);
        } else {
            reason.clear();
        }
    }
    if (!reason.isEmpty()) {
        undoSwitch();
        return fail(reason + QStringLiteral("; rolled back"));
    }

    // ----------------------------------------------------------------- 5. re-render
    // One pass per key whose port binding changed. The canonical number itself never moved,
    // so a vhost pointing at 3306 needs nothing; only a reference naming one of these keys
    // went stale (AGENTS.md rule 2).
    QVector<PortMove> moves;
    if (ownsPort) {
        PortMove promoted;
        promoted.kind = kind;
        promoted.portKey = target.portKey;
        promoted.from = targetWas;
        promoted.to = canonical;
        promoted.becameDefault = true;
        moves.append(promoted);
        if (usedSwap) {
            PortMove demoted;
            demoted.kind = kind;
            demoted.portKey = previous.portKey;
            demoted.from = canonical;
            demoted.to = targetWas;
            demoted.becameDefault = false;
            moves.append(demoted);
        }
    } else {
        PortMove changed;
        changed.kind = kind;
        changed.portKey = target.portKey;
        changed.becameDefault = true;    // from == to == 0: no number moved, only the holder
        moves.append(changed);
    }

    QVector<Rendered> done;
    const int cascadeCount = static_cast<int>(cascades_.size());
    for (int m = 0; m < moves.size(); ++m) {
        for (int c = 0; c < cascadeCount; ++c) {
            QString why;
            if (cascades_[size_t(c)](moves.at(m), &why)) {
                done.append(Rendered{m, c});
                continue;
            }
            // Run every renderer already applied backwards - same functions, from and to
            // swapped - then undo the switch itself.
            for (int i = done.size() - 1; i >= 0; --i) {
                const Rendered& r = done.at(i);
                const PortMove& forward = moves.at(r.move);
                PortMove inverse;
                inverse.kind = forward.kind;
                inverse.portKey = forward.portKey;
                inverse.from = forward.to;
                inverse.to = forward.from;
                inverse.becameDefault = forward.becameDefault;
                QString ignored;
                if (cascades_[size_t(r.cascade)](inverse, &ignored)) {
                    log::warn(QStringLiteral("switch: re-rendered %1 back to port %2")
                                  .arg(forward.portKey)
                                  .arg(forward.from));
                } else {
                    log::error(QStringLiteral("switch: CRITICAL, could not undo the re-render "
                                              "of %1; references to it may be stale")
                                   .arg(forward.portKey));
                }
            }
            undoSwitch();
            return fail(QStringLiteral("re-render of %1 failed (%2); rolled back")
                            .arg(moves.at(m).portKey, why));
        }
    }

    log::info(QStringLiteral("switch: default of %1 is now %2 on port %3")
                  .arg(kind, id)
                  .arg(ownsPort ? canonical : 0));
    return true;
}

}
