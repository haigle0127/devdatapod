#include "core/process/Supervisor.h"

#include "core/util/Log.h"

#include <QFileInfo>
#include <QList>
#include <QProcessEnvironment>

#include <algorithm>
#include <string>
#include <vector>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/proc_info.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace devdatapod::proc {

namespace {

#ifdef Q_OS_WIN
std::wstring toNative(const QString& text) {
    std::wstring wide(static_cast<size_t>(text.size()), L'\0');
    const int written = text.toWCharArray(&wide[0]);
    wide.resize(static_cast<size_t>(written));
    return wide;
}

// The MSVC(argv) quoting rules the child's CRT applies when it re-splits the command line.
// nginx and php parse their own arguments and are unforgiving about a mangled "-d foo=bar",
// so backslashes before a quote get doubled and the quote gets escaped.
QString quoteArgument(const QString& raw) {
    if (raw.isEmpty()) return QStringLiteral("\"\"");
    if (!raw.contains(QLatin1Char(' ')) && !raw.contains(QLatin1Char('\t'))
        && !raw.contains(QLatin1Char('"'))) {
        return raw;
    }
    QString out;
    out.reserve(raw.size() + 2);
    out += QLatin1Char('"');
    int backslashes = 0;
    for (const QChar ch : raw) {
        if (ch == QLatin1Char('\\')) {
            ++backslashes;
            out += ch;
            continue;
        }
        if (ch == QLatin1Char('"')) {
            out += QString(backslashes + 1, QLatin1Char('\\'));
            out += ch;
            backslashes = 0;
            continue;
        }
        backslashes = 0;
        out += ch;
    }
    out += QString(backslashes, QLatin1Char('\\'));
    out += QLatin1Char('"');
    return out;
}
#endif

void setProblem(QString* out, const QString& text) {
    if (out) *out = text;
}

}

Supervisor::Supervisor(const QString& label) : label_(label) {
    QString error;
    if (!createContainer(&error)) {
        problem_ = error;
        log::error(QStringLiteral("proc/%1: container unavailable: %2").arg(label_, error));
        return;
    }
    valid_ = true;
}

Supervisor::~Supervisor() {
#ifndef Q_OS_WIN
    // Windows needs nothing here: closing the job handle below fires KILL_ON_JOB_CLOSE. On
    // macOS there is no such backstop, so a driver destroyed while its child still runs
    // would leave the group listening (rule 5).
    if (isRunning()) killForced();
#endif
    destroyContainer();
}

// ---------------------------------------------------------------------------- container
bool Supervisor::createContainer(QString* error) {
#ifdef Q_OS_WIN
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) {
        setProblem(error, QStringLiteral("CreateJobObjectW failed (%1)").arg(GetLastError()));
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        const DWORD code = GetLastError();
        CloseHandle(job);
        setProblem(error, QStringLiteral("SetInformationJobObject failed (%1)").arg(code));
        return false;
    }
    container_ = reinterpret_cast<qintptr>(job);
    return true;
#else
    // macOS has no job object. The child gets its own process group at spawn time and the
    // group is what every signal below is aimed at, so there is nothing to create here.
    Q_UNUSED(error);
    return true;
#endif
}

void Supervisor::destroyContainer() {
#ifdef Q_OS_WIN
    if (native_) {
        CloseHandle(reinterpret_cast<HANDLE>(native_));
        native_ = 0;
    }
    // Closing the job handle is what triggers KILL_ON_JOB_CLOSE: any descendant still
    // inside dies here, including on the crash path where the OS closes it for us.
    if (container_) {
        CloseHandle(reinterpret_cast<HANDLE>(container_));
        container_ = 0;
    }
#endif
    pid_ = 0;
}

#ifdef Q_OS_WIN
QString Supervisor::environmentBlock() const {
    struct Entry { QString name; QString value; };
    const QProcessEnvironment base = QProcessEnvironment::systemEnvironment();
    QHash<QString, Entry> merged;
    for (const QString& name : base.keys()) {
        merged.insert(name.toUpper(), Entry{name, base.value(name)});
    }
    for (auto it = env_.constBegin(); it != env_.constEnd(); ++it) {
        const QString upper = it.key().toUpper();
        auto found = merged.find(upper);
        if (found != merged.end()) found->value = it.value();
        else merged.insert(upper, Entry{it.key(), it.value()});
    }
    // CreateProcessW documents a sorted block; iterating a QHash would give an order that
    // changes between runs, which some child CRTs take as a broken environment.
    QList<Entry> entries;
    entries.reserve(merged.size());
    for (const Entry& entry : merged) entries.append(entry);
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    QString block;
    for (const Entry& entry : entries) {
        block += entry.name;
        block += QLatin1Char('=');
        block += entry.value;
        block += QLatin1Char('\0');
    }
    block += QLatin1Char('\0');
    return block;
}
#endif

