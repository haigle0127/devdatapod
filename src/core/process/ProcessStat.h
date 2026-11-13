#pragma once

#include <QDateTime>
#include <QVector>

namespace devdatapod::proc {

struct ResourceSample {
    bool valid = false;
    double cpuPercent = 0.0;   // 0..100 for the whole machine, summed over the instance
    qint64 rssBytes = 0;
};

// CPU is a rate, so it needs two observations. The sampler keeps the previous reading per
// pid and returns 0% on the first call instead of inventing a number - the home-page rings
// tick every second and the second tick is the first honest one.
//
// Aggregating over Supervisor::members() is what makes "MySQL" on the dial mean this
// instance, including php-fpm/nginx workers forked after startup.
class ResourceSampler {
public:
    ResourceSample sample(const QVector<qint64>& pids);
    void reset() {
        baselineNs_ = -1;
        baselineAt_ = QDateTime();
    }

private:
    static qint64 processTimeNs(qint64 pid, qint64* rssBytes);

    // One baseline for the whole set: the rate is (sum of cpu time) over (wall time since
    // the previous tick). A vanished worker makes the sum jump down, which resets it.
    qint64 baselineNs_ = -1;
    QDateTime baselineAt_;
};

}
