#pragma once

#include <QString>

namespace devdatapod {

// Observed runtime state. Deliberately separate from the persistent role
// (default / secondary): "default" does not mean "running" and "running" does not mean
// "default" - a secondary instance may run in parallel on its slot port (docs/ARCHITECTURE.md).
enum class RunState {
    Stopped,
    Starting,
    Running,
    Switching,
    Crashed,    // died on its own, not because we stopped it
    Idle        // installed but nothing to run yet (no datadir, no config)
};

// The value written into the "state" dynamic property that QSS matches on. Kept in core so
// the service engine and the widgets agree on one spelling.
inline QString runStateKey(RunState state) {
    switch (state) {
    case RunState::Stopped: return QStringLiteral("stopped");
    case RunState::Starting: return QStringLiteral("starting");
    case RunState::Running: return QStringLiteral("running");
    case RunState::Switching: return QStringLiteral("switching");
    case RunState::Crashed: return QStringLiteral("crashed");
    case RunState::Idle: return QStringLiteral("idle");
    }
    return QStringLiteral("unknown");
}

}
