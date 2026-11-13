#include "ui/MainWindow.h"

#include "app/TrayController.h"
#include "core/service/DefaultSwitcher.h"
#include "core/service/Engine.h"
#include "core/service/SiteStore.h"
#include "core/util/Log.h"
#include "core/util/SettingsStore.h"
#include "ui/ServiceOperator.h"
#include "ui/InstallOperator.h"
#include "ui/theme/Theme.h"
#include "ui/widgets/ConfirmDialog.h"
#include "ui/widgets/ConfigDialog.h"
#include "ui/widgets/Sidebar.h"
#include "ui/widgets/SiteDialog.h"
#include "ui/widgets/StatusBarPanel.h"
#include "ui/widgets/TitleBar.h"
#include "ui/widgets/pages/HomePage.h"
#include "ui/widgets/pages/SettingsPage.h"
#include "ui/widgets/pages/SitePage.h"
#include "ui/widgets/pages/SoftwarePage.h"

#include <QApplication>
#include <QCloseEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QResizeEvent>
#include <QScreen>
#include <QSizeGrip>
#include <QStackedWidget>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <Windows.h>

namespace devdatapod {

namespace {
constexpr int kWidth = 1280;
constexpr int kHeight = 800;
// The canvas is fixed at 1280x800 and the adaptive rule is still an open design gap
// (docs/DESIGN-SPEC.md); this is the smallest layout that keeps the home cards intact.
constexpr int kMinWidth = 1080;
constexpr int kMinHeight = 720;
constexpr int kGripSize = 14;
// Rows come from a poll, not from driver callbacks: a driver proves "running" by speaking
// its protocol, and only status() knows whether the answer arrived since the last check.
constexpr int kPollIntervalMs = 2000;

QVector<ServiceStatusItem> statusItems(const QVector<ServiceRowInfo>& services) {
    QVector<ServiceStatusItem> items;
    items.reserve(services.size());
    for (const ServiceRowInfo& service : services) {
        items.append({service.name, service.state});
    }
    return items;
}
}

MainWindow::MainWindow(Engine* engine, DefaultSwitcher* switcher, QWidget* parent)
    : QWidget(parent), engine_(engine), switcher_(switcher) {
    setObjectName(QStringLiteral("Screen"));
    setAttribute(Qt::WA_StyledBackground, true);
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setWindowTitle(QStringLiteral("devdatapod"));
    setWindowIcon(Theme::instance().appIcon());
    setMinimumSize(kMinWidth, kMinHeight);
    resize(kWidth, kHeight);

    // 无边框窗口没有 WS_MINIMIZEBOX，Windows 不会发 SC_MINIMIZE，任务栏点击无法最小化。
    // 补上这个样式（不影响外观），让任务栏点击能正常触发最小化。
    QTimer::singleShot(0, this, [this] {
        auto hwnd = reinterpret_cast<HWND>(winId());
        LONG style = GetWindowLongW(hwnd, GWL_STYLE);
        SetWindowLongW(hwnd, GWL_STYLE, style | WS_MINIMIZEBOX);
    });

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    titleBar_ = new TitleBar(this);
    root->addWidget(titleBar_);

    auto* body = new QWidget(this);
    auto* bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);

    sidebar_ = new Sidebar(body);
    bodyLayout->addWidget(sidebar_);

    auto* content = new QWidget(body);
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(24, 0, 24, 14);
    contentLayout->setSpacing(12);
    stack_ = new QStackedWidget(content);
    statusBar_ = new StatusBarPanel(QString::fromLatin1(DEVDATAPOD_VERSION), content);
    contentLayout->addWidget(stack_, 1);
    contentLayout->addWidget(statusBar_);
    bodyLayout->addWidget(content, 1);

    root->addWidget(body, 1);

    buildPages();

    connect(titleBar_, &TitleBar::minimizeRequested, this, &QWidget::showMinimized);
    connect(titleBar_, &TitleBar::toggleMaximizeRequested, this, &MainWindow::toggleMaximized);
    connect(titleBar_, &TitleBar::closeRequested, this, &MainWindow::close);
    connect(sidebar_, &Sidebar::pageSelected, stack_, &QStackedWidget::setCurrentIndex);

