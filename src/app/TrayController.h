#pragma once

#include <QObject>

class QSystemTrayIcon;
class QWidget;

namespace devdatapod {

// Tray presence: the panel can live there with the window hidden, and a crash-free exit
// path that does not take the managed services with it (that rule lands in P2).
class TrayController : public QObject {
    Q_OBJECT
public:
    explicit TrayController(QWidget* window, QObject* parent = nullptr);

    // No-op when the platform has no tray; the window then simply stays normal.
    void install();
    bool isAvailable() const { return tray_ != nullptr; }

signals:
    void quitRequested();

private:
    void setVisibleToUser(bool visible);

    QWidget* window_ = nullptr;
    QSystemTrayIcon* tray_ = nullptr;
};
}
