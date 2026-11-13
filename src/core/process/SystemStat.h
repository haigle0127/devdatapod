#pragma once

#include <QString>

namespace devdatapod::sys {

// 首页「运行状态」的真实数据来源。只读系统计数器，core 级、不碰 UI
// （AGENTS.md 规则 3），Windows 先行；macOS 留空实现返回 invalid，仪表画成「--」。
struct Snapshot {
    bool valid = false;
    double cpuFraction = 0.0;        // 0..1，系统整体 CPU 使用率
    quint64 totalPhysBytes = 0;
    quint64 availPhysBytes = 0;
    double memoryFraction() const;   // 0..1，(total-avail)/total
};

// CPU 拓扑：核心 / 线程 / P 核 / E 核。混合架构（Intel 12 代+）靠
// GetSystemCpuSetInformation 的 EfficiencyClass 区分大小核；非混合机器 eCores=0。
// 结果是开机不变的静态数据，cpuTopology() 内部只算一次。
struct CpuTopology {
    bool valid = false;
    int physicalCores = 0;        // 物理核（P+E）
    int logicalProcessors = 0;    // 逻辑处理器（含超线程）
    int performanceCores = 0;     // EfficiencyClass 0 的物理核
    int efficientCores = 0;       // EfficiencyClass > 0 的物理核；非混合架构为 0
    QString model;                // 注册表里的 CPU 型号字符串，可能为空
    bool hybrid() const { return efficientCores > 0; }
};

// CPU 使用率是两个时间点的差值，没法单点采样：类内记住上一次计数，
// 第一次 sample() 只建立基准（hasBaseline()=false，CPU 先画「--"），之后给出区间使用率。
class Sampler {
public:
    Snapshot sample();
    bool hasBaseline() const { return baseline_; }

private:
    bool baseline_ = false;
#ifdef Q_OS_WIN
    quint64 prevIdle_ = 0;
    quint64 prevKernel_ = 0;    // GetSystemTimes 的 kernel 时间含 idle
    quint64 prevUser_ = 0;
#endif
};

CpuTopology cpuTopology();

// 系统运行时长（毫秒）。Windows 走 GetTickCount64；其他平台返回 -1，界面上画「--」。
qint64 uptimeMs();

} // namespace devdatapod::sys