    if (engine_) {
        connect(engine_, &Engine::instancesChanged, this, &MainWindow::refresh);
        connect(engine_, &Engine::instanceChanged, this, &MainWindow::refresh);

        // 启停放到工作线程：MySQL 初始化 / 设密码要几秒，在 UI 线程跑会把界面卡死。
        opThread_ = new QThread(this);
        op_ = new ServiceOperator(engine_);
        op_->moveToThread(opThread_);
        connect(op_, &ServiceOperator::finished, this, &MainWindow::onServiceOpFinished);
        opThread_->start();

        // 下载/解压/注册也放工作线程。解压几百 MB 的 zip 在 UI 线程会卡住。
        installThread_ = new QThread(this);
        installer_ = new InstallOperator(engine_);
        installer_->moveToThread(installThread_);
        connect(installer_, &InstallOperator::stageChanged, software_,
                &SoftwarePage::setRowStage);
        connect(installer_, &InstallOperator::downloadProgress, software_,
                &SoftwarePage::setRowProgress);
        connect(installer_, &InstallOperator::finished, software_, &SoftwarePage::setRowFinished);
        // 安装/卸载成功后整页重建，刷新按钮状态（已安装→卸载，下载→已安装）。
        connect(installer_, &InstallOperator::operationFinished, software_, &SoftwarePage::reload);
        // 装完后首页套件列表要多一行，靠 Engine::instancesChanged 已经连到 refresh()。
        installThread_->start();
    }
    poll_ = new QTimer(this);
    poll_->setInterval(kPollIntervalMs);
    connect(poll_, &QTimer::timeout, this, &MainWindow::refresh);
    poll_->start();

    // 首页四仪表的采样节奏：3 分钟一轮（用户指定）。启动时采一次建立基准，隔 1.2s
    // 再采第二次——CPU 差值要两个时间点，背靠背采样差值约等于 0，得拉开一点间隔。
    statsPoll_ = new QTimer(this);
    statsPoll_->setInterval(3 * 60 * 1000);
    connect(statsPoll_, &QTimer::timeout, this, &MainWindow::updateSystemStats);
    statsPoll_->start();
    updateSystemStats();
    QTimer::singleShot(1200, this, &MainWindow::updateSystemStats);
    refresh();

    resizeGrip_ = new QSizeGrip(this);
    resizeGrip_->setFixedSize(kGripSize, kGripSize);
    resizeGrip_->raise();

    loadWindowGeometry();

    // Deferred: a modal built here would open before show() and the window behind it would
    // never have painted, so the user would see a dialog over a dead region.
    if (engine_ && !engine_->isReady()) {
        const QString detail = engine_->problem();
        QTimer::singleShot(0, this, [this, detail] { offerSiblingTakeover(detail); });
    }
}

// 引擎没就绪 / 服务起不来，十有八九是规范端口被占。若占位者是「另一个 devdatapod
// 安装」的进程，征得同意后停掉它们、重新分配端口——新旧两个安装和平交接，
// 而不是互相报错。retryId 给了的话，接管成功后自动把那个服务再拉起来。
void MainWindow::offerSiblingTakeover(const QString& detail, const QString& retryId) {
    const QVector<net::PortOccupier> siblings = engine_->siblingPanelProcesses();
    if (siblings.isEmpty()) {
        showProblem(detail);
        return;
    }
    QStringList lines;
    for (const net::PortOccupier& item : siblings) {
        lines.append(QStringLiteral("· %1（端口 %2）").arg(item.processName).arg(item.port));
    }
    if (!ConfirmDialog::ask(this, QStringLiteral("发现另一个 devdatapod 安装"),
                            QStringLiteral("以下进程属于另一个 devdatapod 安装，正占用本面板需要的端口："
                                           "\n\n%1\n\n停止它们并继续？\n\n"
                                           "建议之后卸载旧安装目录，彻底避免冲突。")
                                .arg(lines.join(QLatin1Char('\n'))),
                            QStringLiteral("停止并继续"))) {
        showProblem(detail);
        return;
    }
    engine_->stopSiblingPanels();
    QString error;
    if (!engine_->synchronize(&error)) {
        showProblem(error);
        return;
    }
    log::info(QStringLiteral("ui: 接管完成，端口已重新分配"));
    if (!retryId.isEmpty()) {
        busy_.insert(retryId);
        // TerminateProcess 是硬杀，被占端口不会立刻释放（毫秒级残留），
        // 立刻重起还是绑定失败——等 1.5 秒让它彻底撒手再拉起来。
        QTimer::singleShot(1500, this, [this, retryId] {
            QMetaObject::invokeMethod(op_, "start", Qt::QueuedConnection,
                                      Q_ARG(QString, retryId));
        });
    }
    refresh();
}

