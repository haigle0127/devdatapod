#include "core/process/ProcessStat.h"

#include <QDateTime>
#include <QThread>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <libproc.h>
#include <sys/proc_info.h>
#endif

namespace devdatapod::proc {

qint64 ResourceSampler::processTimeNs(qint64 pid, qint64* rssBytes) {
#ifdef Q_OS_WIN
    // K32GetProcessMemoryInfo lives in kernel32 on Vista and later, so including psapi.h is
    // enough - no extra import library.
    HANDLE handle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE,
                               static_cast<DWORD>(pid));
    if (!handle) return -1;
    FILETIME creation{}, exitTime{}, kernel{}, user{};
    qint64 total = -1;
    if (GetProcessTimes(handle, &creation, &exitTime, &kernel, &user)) {
        auto toNs = [](const FILETIME& value) {
            const quint64 ticks = (quint64(value.dwHighDateTime) << 32) | value.dwLowDateTime;
            return static_cast<qint64>(ticks * 100);    // 100ns ticks
        };
        total = toNs(kernel) + toNs(user);
    }
    if (rssBytes) {
        PROCESS_MEMORY_COUNTERS counters{};
        counters.cb = sizeof(counters);
        if (K32GetProcessMemoryInfo(handle, &counters, sizeof(counters))) {
            *rssBytes = static_cast<qint64>(counters.WorkingSetSize);
        }
    }
    CloseHandle(handle);
    return total;
#else
    rusage_info_v2 info{};
    if (proc_pid_rusage(static_cast<int>(pid), RUSAGE_INFO_V2,
                        reinterpret_cast<rusage_info_t*>(&info)) != 0) {
        return -1;
    }
    if (rssBytes) *rssBytes = static_cast<qint64>(info.ri_phys_footprint);
    return static_cast<qint64>(info.ri_user_time + info.ri_system_time);    // already ns
#endif
}

ResourceSample ResourceSampler::sample(const QVector<qint64>& pids) {
    ResourceSample out;
    if (pids.isEmpty()) {
        reset();
        return out;
    }

    qint64 cpuTotal = 0;
    qint64 rssTotal = 0;
    int observed = 0;
    for (qint64 pid : pids) {
        qint64 rss = 0;
        const qint64 cpuNs = processTimeNs(pid, &rss);
        if (cpuNs < 0) continue;        // gone, or not ours to read
        ++observed;
        cpuTotal += cpuNs;
        rssTotal += rss;
    }
    if (observed == 0) {
        reset();
        return out;
    }

    out.valid = true;
    out.rssBytes = rssTotal;

    const QDateTime now = QDateTime::currentDateTime();
    if (baselineNs_ < 0 || cpuTotal < baselineNs_) {
        // First tick, or the set shrank and the sum went backwards: no rate yet.
        baselineNs_ = cpuTotal;
        baselineAt_ = now;
        return out;
    }
    const qint64 wallNs = baselineAt_.msecsTo(now) * 1000000LL;
    const int cores = qMax(1, QThread::idealThreadCount());
    if (wallNs > 0) {
        out.cpuPercent = double(cpuTotal - baselineNs_) / double(wallNs) / double(cores) * 100.0;
    }
    baselineNs_ = cpuTotal;
    baselineAt_ = now;
    return out;
}

}
