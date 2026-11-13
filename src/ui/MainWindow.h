#pragma once

#include "core/process/SystemStat.h"
#include "ui/widgets/ServiceRow.h"
#include "core/service/ComponentCatalog.h"

#include <QRect>
#include <QSet>
#include <QString>
#include <QVector>
#include <QWidget>

class QCloseEvent;
class QTimer;
class QResizeEvent;
class QSizeGrip;
class QStackedWidget;
class QThread;

namespace devdatapod {

class DefaultSwitcher;
class Engine;
class HomePage;
class ServiceOperator;
class InstallOperator;
class Sidebar;
class SitePage;
class StatusBarPanel;
class TitleBar;
class TrayController;
class SoftwarePage;
class SettingsPage;

// Frameless shell: 46px custom title bar over a 216px rail, a page stack and the status
// bar. Maximize is manual (availableGeometry, not Qt::WindowMaximized) so the taskbar
// stays visible, which also keeps the normal/maximized geometry bookkeeping symmetric.
//
// Owns nothing from core: the engine and the switcher are built in main.cpp so the crash
// path (job objects) and the quit path (stopAll) stay in one place.
class MainWindow : public QWidget {
    Q_OBJECT
public:
    explicit MainWindow(Engine* engine, DefaultSwitcher* switcher, QWidget* parent = nullptr);

    void setTrayController(TrayController* tray);
    void presentFromSecondInstance();

public slots:
    void requestQuit();

protected:
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    void buildPages();
    void toggleMaximized();
    void loadWindowGeometry();
    void saveWindowGeometry();

    // Poll the engine: rows() is the one call that reads state, port and role together.
    void refresh();
    QVector<ServiceRowInfo> collectRows();
    void handlePrimary(const QString& id);
    void handleRestart(const QString& id);
    void handlePromote(const QString& id);
    void handleInstall(const devdatapod::Component& component);
    void handleUninstall(const devdatapod::Component& component);
    void handleConfigure(const QString& id);
    void handleCreateSite();
    void handleManageSite(const QString& domain);
    // 首页「运行状态」四仪表的采样入口，由 statsPoll_ 每 3 分钟驱动一次。
    void updateSystemStats();
    // 站点变更后：刷新网站页；若 Web 服务在跑，把新配置热重载进去（不断开连接）。
    void applyWebConfig();
    bool webServiceUp() const;
    // 引擎没就绪、或服务启动失败且占位者是另一个 devdatapod 安装时，征得同意后
    // 接管端口；retryId 非空时接管成功后自动重试该服务。
    void offerSiblingTakeover(const QString& detail, const QString& retryId = QString());
    void showProblem(const QString& detail);
    void onServiceOpFinished(const QString& id, bool ok, const QString& error);

    Engine* engine_;
    DefaultSwitcher* switcher_;
    QTimer* poll_ = nullptr;
    // 系统统计（CPU / 内存 / 磁盘）独立定时器：用户要求 3 分钟一轮，比服务状态轮询慢得多。
    QTimer* statsPoll_ = nullptr;
    sys::Sampler systemSampler_;
    HomePage* home_ = nullptr;
    SitePage* site_ = nullptr;
    SoftwarePage* software_ = nullptr;
    // 上次轮询时 Web 服务是否在跑，驱动网站页的状态列按需重建。
    bool lastWebUp_ = false;
    // 已经就「崩溃 + 疑似兄弟安装占位」弹过接管提示的实例，Running/Stopped 后复位。
    QSet<QString> crashNotified_;
    // Stops the status bar from rebuilding its chips twice a second while nothing moved.
    QString signature_;

    // 启停在工作线程跑，UI 不卡。busy_ 记录正在启停的实例，期间该行按钮置灰。
    QThread* opThread_ = nullptr;
    ServiceOperator* op_ = nullptr;
    QSet<QString> busy_;

    // 下载/解压/注册也在自己的线程，装大组件不卡界面。
    QThread* installThread_ = nullptr;
    InstallOperator* installer_ = nullptr;

    TitleBar* titleBar_ = nullptr;
    Sidebar* sidebar_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    StatusBarPanel* statusBar_ = nullptr;
    QSizeGrip* resizeGrip_ = nullptr;
    TrayController* tray_ = nullptr;

    QRect normalBounds_;
    bool maximized_ = false;
    bool quitting_ = false;
};

}