void MainWindow::buildPages() {
    home_ = new HomePage(QStringLiteral("永远相信 美好的事情即将发生"), collectRows(),
                         QString::fromLatin1(DEVDATAPOD_VERSION), stack_);
    connect(home_, &HomePage::servicePrimaryRequested, this, &MainWindow::handlePrimary);
    connect(home_, &HomePage::serviceRestartRequested, this, &MainWindow::handleRestart);
    connect(home_, &HomePage::serviceConfigureRequested, this, &MainWindow::handleConfigure);
    connect(home_, &HomePage::servicePromoteRequested, this, &MainWindow::handlePromote);
    stack_->addWidget(home_);

    // 侧栏顺序：首页 / 网站 / 软件管理 / 设置。
    site_ = new SitePage(engine_, stack_);
    connect(site_, &SitePage::createRequested, this, &MainWindow::handleCreateSite);
    connect(site_, &SitePage::manageRequested, this, &MainWindow::handleManageSite);
    stack_->addWidget(site_);

    software_ = new SoftwarePage(engine_, stack_);
    connect(software_, &SoftwarePage::installRequested, this, &MainWindow::handleInstall);
    connect(software_, &SoftwarePage::uninstallRequested, this, &MainWindow::handleUninstall);
    stack_->addWidget(software_);

    stack_->addWidget(new SettingsPage(stack_));
}

QVector<ServiceRowInfo> MainWindow::collectRows() {
    QVector<ServiceRowInfo> out;
    if (!engine_) return out;

    const QVector<Engine::Row> rows = engine_->rows();
    out.reserve(rows.size());
    for (const Engine::Row& row : rows) {
        ServiceRowInfo info;
        info.id = row.instance.id;
        info.name = row.instance.displayName();
        info.role = row.instance.isDefault ? QStringLiteral("default")
                                           : QStringLiteral("secondary");
        info.port = row.port > 0 ? QString::number(row.port) : QString();
        // 刚点下按钮、进程还没起来的这一瞬，Engine 还报 Stopped。busy_ 里的直接显示
        // 「启动中」，让按钮立刻置灰，不用等下一次 poll。
        info.state = busy_.contains(row.instance.id) ? RunState::Starting : row.state;
        info.detail = row.detail;
        info.promotable = engine_->isReady() && !row.instance.isDefault;
        out.append(info);
    }
    return out;
}

void MainWindow::refresh() {
    if (!engine_ || !home_) return;
    // A promote runs its own event loop while it waits for the new default to answer. During
    // that window the allocations are deliberately mid-transition, so repainting the list
    // would show a role and a port that never existed together.
    if (switcher_ && switcher_->isSwitching()) return;

    // 网站页的「正常 / 已停用」列跟着 Web 服务跑：状态翻转时重建一次站点表，
    // 平时 2 秒一次的轮询不动它（重建会把悬停中的按钮拆掉）。
    const bool webUp = webServiceUp();
    if (webUp != lastWebUp_) {
        lastWebUp_ = webUp;
        if (site_) site_->reload();
    }

    const QVector<ServiceRowInfo> services = collectRows();
    home_->setServices(services);
    // 「资源信息」页的实例 / 站点计数跟着轮询走，装一个组件或建一个站点都实时反映。
    home_->setResourceCounts(services.size(), sites::load().size());

    // 服务崩了且检测到另一个 devdatapod 的进程占着端口：主动给接管选项。
    // start() 只保证进程拉起来了，绑定失败是之后 status() 才发现的（Crashed），
    // 所以接管的另一入口在这里，按实例只提醒一次。
    if (engine_) {
        for (const ServiceRowInfo& info : services) {
            if (info.state == RunState::Crashed) {
                if (!crashNotified_.contains(info.id)
                    && !engine_->siblingPanelProcesses().isEmpty()) {
                    crashNotified_.insert(info.id);
                    offerSiblingTakeover(info.detail, info.id);
                    break;    // 一次轮询只弹一个，别叠罗汉
                }
            } else {
                crashNotified_.remove(info.id);
            }
        }
    }

    QString signature;
    for (const ServiceRowInfo& info : services) {
        signature += info.id;
        signature += QLatin1Char(':');
        signature += runStateKey(info.state);
        signature += QLatin1Char(';');
    }
    if (signature != signature_) {
        signature_ = signature;
        statusBar_->setServices(statusItems(services));
    }
}

