#pragma once

#include <QDateTime>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

namespace devdatapod::proc {

// One supervised service instance: a single direct child plus everything it forks, inside
// a container that dies with the panel.
//
// This is AGENTS.md rule 5, the whole reason it exists: after the panel crashes or is
// killed there must be no nginx/php/redis left listening. On Windows that is a Job Object
// with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE - closing the handle (which the OS does for us
// when the process dies) tears the tree down. On macOS the child is put in its own process
// group and the group is signalled.
//
// The child is created suspended and assigned to the job *before* it runs, so there is no
// window where a process exists that the panel cannot take with it.
class Supervisor {
public:
    explicit Supervisor(const QString& label);
    ~Supervisor();

    Supervisor(const Supervisor&) = delete;
    Supervisor& operator=(const Supervisor&) = delete;

    // False when the container could not be created, which means starting anything here
    // would risk orphans. start() refuses in that state.
    bool isValid() const { return valid_; }
    QString problem() const { return problem_; }
    QString label() const { return label_; }

    void setWorkingDirectory(const QString& dir) { workingDir_ = dir; }
    // stdout and stderr merge into this file, appended. Empty = inherit (log view then has
    // nothing to show, which is fine for a foreground CLI like php --version).
    void setLogPath(const QString& path) { logPath_ = path; }
    void setEnvironment(const QString& name, const QString& value) { env_.insert(name, value); }

    bool start(const QString& program, const QStringList& arguments, QString* error = nullptr);

    bool isRunning() const;
    qint64 pid() const { return pid_; }
    // Refreshes isRunning() first, so the cached code is current.
    int exitCode() const;
    QDateTime startedAt() const { return startedAt_; }

    // Every process inside the container right now: the direct child plus the workers it
    // forked. stats() aggregates over this list, which is what keeps the home-page rings
    // per instance instead of system-wide (docs/ARCHITECTURE.md 资源统计).
    QVector<qint64> members() const;

    // SIGTERM to the whole group. Windows has no such thing for a job, so it reports false
    // and the driver is expected to have used the service's own shutdown verb already.
    bool signalGraceful();
    bool waitFinished(int msecs);
    // Last resort: take the tree down now.
    bool killForced();

private:
    bool createContainer(QString* error);
    void destroyContainer();
#ifdef Q_OS_WIN
    QString environmentBlock() const;    // UTF-16 "k=v\0" list, extra NUL at the end
#else
    bool signalGroup(int signal) const;
#endif

    QString label_;
    QString workingDir_;
    QString logPath_;
    QHash<QString, QString> env_;

    qint64 pid_ = 0;
    mutable qintptr native_ = 0;    // Windows: process HANDLE
    qintptr container_ = 0;         // Windows: job HANDLE
    mutable int exitCode_ = -1;
    mutable bool finished_ = false;
    bool valid_ = false;
    QDateTime startedAt_;
    QString problem_;
};

}
