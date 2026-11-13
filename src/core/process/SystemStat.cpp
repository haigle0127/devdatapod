#include "core/process/SystemStat.h"

#include <QByteArray>
#include <QSet>
#include <QSettings>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
// macOS 的实现等真机移植时补（host_processor_info / statfs），先返回 invalid，
// 首页仪表画「--」而不是编一个数。
#endif

namespace devdatapod::sys {

namespace {

constexpr double kClampMax = 1.0;

double clamp01(double value) {
    if (value < 0.0) return 0.0;
    if (value > kClampMax) return kClampMax;
    return value;
}

} // namespace

double Snapshot::memoryFraction() const {
    if (!valid || totalPhysBytes == 0) return 0.0;
    return clamp01(1.0 - static_cast<double>(availPhysBytes)
                             / static_cast<double>(totalPhysBytes));
}

CpuTopology cpuTopology() {
    static const CpuTopology cached = [] {
        CpuTopology out;
#ifdef Q_OS_WIN
        // 物理核：每个 RelationProcessorCore 条目就是一个核，核内线程数看它的掩码。
        DWORD length = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
        if (length > 0) {
            QByteArray buffer(static_cast<int>(length), 0);
            if (GetLogicalProcessorInformationEx(
                    RelationProcessorCore,
                    reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()),
                    &length)) {
                char* cursor = buffer.data();
                char* end = buffer.data() + length;
                while (cursor < end) {
                    const auto* info =
                        reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(cursor);
                    if (info->Relationship == RelationProcessorCore) ++out.physicalCores;
                    cursor += info->Size;
                }
            }
        }

        SYSTEM_INFO systemInfo{};
        GetSystemInfo(&systemInfo);
        out.logicalProcessors = static_cast<int>(systemInfo.dwNumberOfProcessors);

        // 大小核：CPU set 的 EfficiencyClass 0 = 高性能核（P），>0 = 能效核（E）。
        // 按 CoreIndex 去重数物理核。非混合架构（老 U、AMD）全部落在 class 0。
        ULONG cpuSetLength = 0;
        GetSystemCpuSetInformation(nullptr, 0, &cpuSetLength, GetCurrentProcess(), 0);
        if (cpuSetLength > 0) {
            QByteArray buffer(static_cast<int>(cpuSetLength), 0);
            if (GetSystemCpuSetInformation(
                    reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()),
                    cpuSetLength, &cpuSetLength, GetCurrentProcess(), 0)) {
                QSet<ULONG> pCores;
                QSet<ULONG> eCores;
                char* cursor = buffer.data();
                char* end = buffer.data() + cpuSetLength;
                while (cursor < end) {
                    const auto* info =
                        reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(cursor);
                    if (info->Type == CpuSetInformation) {
                        if (info->CpuSet.EfficiencyClass > 0) {
                            eCores.insert(info->CpuSet.CoreIndex);
                        } else {
                            pCores.insert(info->CpuSet.CoreIndex);
                        }
                    }
                    cursor += info->Size;
                }
                out.performanceCores = pCores.size();
                out.efficientCores = eCores.size();
                if (out.physicalCores == 0) {
                    out.physicalCores = out.performanceCores + out.efficientCores;
                }
            }
        }
        if (out.physicalCores == 0) out.performanceCores = out.physicalCores;

        // 型号字符串在注册表里，读一下给详情卡用（只读，符合规则 7 的绿色约定）。
        const QSettings cpuKey(
            QStringLiteral("HKEY_LOCAL_MACHINE\\HARDWARE\\DESCRIPTION\\System\\"
                           "CentralProcessor\\0"),
            QSettings::NativeFormat);
        out.model = cpuKey.value(QStringLiteral("ProcessorNameString")).toString().simplified();

        out.valid = out.logicalProcessors > 0;
#endif
        return out;
    }();
    return cached;
}

qint64 uptimeMs() {
#ifdef Q_OS_WIN
    return static_cast<qint64>(GetTickCount64());
#else
    return -1;
#endif
}

Snapshot Sampler::sample() {
    Snapshot out;
#ifdef Q_OS_WIN
    FILETIME idleTime{}, kernelTime{}, userTime{};
    if (!GetSystemTimes(&idleTime, &kernelTime, &userTime)) return out;

    // FILETIME 是 100ns 间隔的两个 DWORD，拼成 64 位。
    const auto toU64 = [](const FILETIME& ft) {
        return (static_cast<quint64>(ft.dwHighDateTime) << 32)
               | static_cast<quint64>(ft.dwLowDateTime);
    };
    const quint64 idle = toU64(idleTime);
    const quint64 kernel = toU64(kernelTime);
    const quint64 user = toU64(userTime);

    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (!GlobalMemoryStatusEx(&memory)) return out;
    out.totalPhysBytes = memory.ullTotalPhys;
    out.availPhysBytes = memory.ullAvailPhys;

    if (baseline_) {
        const quint64 idleDelta = idle - prevIdle_;
        const quint64 totalDelta = (kernel - prevKernel_) + (user - prevUser_);
        if (totalDelta > 0) {
            out.cpuFraction = clamp01(1.0 - static_cast<double>(idleDelta)
                                              / static_cast<double>(totalDelta));
        }
    }
    prevIdle_ = idle;
    prevKernel_ = kernel;
    prevUser_ = user;
    baseline_ = true;
    out.valid = out.totalPhysBytes > 0;
#endif
    return out;
}

} // namespace devdatapod::sys