void MainWindow::handlePrimary(const QString& id) {
    if (!engine_ || !op_ || (switcher_ && switcher_->isSwitching())) return;
    if (busy_.contains(id)) return;    // 这行已经在启停中，别重复发

    busy_.insert(id);
    refresh();    // 立刻显示「启动中」、按钮置灰，不用等 poll

    const bool running = engine_->isRunning(id);
    const char* method = running ? "stop" : "start";
    // 排队到工作线程跑，UI 立即返回。结果走 onServiceOpFinished。
    QMetaObject::invokeMethod(op_, method, Qt::QueuedConnection, Q_ARG(QString, id));
}

void MainWindow::handleRestart(const QString& id) {
    if (!engine_ || !op_ || (switcher_ && switcher_->isSwitching())) return;
    if (busy_.contains(id)) return;

    busy_.insert(id);
    refresh();
    QMetaObject::invokeMethod(op_, "restart", Qt::QueuedConnection, Q_ARG(QString, id));
}

void MainWindow::onServiceOpFinished(const QString& id, bool ok, const QString& error) {
    busy_.remove(id);
    if (!ok) {
        log::error(QStringLiteral("engine: 操作 %1 失败：%2").arg(id, error));
        // 起不来且检测到另一个 devdatapod 的进程占着端口：给接管选项，而不是干报错。
        if (engine_ && !engine_->siblingPanelProcesses().isEmpty()) {
            offerSiblingTakeover(error, id);
            return;
        }
        showProblem(error);
    }
    refresh();
}

void MainWindow::handlePromote(const QString& id) {
    if (!engine_ || !switcher_ || switcher_->isSwitching()) return;

    ServiceInstance instance;
    if (!engine_->find(id, &instance) || instance.isDefault) return;

    if (!ConfirmDialog::ask(this, QStringLiteral("设为默认"),
                            QStringLiteral("将 %1 设为 %2 的默认实例。\n\n"
                                           "默认实例会接管规范端口，当前默认实例改用槽位端口，"
                                           "两者都会先停掉再按新端口启动。")
                                .arg(instance.displayName(), instance.kind),
                            QStringLiteral("设为默认"))) {
        return;
    }

    QString error;
    if (!switcher_->promote(id, &error)) {
        log::error(QStringLiteral("engine: promote %1 failed: %2").arg(id, error));
        showProblem(error);
    }
    refresh();
}

void MainWindow::handleInstall(const Component& component) {
    if (!engine_ || !installer_) return;
    if (!engine_->isReady()) {
        showProblem(engine_->problem());
        return;
    }
    log::info(QStringLiteral("软件管理：开始安装 %1").arg(component.id));
    // 排队到安装线程跑 下载→解压→注册，UI 立即返回，进度走信号回来。
    QMetaObject::invokeMethod(installer_, "install", Qt::QueuedConnection,
                              Q_ARG(devdatapod::Component, component));
}

void MainWindow::handleUninstall(const Component& component) {
    if (!engine_ || !installer_) return;
    if (!engine_->isReady()) {
        showProblem(engine_->problem());
        return;
    }
    log::info(QStringLiteral("软件管理：开始卸载 %1").arg(component.id));
    QMetaObject::invokeMethod(installer_, "uninstall", Qt::QueuedConnection,
                              Q_ARG(devdatapod::Component, component));
}

void MainWindow::handleConfigure(const QString& id) {
    if (!engine_) return;
    if (busy_.contains(id)) return;

    ConfigDialog dialog(engine_, id, this);
    dialog.exec();
    refresh();
}

bool MainWindow::webServiceUp() const {
    if (!engine_) return false;
    const ServiceInstance* web = engine_->defaultOf(QStringLiteral("nginx"));
    return web && engine_->isRunning(web->id);
}

void MainWindow::handleCreateSite() {
    if (!engine_) return;
    // 和小皮一致：Web 服务没跑时直接拒绝创建，先让人把 nginx 打开。
    if (!webServiceUp()) {
        ConfirmDialog::notice(this, QStringLiteral("提示"), QStringLiteral("请先启动 web 服务！"));
        return;
    }
    SiteDialog dialog(engine_, QString(), this);
    if (dialog.exec() == QDialog::Accepted) applyWebConfig();
}

