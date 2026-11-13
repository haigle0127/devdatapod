#pragma once

#include <QString>
#include <QStringList>

namespace devdatapod::proc {

// One short-lived command: `<program> --version`, mysqladmin shutdown, redis-cli INFO.
// Not for services - anything that keeps listening belongs to a Supervisor so the job
// object / process group owns its lifetime (rule 5). A command that outlives its timeout
// is killed here rather than left behind.
struct CommandResult {
    bool ran = false;
    int exitCode = -1;
    QString output;        // stdout, already decoded
    QString errorOutput;
};

CommandResult runOnce(const QString& program, const QStringList& arguments, int timeoutMs = 5000,
                      const QString& workingDir = QString());

// First "x.y[.z]" in the text: redis prints "Redis server v=7.2.5 sha=...", nginx
// "nginx version: nginx/1.25.4", php "PHP 8.3.10 (cli)".
QString extractVersion(const QString& text);

}
