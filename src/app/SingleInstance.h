#pragma once

#include <QLocalServer>
#include <QLockFile>
#include <QObject>

namespace devdatapod {

// One panel per install directory. The lock guards ownership; the local server lets a
// second launch ask the running one to raise its window instead of opening a copy.
class SingleInstance : public QObject {
    Q_OBJECT
public:
    explicit SingleInstance(QObject* parent = nullptr);
    ~SingleInstance() override;

    // True when this process owns the install directory. When false, the existing
    // instance has been pinged and the caller should exit.
    bool acquire();
    bool isOwner() const { return acquired_; }

signals:
    void activateRequested();

private:
    QLockFile lock_;
    QLocalServer server_;
    QString serverName_;
    bool acquired_ = false;
};

}
