#include "core/net/PortProbe.h"

#include <QtGlobal>

#if defined(Q_OS_WIN)
// WIN32_LEAN_AND_MEAN keeps the legacy winsock.h out of windows.h, so AF_INET and ntohs
// arrive transitively from ws2tcpip.h. iphlpapi.h does not include windows.h itself - it
// uses DWORD/HANDLE/WINAPI unqualified - so windows.h must come first.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>
#endif

#include "core/util/Log.h"

#include <QByteArray>
#include <QFileInfo>
#include <QHostAddress>
#include <QTcpServer>

#if defined(Q_OS_MACOS)
#include <QProcess>
#endif

namespace devdatapod::net {

bool isPortFree(quint16 port, const QString& address) {
    QHostAddress host;
    if (!host.setAddress(address)) host = QHostAddress(QHostAddress::LocalHost);

    QTcpServer server;
    if (!server.listen(host, port)) return false;
    server.close();
    return true;
}

PortOccupier identifyPortOwner(quint16 port) {
    PortOccupier out;
    out.port = port;

#if defined(Q_OS_WIN)
    ULONG size = 0;
    const ULONG probe = GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET,
                                           TCP_TABLE_OWNER_PID_LISTENER, 0);
    if (probe != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        log::warn(QStringLiteral("ports: no TCP table (win32 error %1)").arg(probe));
        return out;
    }
    QByteArray table(static_cast<int>(size), '\0');
    if (GetExtendedTcpTable(table.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER,
                            0) != NO_ERROR) {
        return out;
    }
    const auto* rows = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(table.constData());
    for (DWORD i = 0; i < rows->dwNumEntries; ++i) {
        const MIB_TCPROW_OWNER_PID& row = rows->table[i];
        if (ntohs(static_cast<u_short>(row.dwLocalPort)) != port) continue;
        out.occupied = true;
        out.pid = row.dwOwningPid;
        // dwLocalAddr is network byte order; QHostAddress(quint32) wants host byte order.
        out.address = QHostAddress(static_cast<quint32>(ntohl(row.dwLocalAddr))).toString();

        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                     static_cast<DWORD>(row.dwOwningPid));
        if (process) {
            wchar_t path[MAX_PATH] = {};
            DWORD chars = MAX_PATH;
            if (QueryFullProcessImageNameW(process, 0, path, &chars)) {
                out.executable = QString::fromWCharArray(path, static_cast<int>(chars));
                out.processName = QFileInfo(out.executable).fileName();
            }
            CloseHandle(process);
        }
        return out;
    }
#elif defined(Q_OS_MACOS)
    // No public API for "who owns this socket" outside the kernel; lsof is the standard
    // answer. It only sees the current user's processes, which covers everything devdatapod
    // starts itself and most of what a single-user mac runs.
    QProcess lsof;
    lsof.start(QStringLiteral("lsof"), {QStringLiteral("-nP"),
                                        QStringLiteral("-iTCP:%1").arg(port),
                                        QStringLiteral("-sTCP=LISTEN"), QStringLiteral("-Fpcn")});
    if (!lsof.waitForFinished(3000)) return out;
    const QList<QByteArray> lines = lsof.readAllStandardOutput().split('\n');
    QByteArray command;
    for (const QByteArray& line : lines) {
        if (line.startsWith('p')) out.pid = line.mid(1).toLongLong();
        else if (line.startsWith('c')) command = line.mid(1);
        else if (line.startsWith('n')) out.address = QString::fromUtf8(line.mid(1));
    }
    if (out.pid) {
        out.occupied = true;
        out.processName = QString::fromUtf8(command);
    }
#else
    Q_UNUSED(port);
#endif
    return out;
}

}
