#include "app/SingleInstance.h"
#include "app/TrayController.h"
#include "core/net/PortAllocator.h"
#include "core/process/OrphanSweep.h"
#include "core/service/ComponentCatalog.h"
#include "core/service/DefaultSwitcher.h"
#include "core/service/Engine.h"
#include "core/service/InstanceRegistry.h"
#include "core/util/AppPaths.h"
#include "core/util/Log.h"
#include "ui/MainWindow.h"
#include "ui/theme/Theme.h"

#include <QApplication>
#include <QDir>
#include <QIcon>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("devdatapod"));
    QApplication::setApplicationVersion(QString::fromLatin1(DEVDATAPOD_VERSION));
    QApplication::setOrganizationName(QStringLiteral("devdatapod"));
    // Closing the window hides it to the tray, so the app must not quit by itself.
    app.setQuitOnLastWindowClosed(false);

    if (!devdatapod::paths::ensureWritableDirs()) {
        qWarning("安装目录不可写，日志与设置无法持久化：%s",
                 qUtf8Printable(devdatapod::paths::installDir()));
    }
    devdatapod::log::init();
    devdatapod::log::installQtMessageHandler();
    // 软件管理页把 Component 排队送进安装线程，得先注册元类型。
    qRegisterMetaType<devdatapod::Component>("devdatapod::Component");
    devdatapod::log::info(QStringLiteral("devdatapod %1 启动，安装目录 %2")
                          .arg(QString::fromLatin1(DEVDATAPOD_VERSION), devdatapod::paths::installDir()));

    if (!devdatapod::Theme::instance().isLoaded()) {
        devdatapod::log::error(QStringLiteral("主题 token 加载失败，界面无法按设计稿渲染"));
        return 2;
    }
    devdatapod::Theme::instance().applyToApplication();

    devdatapod::SingleInstance instance;
    if (!instance.acquire()) {
        devdatapod::log::info(QStringLiteral("该安装目录已有面板在运行，已请求它显示窗口"));
        return 0;
    }

    // AGENTS.md rule 5: only after the lock. Sweeping while a second copy is running would
    // kill the services the first copy owns, and "second copy" is exactly the case where
    // another panel is already holding those job objects.
    {
        QStringList swept;
        const int killed = devdatapod::proc::sweepInstallRoot(devdatapod::paths::installDir(), &swept);
        for (const QString& line : swept) devdatapod::log::warn(line);
        if (killed > 0) {
            devdatapod::log::warn(QStringLiteral("清理了 %1 个上次崩溃遗留的孤儿进程").arg(killed));
        }
    }

    devdatapod::PortAllocator ports(QDir(devdatapod::paths::dataDir()).filePath(QStringLiteral("ports.json")));
    if (!ports.load()) {
        devdatapod::log::error(QStringLiteral("ports.json 不可用，面板将以只读模式运行：%1")
                               .arg(ports.problem()));
    } else if (!ports.isHealthy()) {
        devdatapod::log::error(QStringLiteral("ports.json 存在冲突，写入已全部拒绝：%1")
                               .arg(ports.problem()));
    }

    devdatapod::InstanceRegistry registry(
        QDir(devdatapod::paths::dataDir()).filePath(QStringLiteral("services.json")));
    if (!registry.load()) {
        devdatapod::log::error(QStringLiteral("services.json 不可用，面板将以只读模式运行：%1")
                               .arg(registry.problem()));
    } else if (!registry.isHealthy()) {
        devdatapod::log::error(QStringLiteral("services.json 存在冲突，写入已全部拒绝：%1")
                               .arg(registry.problem()));
    }

    devdatapod::Engine engine(&ports, &registry);
    // A failed reconcile is not fatal: the engine then reports !isReady() and the window
    // shows the reason instead of half-working, so nothing starts on a port nobody owns.
    QString syncError;
    if (!engine.synchronize(&syncError)) {
        devdatapod::log::error(QStringLiteral("服务模型对账失败：%1").arg(syncError));
    }

    devdatapod::DefaultSwitcher switcher(&engine);

    devdatapod::MainWindow window(&engine, &switcher);
    devdatapod::TrayController tray(&window);
    tray.install();
    window.setTrayController(&tray);
    QObject::connect(&tray, &devdatapod::TrayController::quitRequested, &window,
                     &devdatapod::MainWindow::requestQuit);
    QObject::connect(&instance, &devdatapod::SingleInstance::activateRequested, &window,
                     &devdatapod::MainWindow::presentFromSecondInstance);

    // Polite quit path. The job objects are the crash backstop; this is what runs when the
    // user presses 退出面板, and it must happen before the supervisors die with main().
    QObject::connect(&app, &QCoreApplication::aboutToQuit, [&engine] { engine.stopAll(); });

    // 窗口图标：exe 图标走 RC 文件嵌入，窗口标题栏图标从 qrc 加载。
    window.setWindowIcon(QIcon(QStringLiteral(":/devdatapod/assets/logo.svg")));
    window.show();
    const int code = app.exec();
    devdatapod::log::shutdown();
    return code;
}
