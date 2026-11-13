#include "core/process/OrphanSweep.h"

#include "core/util/Log.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QVector>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// tlhelp32.h documents that windows.h must come first; it does not include it.
#include <tlhelp32.h>
#else
#include <csignal>
#include <libproc.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace devdatapod::proc {

namespace {

// Every path compared here decides whether a process gets terminated, so both sides go
// through the same normalization: cleanPath (forward slashes on every platform), the \\?\
// prefix that QueryFullProcessImageNameW sometimes returns, and folded case on Windows.
QString normalizePath(const QString& raw) {
    QString out = QDir::cleanPath(raw);
    if (out.startsWith(QLatin1String("\\\\?\\"))) out = out.mid(4);
#ifdef Q_OS_WIN
    out = out.toLower();
#endif
    return out;
}

QString normalizedRoot(const QString& root) {
    QString out = normalizePath(QFileInfo(root).absoluteFilePath());
    if (!out.endsWith(QLatin1Char('/'))) out += QLatin1Char('/');
    return out;
}

// The trailing separator carried over from normalizedRoot is what keeps a sibling install
// (D:/devdatapod2 under root D:/devdatapod) from matching.
bool underRoot(const QString& normalizedRoot, const QString& path) {
    if (path.isEmpty()) return false;
    const QString candidate = normalizePath(path);
    return candidate.size() > normalizedRoot.size() && candidate.startsWith(normalizedRoot);
}

#ifdef Q_OS_WIN
QString imagePath(HANDLE process) {
    QVector<wchar_t> buffer(4096);
    DWORD chars = DWORD(buffer.size());
    if (!QueryFullProcessImageNameW(process, 0, buffer.data(), &chars)) {
        if (GetLastError() != ERROR_MORE_DATA || chars <= 0) return QString();
        buffer.resize(int(chars) + 1);
        chars = DWORD(buffer.size());
        if (!QueryFullProcessImageNameW(process, 0, buffer.data(), &chars)) return QString();
    }
    return QString::fromWCharArray(buffer.data(), int(chars));
}
#endif

}

QVector<Orphan> scanOrphans(const QString& root) {
    QVector<Orphan> out;
    if (root.isEmpty()) return out;
    const QString normalized = normalizedRoot(root);
    const qint64 self = QCoreApplication::applicationPid();

#ifdef Q_OS_WIN
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        log::error(QStringLiteral("sweep: CreateToolhelp32Snapshot failed (%1)")
                       .arg(GetLastError()));
        return out;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            const qint64 pid = static_cast<qint64>(entry.th32ProcessID);
            if (pid == self) continue;
            // QUERY_LIMITED_INFORMATION works across bitness and without SeDebugPrivilege,
            // which is what a green app started from Explorer has.
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                         entry.th32ProcessID);
            if (!process) continue;
            const QString path = imagePath(process);
            CloseHandle(process);
            if (!underRoot(normalized, path)) continue;
            Orphan orphan;
            orphan.pid = pid;
            orphan.executable = path;
            orphan.name = QFileInfo(path).fileName();
            out.append(orphan);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
#else
    // proc_listallpids is the documented enumeration and needs the whole buffer up front;
    // 4096 pids is plenty for a single-user machine.
    const qint64 capacity = 4096;
    QVector<pid_t> pids(int(capacity));
    // Returns bytes, not a pid count: the xnu idiom is to divide by sizeof(pid_t).
    const int bytes = proc_listallpids(pids.data(), int(pids.size() * sizeof(pid_t)));
    const int count = bytes > 0 ? qMin(bytes / int(sizeof(pid_t)), pids.size()) : 0;
    for (int i = 0; i < count; ++i) {
        const qint64 pid = static_cast<qint64>(pids.at(i));
        if (pid <= 0 || pid == self) continue;
        QVector<char> buffer(int(PROC_PIDPATHINFO_MAXSIZE), '\0');
        const int written = proc_pidpath(pid_t(pid), buffer.data(), PROC_PIDPATHINFO_MAXSIZE);
        if (written <= 0) continue;
        const QString path = QString::fromUtf8(buffer.constData());
        if (!underRoot(normalized, path)) continue;
        Orphan orphan;
        orphan.pid = pid;
        orphan.executable = path;
        orphan.name = QFileInfo(path).fileName();
        out.append(orphan);
    }
#endif
    return out;
}

QVector<qint64> killOrphans(const QVector<Orphan>& orphans) {
    QVector<qint64> gone;
    for (const Orphan& orphan : orphans) {
#ifdef Q_OS_WIN
        HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE,
                                     DWORD(orphan.pid));
        if (process) {
            const bool terminated = TerminateProcess(process, 1) != FALSE;
            if (terminated) {
                // Without the wait the caller's port check still sees the old listener for a
                // few milliseconds and reports a conflict that is already resolved.
                WaitForSingleObject(process, 3000);
            }
            CloseHandle(process);
            if (terminated) {
                gone.append(orphan.pid);
                continue;
            }
        }
#else
        // No graceful option here: these are leftovers from a dead panel, and the only
        // language we speak to them is a signal, while the point of the sweep is that
        // nothing may keep listening.
        if (::kill(pid_t(orphan.pid), SIGKILL) == 0) {
            gone.append(orphan.pid);
            continue;
        }
#endif
        log::error(QStringLiteral("sweep: could not terminate pid %1 (%2)")
                       .arg(orphan.pid)
                       .arg(orphan.name));
    }
    return gone;
}

int sweepInstallRoot(const QString& root, QStringList* report) {
    const QVector<Orphan> orphans = scanOrphans(root);
    if (orphans.isEmpty()) return 0;
    const QVector<qint64> gone = killOrphans(orphans);
    for (const Orphan& orphan : orphans) {
        const bool dead = gone.contains(orphan.pid);
        const QString line = QStringLiteral("%1 pid %2 %3")
                                 .arg(dead ? QStringLiteral("killed") : QStringLiteral("SURVIVED"))
                                 .arg(orphan.pid)
                                 .arg(orphan.executable);
        if (report) report->append(line);
        if (dead) {
            log::warn(QStringLiteral("sweep: %1").arg(line));
        } else {
            log::error(QStringLiteral("sweep: %1").arg(line));
        }
    }
    return static_cast<int>(gone.size());
}

}
