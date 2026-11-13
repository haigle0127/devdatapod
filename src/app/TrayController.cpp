#include "app/TrayController.h"

#include "core/util/Log.h"
#include "ui/theme/Theme.h"

#include <QAction>
#include <QMenu>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QWidget>

namespace devdatapod {

TrayController::TrayController(QWidget* window, QObject* parent)
    : QObject(parent), window_(window) {}

void TrayController::install() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        log::warn(QStringLiteral("系统托盘不可用，已跳过托盘常驻"));
        return;
    }

    tray_ = new QSystemTrayIcon(this);
    tray_->setIcon(Theme::instance().appIcon());
    tray_->setToolTip(QStringLiteral("devdatapod"));

    auto* menu = new QMenu(nullptr);
    QAction* toggle = menu->addAction(QStringLiteral("显示 / 隐藏主界面"));
    QAction* quit = menu->addAction(QStringLiteral("退出 devdatapod"));
    tray_->setContextMenu(menu);

    auto toggleVisibility = [this] {
        // 判断窗口是否"正在显示"：可见且未最小化。失焦或被遮挡都算显示，此时点击托盘应隐藏。
        const bool showing = window_ && window_->isVisible() && !window_->isMinimized();
        setVisibleToUser(!showing);
    };

    // 菜单项延迟到菜单关闭后再执行，避免 hide 与菜单销毁互相干扰。
    connect(toggle, &QAction::triggered, this, [toggleVisibility] {
        QTimer::singleShot(0, toggleVisibility);
    });
    connect(quit, &QAction::triggered, this, &TrayController::quitRequested);
    connect(tray_, &QSystemTrayIcon::activated, this,
            [toggleVisibility](QSystemTrayIcon::ActivationReason reason) {
                if (reason != QSystemTrayIcon::Trigger &&
                    reason != QSystemTrayIcon::DoubleClick) {
                    return;
                }
                toggleVisibility();
            });

    tray_->show();
}

void TrayController::setVisibleToUser(bool visible) {
    if (!window_) return;
    if (!visible) {
        window_->hide();
        return;
    }
    // 最小化的窗口需要 showNormal 才能恢复到正常大小，show 只会保持最小化。
    if (window_->isMinimized()) {
        window_->showNormal();
    } else {
        window_->show();
    }
    window_->raise();
    window_->activateWindow();
}

}