#ifndef Q_OS_WIN
bool Supervisor::signalGroup(int signal) const {
    if (pid_ <= 0) return false;
    return ::kill(static_cast<pid_t>(-pid_), signal) == 0;
}
#endif

// ------------------------------------------------------------------------------- start
bool Supervisor::start(const QString& program, const QStringList& arguments, QString* error) {
    if (!valid_) {
        setProblem(error, QStringLiteral("no orphan protection for %1 (%2), refusing to spawn")
                              .arg(label_, problem_));
        return false;
    }
    if (pid_ != 0 && isRunning()) {
        setProblem(error, QStringLiteral("%1 is already running as pid %2")
                              .arg(label_)
                              .arg(pid_));
        return false;
    }
    if (!QFileInfo::exists(program)) {
        setProblem(error, QStringLiteral("%1 does not exist").arg(program));
        return false;
    }
#ifdef Q_OS_WIN
    // Restarting on the same Supervisor is the normal path (stop, then start with a new
    // port). The old handle refers to an exited process; leaving it open would leak one per
    // switch and keep the exit code of the previous run readable.
    if (native_) {
        CloseHandle(reinterpret_cast<HANDLE>(native_));
        native_ = 0;
    }
    pid_ = 0;

    const std::wstring application = toNative(QFileInfo(program).absoluteFilePath());
    QString line = quoteArgument(program);
    for (const QString& arg : arguments) {
        line += QLatin1Char(' ');
        line += quoteArgument(arg);
    }
    std::wstring commandLine = toNative(line);
    const QString block = environmentBlock();

    SECURITY_ATTRIBUTES inheritable{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    HANDLE logHandle = nullptr;
    if (!logPath_.isEmpty()) {
        const std::wstring path = toNative(logPath_);
        logHandle = CreateFileW(path.c_str(), GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
        if (logHandle == INVALID_HANDLE_VALUE) {
            log::warn(QStringLiteral("proc/%1: cannot open log %2 (%3), output will be lost")
                          .arg(label_, logPath_)
                          .arg(GetLastError()));
            logHandle = nullptr;
        } else {
            SetFilePointer(logHandle, 0, nullptr, FILE_END);
        }
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    if (logHandle) {
        startup.dwFlags |= STARTF_USESTDHANDLES;
        startup.hStdOutput = logHandle;
        startup.hStdError = logHandle;
    }
    const std::wstring working = workingDir_.isEmpty()
                                     ? std::wstring()
                                     : toNative(QFileInfo(workingDir_).absoluteFilePath());

    PROCESS_INFORMATION info{};
    DWORD flags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;
    bool created = CreateProcessW(application.c_str(), &commandLine[0], nullptr, nullptr, TRUE,
                                  flags,
                                  block.isEmpty() ? nullptr : (LPVOID)block.utf16(),
                                  working.empty() ? nullptr : working.c_str(), &startup, &info);
    if (created) {
        // Assign before the thread runs. If the panel dies between these two calls the OS
        // still owns the child through the job.
        if (!AssignProcessToJobObject(reinterpret_cast<HANDLE>(container_), info.hProcess)) {
            const DWORD assignCode = GetLastError();
            CloseHandle(info.hThread);
            CloseHandle(info.hProcess);
            // Reached when the panel itself runs inside a job that forbids nesting. Asking
            // for breakaway is the only way out, and only works if our parent allows it.
            flags |= CREATE_BREAKAWAY_FROM_JOB;
            info = PROCESS_INFORMATION{};
            if (CreateProcessW(application.c_str(), &commandLine[0], nullptr, nullptr, TRUE,
                               flags, block.isEmpty() ? nullptr : (LPVOID)block.utf16(),
                               working.empty() ? nullptr : working.c_str(), &startup, &info)) {
                if (!AssignProcessToJobObject(reinterpret_cast<HANDLE>(container_),
                                              info.hProcess)) {
                    const DWORD second = GetLastError();
                    TerminateProcess(info.hProcess, 1);
                    CloseHandle(info.hThread);
                    CloseHandle(info.hProcess);
                    if (logHandle) CloseHandle(logHandle);
                    setProblem(error,
                               QStringLiteral("cannot put %1 in a job object (%2, then %3): "
                                              "orphan protection is impossible here")
                                   .arg(label_)
                                   .arg(assignCode)
                                   .arg(second));
                    return false;
                }
            } else {
                if (logHandle) CloseHandle(logHandle);
                setProblem(error, QStringLiteral("CreateProcessW for %1 failed (%2)")
                                      .arg(label_)
                                      .arg(GetLastError()));
                return false;
            }
        }
        ResumeThread(info.hThread);
        native_ = reinterpret_cast<qintptr>(info.hProcess);
        pid_ = static_cast<qint64>(info.dwProcessId);
        CloseHandle(info.hThread);
        if (logHandle) CloseHandle(logHandle);    // the child holds its own copy now
        finished_ = false;
        exitCode_ = -1;
        startedAt_ = QDateTime::currentDateTime();
        log::info(QStringLiteral("proc/%1: started pid %2").arg(label_).arg(pid_));
        return true;
    }

    if (logHandle) CloseHandle(logHandle);
    setProblem(error, QStringLiteral("CreateProcessW for %1 failed (%2)")
                          .arg(label_)
                          .arg(GetLastError()));
    return false;
#else
    const QByteArray nativeProgram = QFileInfo(program).absoluteFilePath().toUtf8();

    std::vector<QByteArray> argumentStore;
    argumentStore.reserve(static_cast<size_t>(arguments.size()) + 1);
    argumentStore.push_back(nativeProgram);
    for (const QString& arg : arguments) argumentStore.push_back(arg.toUtf8());
    std::vector<char*> argv;
    argv.reserve(argumentStore.size() + 1);
    for (QByteArray& stored : argumentStore) argv.push_back(stored.data());
    argv.push_back(nullptr);

    const QProcessEnvironment base = QProcessEnvironment::systemEnvironment();
    QHash<QString, QString> merged;
    for (const QString& name : base.keys()) merged.insert(name, base.value(name));
    for (auto it = env_.constBegin(); it != env_.constEnd(); ++it) merged.insert(it.key(), it.value());
    std::vector<QByteArray> environmentStore;
    environmentStore.reserve(static_cast<size_t>(merged.size()));
    for (auto it = merged.constBegin(); it != merged.constEnd(); ++it) {
        environmentStore.push_back((it.key() + QLatin1Char('=') + it.value()).toUtf8());
    }
    std::vector<char*> envp;
    envp.reserve(environmentStore.size() + 1);
    for (QByteArray& stored : environmentStore) envp.push_back(stored.data());
    envp.push_back(nullptr);

    posix_spawnattr_t attributes;
    if (posix_spawnattr_init(&attributes) != 0) {
        setProblem(error, QStringLiteral("posix_spawnattr_init failed"));
        return false;
    }
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);    // child becomes its own group leader

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    QByteArray nativeLog;
    if (!logPath_.isEmpty()) {
        nativeLog = logPath_.toUtf8();
        if (posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, nativeLog.constData(),
                                            O_WRONLY | O_CREAT | O_APPEND, 0644) != 0) {
            log::warn(QStringLiteral("proc/%1: cannot redirect output to %2")
                          .arg(label_, logPath_));
        } else {
            posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
        }
    }

    pid_t child = 0;
    const int rc = posix_spawn(&child, nativeProgram.constData(), &actions, &attributes,
                               argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);

    if (rc != 0) {
        setProblem(error, QStringLiteral("posix_spawn(%1) failed: %2")
                              .arg(program, QString::fromUtf8(strerror(rc))));
        return false;
    }
    pid_ = static_cast<qint64>(child);
    finished_ = false;
    exitCode_ = -1;
    startedAt_ = QDateTime::currentDateTime();
    log::info(QStringLiteral("proc/%1: started pid %2 in its own group").arg(label_).arg(pid_));
    return true;
#endif
}

// ------------------------------------------------------------------------------ observe
bool Supervisor::isRunning() const {
    if (pid_ <= 0 || finished_) return false;
#ifdef Q_OS_WIN
    if (!native_) {
        finished_ = true;
        return false;
    }
    DWORD code = 0;
    if (!GetExitCodeProcess(reinterpret_cast<HANDLE>(native_), &code)) {
        finished_ = true;
        return false;
    }
    // STILL_ACTIVE is 259, so an exit code of 259 reads as "still running" once. Every
    // caller follows up with waitFinished() or killForced(), which resolves it.
    if (code == STILL_ACTIVE) return true;
    finished_ = true;
    exitCode_ = static_cast<int>(code);
    return false;
#else
    int status = 0;
    const pid_t result = ::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
    if (result == 0) return true;
    finished_ = true;
    if (result == static_cast<pid_t>(pid_)) {
        exitCode_ = WIFEXITED(status)
                        ? WEXITSTATUS(status)
                        : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    }
    return false;
#endif
}

bool Supervisor::waitFinished(int msecs) {
#ifdef Q_OS_WIN
    if (!native_) return true;
    const DWORD result = WaitForSingleObject(reinterpret_cast<HANDLE>(native_),
                                             static_cast<DWORD>(qMax(0, msecs)));
    isRunning();    // pulls the exit code in
    return result == WAIT_OBJECT_0;
#else
    const int step = 20;
    int waited = 0;
    while (isRunning() && waited < qMax(0, msecs)) {
        ::usleep(static_cast<useconds_t>(step) * 1000);
        waited += step;
    }
    return !isRunning();
#endif
}

int Supervisor::exitCode() const {
    isRunning();
    return exitCode_;
}

QVector<qint64> Supervisor::members() const {
    QVector<qint64> out;
    if (pid_ <= 0 || !isRunning()) return out;
#ifdef Q_OS_WIN
    // The job knows exactly who is inside it, including workers the master forked after
    // we started it. 64 slots is plenty for a php-fpm pool; if it ever fills up we report
    // the processes that fit rather than nothing.
    std::vector<BYTE> buffer(sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST)
                             + sizeof(ULONG_PTR) * 64);
    DWORD needed = 0;
    const BOOL ok = QueryInformationJobObject(
        reinterpret_cast<HANDLE>(container_), JobObjectBasicProcessIdList, buffer.data(),
        static_cast<DWORD>(buffer.size()), &needed);
    if (!ok) return out;
    const auto* list = reinterpret_cast<const JOBOBJECT_BASIC_PROCESS_ID_LIST*>(buffer.data());
    const DWORD count = qMin(list->NumberOfProcessIdsInList, list->NumberOfAssignedProcesses);
    for (DWORD i = 0; i < count; ++i) {
        out.append(static_cast<qint64>(list->ProcessIdList[i]));
    }
#else
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PGRP, static_cast<int>(pid_)};
    size_t length = 0;
    if (::sysctl(mib, 4, nullptr, &length, nullptr, 0) != 0 || length == 0) {
        out.append(pid_);
        return out;
    }
    std::vector<kinfo_proc> table(length / sizeof(kinfo_proc));
    if (::sysctl(mib, 4, table.data(), &length, nullptr, 0) != 0) {
        out.append(pid_);
        return out;
    }
    for (const kinfo_proc& entry : table) out.append(entry.kp_proc.p_pid);