void MainWindow::handleManageSite(const QString& domain) {
    if (!engine_) return;
    SiteDialog dialog(engine_, domain, this);
    if (dialog.exec() == QDialog::Accepted) applyWebConfig();
}

void MainWindow::applyWebConfig() {
    if (site_) site_->reload();
    // nginx 在跑就把新配置热重载进去（不断开连接）；没跑也不拦着用户，下次启动自然带上。
    const ServiceInstance* web = engine_ ? engine_->defaultOf(QStringLiteral("nginx")) : nullptr;
    if (!web || !engine_->isRunning(web->id)) return;
    QString error;
    if (!engine_->reload(web->id, &error)) {
        log::error(QStringLiteral("engine: web 服务热重载失败：%1").arg(error));
        showProblem(QStringLiteral("站点已保存，但 Web 服务没有应用新配置：%1").arg(error));
    }
}

void MainWindow::updateSystemStats() {
    if (!home_) return;
    const sys::Snapshot snapshot = systemSampler_.sample();
    home_->setSystemStats(snapshot, systemSampler_.hasBaseline());
}

void MainWindow::showProblem(const QString& detail) {
    const QString text = detail.isEmpty() ? QStringLiteral("服务引擎当前不可用") : detail;
    log::error(QStringLiteral("ui: %1").arg(text));
    ConfirmDialog::notice(this, QStringLiteral("devdatapod"), text);
}

void MainWindow::setTrayController(TrayController* tray) {
    tray_ = tray;
}

void MainWindow::presentFromSecondInstance() {
    if (isMinimized()) setWindowState(windowState() & ~Qt::WindowMinimized);
    show();
    raise();
    activateWindow();
}

void MainWindow::requestQuit() {
    quitting_ = true;
    close();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    saveWindowGeometry();
    const bool toTray = !quitting_ && tray_ && tray_->isAvailable() &&
                        SettingsStore::instance()
                            .value(QStringLiteral("ui/closeToTray"), true)
                            .toBool();
    if (toTray) {
        event->ignore();
        hide();
        return;
    }
    event->accept();
    // 真正退出前停掉启停线程，别让它在 QApplication 销毁后还在跑。
    if (opThread_) {
        opThread_->quit();
        opThread_->wait(3000);
    }
    if (installThread_) {
        installThread_->quit();
        installThread_->wait(3000);
    }
    QApplication::quit();
}

void MainWindow::toggleMaximized() {
    QScreen* screen = this->screen();
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (!screen) return;

    if (!maximized_) {
        normalBounds_ = geometry();
        setGeometry(screen->availableGeometry());
        maximized_ = true;
    } else {
        if (normalBounds_.isValid()) setGeometry(normalBounds_);
        maximized_ = false;
    }
    titleBar_->setMaximized(maximized_);
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (resizeGrip_) {
        resizeGrip_->move(width() - resizeGrip_->width(), height() - resizeGrip_->height());
    }
}

// 无边框窗口不响应任务栏点击最小化，需要手动拦截 WM_SYSCOMMAND。
bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
    if (eventType == "windows_generic_MSG") {
        auto* msg = static_cast<MSG*>(message);
        if (msg->message == WM_SYSCOMMAND && (msg->wParam & 0xFFF0) == SC_MINIMIZE) {
            showMinimized();
            *result = 0;
            return true;
        }
    }
    return QWidget::nativeEvent(eventType, message, result);
}

void MainWindow::loadWindowGeometry() {
    const QStringList parts = SettingsStore::instance()
                                  .value(QStringLiteral("window/bounds"))
                                  .toString()
                                  .split(QLatin1Char(','));
    if (parts.size() != 4) return;

    const QRect bounds(parts[0].toInt(), parts[1].toInt(), parts[2].toInt(), parts[3].toInt());
    if (!bounds.isValid()) return;
    setGeometry(bounds);
    normalBounds_ = bounds;
}

void MainWindow::saveWindowGeometry() {
    const QRect bounds = maximized_ ? normalBounds_ : geometry();
    if (!bounds.isValid()) return;
    SettingsStore::instance().setValue(
        QStringLiteral("window/bounds"),
        QStringLiteral("%1,%2,%3,%4")
            .arg(bounds.x())
            .arg(bounds.y())
            .arg(bounds.width())
            .arg(bounds.height()));
}

}
