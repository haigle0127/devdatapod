#pragma once

#include <QObject>
#include <QString>

namespace devdatapod {

class Engine;

// 把 Engine 的启停调用挪到专用工作线程，避免 MySQL 初始化 / 设密码这类耗时操作
// 卡住 UI 主线程（点「启动」界面冻住几秒就是这么来的）。
//
// 所有 Engine::start/stop/restart 都在这一个线程里串行执行，所以不用加锁：
// Engine 本身不是线程安全的，但只要不并发进它就没事。请求通过信号发进来，
// 结果通过 finished 信号回主线程（Qt 自动走队列连接，跨线程安全）。
class ServiceOperator : public QObject {
    Q_OBJECT
public:
    explicit ServiceOperator(Engine* engine, QObject* parent = nullptr);

public slots:
    void start(const QString& id);
    void stop(const QString& id);
    void restart(const QString& id);

signals:
    // ok=false 时 error 带原因。id 让主线程知道是哪一行完事了。
    void finished(const QString& id, bool ok, const QString& error);

private:
    Engine* engine_;    // 不拥有：归 main.cpp 管
};

}
