#include "core/process/RunOnce.h"

#include "core/util/Log.h"
#include "core/util/LogTailReader.h"

#include <QProcess>
#include <QRegularExpression>

namespace devdatapod::proc {

CommandResult runOnce(const QString& program, const QStringList& arguments, int timeoutMs,
                      const QString& workingDir) {
    CommandResult out;
    QProcess process;
    if (!workingDir.isEmpty()) process.setWorkingDirectory(workingDir);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(program, arguments);
    if (!process.waitForStarted(timeoutMs)) {
        log::warn(QStringLiteral("runOnce: %1 never started (%2)")
                      .arg(program, process.errorString()));
        return out;
    }
    out.ran = true;
    if (!process.waitForFinished(timeoutMs)) {
        log::warn(QStringLiteral("runOnce: %1 timed out after %2ms, killed")
                      .arg(program)
                      .arg(timeoutMs));
        process.kill();
        process.waitForFinished(1000);
    }
    out.output = text::decode(process.readAllStandardOutput());
    out.errorOutput = text::decode(process.readAllStandardError());
    out.exitCode = process.exitCode();
    return out;
}

QString extractVersion(const QString& text) {
    static const QRegularExpression pattern(QStringLiteral("(\\d+\\.\\d+(?:\\.\\d+)?)"));
    const QRegularExpressionMatch match = pattern.match(text);
    return match.hasMatch() ? match.captured(1) : QString();
}

}
