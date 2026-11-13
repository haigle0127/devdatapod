#pragma once

#include <QString>
#include <QStringList>

namespace devdatapod {

// devdatapod-selftest --stage <dir>...: register component directories that are already sitting
// in the install tree, so docs/ROADMAP.md P2's live acceptance (two Redis versions side by
// side, 6379 following whichever one is default, self-healing after the panel is killed
// mid-switch) can be driven by hand. The installer that would do this automatically is P5.
//
// Each argument is a directory under the install root named <kind><version>, e.g.
// Extensions/redis3.0.504. Registration goes through InstanceRegistry, so this writes the
// same data/services.json schema the panel reads - it is not a second copy of that format.
//
// Returns the process exit code: 0 only when every argument registered, reconciled, and
// reported a version its binary actually answers with.
int runStage(const QStringList& arguments);

// devdatapod-selftest --run <id>: start one registered instance through its real driver,
// wait until status() says Running (for every driver that means its protocol answered),
// print the log tail, then stop it. This is the P3 acceptance vehicle: start, readiness
// probe and graceful stop proven without a GUI or a debugger.
//
// Exit code 0 only when the instance reached Running and stopped cleanly.
int runInstance(const QString& id);

}
