#include "app/SelfTest.h"
#include "app/Stage.h"
#include "core/util/AppPaths.h"
#include "core/util/Log.h"

#include <QCoreApplication>
#include <QStringList>

// Console entry point for the devdatapod-selftest target. It exists as a separate executable
// because a windowed process does not make a shell wait, and P2's acceptance evidence is an
// exit code plus the check list on stdout - neither of which cmd or PowerShell can read from
// a GUI subsystem binary.
//
// Two modes: with no arguments it proves the model invariants inside a throwaway temp root;
// with --stage it registers component directories in the real data/ of the install root,
// which is what the live two-Redis acceptance needs before P5 ships an installer.
//
// QCoreApplication, not QApplication: this target links no Gui and no Widgets, which is what
// keeps src/core honest about not depending on the UI (AGENTS.md rule 3).
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("devdatapod-selftest"));
    QCoreApplication::setApplicationVersion(QString::fromLatin1(DEVDATAPOD_VERSION));
    QCoreApplication::setOrganizationName(QStringLiteral("devdatapod"));

    // Creates data/ and logs/ next to this executable. The invariant checks still run in a
    // throwaway temp root, but --stage writes this root for real - a failed assertion with no
    // log line is far harder to diagnose, so logging is initialised either way.
    devdatapod::paths::ensureWritableDirs();
    devdatapod::log::init();
    devdatapod::log::installQtMessageHandler();

    QStringList arguments = QCoreApplication::arguments();
    arguments.removeFirst();
    const int stageAt = arguments.indexOf(QStringLiteral("--stage"));
    const int runAt = arguments.indexOf(QStringLiteral("--run"));
    int code = 0;
    if (runAt >= 0) {
        const QString id = arguments.value(runAt + 1);
        code = id.isEmpty() ? 1 : devdatapod::runInstance(id);
    } else {
        code = stageAt < 0 ? devdatapod::runSelfTest()
                           : devdatapod::runStage(arguments.mid(stageAt + 1));
    }
    devdatapod::log::shutdown();
    return code;
}