#endif
    return out;
}

// ------------------------------------------------------------------------------- teardown
bool Supervisor::signalGraceful() {
#ifdef Q_OS_WIN
    // A job object has no "please exit" signal. The driver is expected to have sent the
    // service's own shutdown verb (redis SHUTDOWN, nginx -s quit) before escalating.
    log::info(QStringLiteral("proc/%1: Windows has no group signal, use the service verb")
                  .arg(label_));
    return false;
#else
    if (!isRunning()) return true;
    if (!signalGroup(SIGTERM)) {
        log::warn(QStringLiteral("proc/%1: SIGTERM to group %2 failed").arg(label_).arg(pid_));
        return false;
    }
    return true;
#endif
}

bool Supervisor::killForced() {
#ifdef Q_OS_WIN
    if (!container_) return !isRunning();
    if (!TerminateJobObject(reinterpret_cast<HANDLE>(container_), 0)) {
        log::error(QStringLiteral("proc/%1: TerminateJobObject failed (%2)")
                       .arg(label_)
                       .arg(GetLastError()));
        return false;
    }
    waitFinished(2000);
    return true;
#else
    if (!isRunning()) return true;
    if (!signalGroup(SIGKILL)) signalGroup(SIGTERM);
    const bool gone = waitFinished(2000);
    if (!gone) {
        log::error(QStringLiteral("proc/%1: pid %2 survived SIGKILL").arg(label_).arg(pid_));
    }
    return gone;
#endif
}

}
