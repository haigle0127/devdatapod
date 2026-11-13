#include "ui/widgets/ConfigDialog.h"

#include "core/net/PortAllocator.h"
#include "core/service/Engine.h"
#include "core/service/MysqlDriver.h"
#include "core/service/NginxDriver.h"
#include "core/service/RedisDriver.h"
#include "core/service/ServiceKind.h"
#include "core/util/AppPaths.h"
#include "core/util/SettingsStore.h"
#include "ui/theme/Theme.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QThread>
#include <QVBoxLayout>

namespace devdatapod {

namespace {
// 输入框统一高度，和按钮对齐。
constexpr int kFieldHeight = 30;

QLineEdit* makeLineEdit(QWidget* parent) {
    auto* edit = new QLineEdit(parent);
    edit->setFixedHeight(kFieldHeight);
    // 弹窗整体加宽后输入框跟着撑开，没有这行时下限太窄。
    edit->setMinimumWidth(240);
    return edit;
}

QSpinBox* makeSpinBox(QWidget* parent, int min, int max, int step = 1) {
    auto* spin = new QSpinBox(parent);
    spin->setRange(min, max);
    spin->setSingleStep(step);
    spin->setFixedHeight(kFieldHeight);
    return spin;
}

QLabel* makeFieldLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setStyleSheet(QStringLiteral("font-size: 12px; color: %1;")
                             .arg(Theme::instance().color(QStringLiteral("ink-2")).name()));
    return label;
}
} // namespace

ConfigDialog::ConfigDialog(Engine* engine, const QString& instanceId, QWidget* parent)
    : QDialog(parent), engine_(engine), instanceId_(instanceId) {
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setModal(true);

    if (engine_) {
        engine_->find(instanceId_, &instance_);
        currentPort_ = engine_->portOf(instanceId_);
        isDefault_ = instance_.isDefault;
    }

    buildUi();
    loadCurrentValues();
    // 卡片内容自适应宽度太窄（端口行决定了下限），给整个弹窗一个下限，
    // 行内的输入框会跟着撑开。
    setMinimumWidth(480);
}

void ConfigDialog::buildUi() {
    Theme& theme = Theme::instance();

    auto* card = new QWidget(this);
    card->setObjectName(QStringLiteral("ConfirmCard"));

    auto* shadow = new QGraphicsDropShadowEffect(card);
    shadow->setBlurRadius(28);
    shadow->setOffset(0, 6);
    shadow->setColor(QColor(38, 32, 64, 60));
    card->setGraphicsEffect(shadow);

    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(24, 22, 24, 20);
    cardLayout->setSpacing(14);

    // 标题
    auto* titleLabel = new QLabel(QStringLiteral("%1 配置").arg(instance_.displayName()), card);
    titleLabel->setStyleSheet(QStringLiteral(
        "font-size: 16px; font-weight: 700; color: %1;")
        .arg(theme.color(QStringLiteral("ink")).name()));
    cardLayout->addWidget(titleLabel);

    // 端口区
    buildPortSection(cardLayout);

    // 分隔线
    auto* divider = new QFrame(card);
    divider->setObjectName(QStringLiteral("Divider"));
    divider->setFixedHeight(1);
    divider->setStyleSheet(QStringLiteral("background: %1; border: none;")
                               .arg(theme.color(QStringLiteral("line")).name()));
    cardLayout->addWidget(divider);

    // 组件特有字段
    if (instance_.kind == QLatin1String("redis")) {
        buildRedisFields(cardLayout);
    } else if (instance_.kind == QLatin1String("mysql")) {
        buildMysqlFields(cardLayout);
    } else if (instance_.kind == QLatin1String("nginx")) {
        buildNginxFields(cardLayout);
    }

    // 错误提示（初始隐藏，确定失败时显示在按钮上方）
    errorLabel_ = new QLabel(card);
    errorLabel_->setWordWrap(true);
    errorLabel_->setVisible(false);
    errorLabel_->setStyleSheet(QStringLiteral(
        "font-size: 12px; color: %1; padding: 6px 8px;"
        "background: rgba(222, 91, 76, 0.08); border-radius: 4px;")
        .arg(theme.color(QStringLiteral("danger")).name()));
    cardLayout->addWidget(errorLabel_);

    // 按钮
    cancelBtn_ = new QPushButton(QStringLiteral("取消"), card);
    cancelBtn_->setObjectName(QStringLiteral("SoftBtn"));
    cancelBtn_->setCursor(Qt::PointingHandCursor);

    confirmBtn_ = new QPushButton(QStringLiteral("确定"), card);
    confirmBtn_->setObjectName(QStringLiteral("PrimaryBtn"));
    confirmBtn_->setCursor(Qt::PointingHandCursor);
    confirmBtn_->setDefault(true);

    auto* btnLayout = new QHBoxLayout;
    btnLayout->addStretch();
    btnLayout->addWidget(cancelBtn_);
    btnLayout->addWidget(confirmBtn_);
    cardLayout->addLayout(btnLayout);

    // 外层留边距给阴影
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 20);
    root->addWidget(card);

    // 卡片样式（和 ConfirmDialog 一致）
    card->setStyleSheet(QStringLiteral(
        "#ConfirmCard { background: %1; border-radius: 12px; }")
        .arg(theme.color(QStringLiteral("card")).name()));

    connect(confirmBtn_, &QPushButton::clicked, this, [this] {
        QString error;
        if (applyChanges(&error)) {
            accept();
        } else {
            lastError_ = error;
            errorLabel_->setText(error);
            errorLabel_->setVisible(true);
            adjustSize();
        }
    });
    connect(cancelBtn_, &QPushButton::clicked, this, &QDialog::reject);
}

void ConfigDialog::buildPortSection(QVBoxLayout* layout) {
    auto* row = new QWidget(this);
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(10);

    auto* portLabel = makeFieldLabel(QStringLiteral("端口"), row);
    portLabel->setFixedWidth(60);

    // 下限必须是 1：nginx 的规范端口是 80/443，写死 1024 会把当前端口夹到 1024，
    // 弹窗一打开就显示成错的端口。
    portSpin_ = makeSpinBox(row, 1, 65535);
    // 没有分配时按该 kind 的规范端口起步，而不是一律 6379。
    int fallback = 6379;
    if (const kinds::Meta* meta = kinds::find(instance_.kind)) {
        if (!meta->canonical.isEmpty() && engine_ && engine_->ports()) {
            const int canonical = engine_->ports()->canonicalPort(meta->canonical);
            if (canonical > 0) fallback = canonical;
        }
    }
    portSpin_->setValue(currentPort_ > 0 ? currentPort_ : fallback);

    // 默认实例的星星标记
    starLabel_ = new QLabel(row);
    if (isDefault_) {
        starLabel_->setText(QStringLiteral("★ 默认"));
        starLabel_->setStyleSheet(QStringLiteral(
            "font-size: 12px; color: %1; font-weight: 500;")
            .arg(Theme::instance().color(QStringLiteral("accent")).name()));
    } else {
        starLabel_->setText(QStringLiteral("备用"));
        starLabel_->setStyleSheet(QStringLiteral(
            "font-size: 12px; color: %1;")
            .arg(Theme::instance().color(QStringLiteral("ink-2")).name()));
    }

    rowLayout->addWidget(portLabel);
    rowLayout->addWidget(portSpin_);
    rowLayout->addWidget(starLabel_);
    rowLayout->addStretch();

    layout->addWidget(row);

    // 端口改动时的联动提示
    connect(portSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int value) {
        if (!engine_) return;
        // 检查新端口是否等于 canonical
        const kinds::Meta* meta = kinds::find(instance_.kind);
        if (!meta) return;
        const int canonical = engine_->ports()->canonicalPort(meta->canonical);
        if (value == canonical && !isDefault_) {
            starLabel_->setText(QStringLiteral("★ 将设为默认端口"));
            starLabel_->setStyleSheet(QStringLiteral(
                "font-size: 12px; color: %1; font-weight: 500;")
                .arg(Theme::instance().color(QStringLiteral("accent")).name()));
        } else if (value != canonical && isDefault_) {
            starLabel_->setText(QStringLiteral("将失去默认标记"));
            starLabel_->setStyleSheet(QStringLiteral(
                "font-size: 12px; color: %1;")
                .arg(Theme::instance().color(QStringLiteral("warning")).name()));
        } else {
            starLabel_->setText(isDefault_ ? QStringLiteral("★ 默认") : QStringLiteral("备用"));
            starLabel_->setStyleSheet(QStringLiteral(
                "font-size: 12px; color: %1;%2")
                .arg(Theme::instance().color(isDefault_ ? QStringLiteral("accent")
                                                        : QStringLiteral("ink-2")).name())
                .arg(isDefault_ ? QStringLiteral(" font-weight: 500;") : QString()));
        }
    });
}

void ConfigDialog::buildRedisFields(QVBoxLayout* layout) {
    const QString dataDir = paths::installDir() + QLatin1Char('/') + instance_.dataDir;
    RedisSettings s = RedisSettings::load(dataDir);

    // 密码
    auto* pwdRow = new QWidget(this);
    auto* pwdLayout = new QHBoxLayout(pwdRow);
    pwdLayout->setContentsMargins(0, 0, 0, 0);
    pwdLayout->setSpacing(10);
    auto* pwdLabel = makeFieldLabel(QStringLiteral("密码"), pwdRow);
    pwdLabel->setFixedWidth(60);
    redisPassword_ = makeLineEdit(pwdRow);
    redisPassword_->setPlaceholderText(QStringLiteral("留空 = 无密码"));
    redisPassword_->setText(s.password);
    pwdLayout->addWidget(pwdLabel);
    pwdLayout->addWidget(redisPassword_, 1);
    layout->addWidget(pwdRow);

    // 最大内存
    auto* memRow = new QWidget(this);
    auto* memLayout = new QHBoxLayout(memRow);
    memLayout->setContentsMargins(0, 0, 0, 0);
    memLayout->setSpacing(10);
    auto* memLabel = makeFieldLabel(QStringLiteral("最大内存"), memRow);
    memLabel->setFixedWidth(60);
    redisMaxMemory_ = makeSpinBox(memRow, 16, 65536, 16);
    redisMaxMemory_->setSuffix(QStringLiteral(" MB"));
    redisMaxMemory_->setValue(s.maxMemoryMb);
    memLayout->addWidget(memLabel);
    memLayout->addWidget(redisMaxMemory_);
    memLayout->addStretch();
    layout->addWidget(memRow);

    // 最大连接数
    auto* cliRow = new QWidget(this);
    auto* cliLayout = new QHBoxLayout(cliRow);
    cliLayout->setContentsMargins(0, 0, 0, 0);
    cliLayout->setSpacing(10);
    auto* cliLabel = makeFieldLabel(QStringLiteral("最大连接"), cliRow);
    cliLabel->setFixedWidth(60);
    redisMaxClients_ = makeSpinBox(cliRow, 1, 100000, 100);
    redisMaxClients_->setValue(s.maxClients);
    cliLayout->addWidget(cliLabel);
    cliLayout->addWidget(redisMaxClients_);
    cliLayout->addStretch();
    layout->addWidget(cliRow);

    // 数据库数量
    auto* dbRow = new QWidget(this);
    auto* dbLayout = new QHBoxLayout(dbRow);
    dbLayout->setContentsMargins(0, 0, 0, 0);
    dbLayout->setSpacing(10);
    auto* dbLabel = makeFieldLabel(QStringLiteral("数据库数"), dbRow);
    dbLabel->setFixedWidth(60);
    redisDatabases_ = makeSpinBox(dbRow, 1, 256);
    redisDatabases_->setValue(s.databases);
    dbLayout->addWidget(dbLabel);
    dbLayout->addWidget(redisDatabases_);
    dbLayout->addStretch();
    layout->addWidget(dbRow);
}

void ConfigDialog::buildMysqlFields(QVBoxLayout* layout) {
    const QString dataDir = paths::installDir() + QLatin1Char('/') + instance_.dataDir;
    MysqlSettings s = MysqlSettings::load(dataDir);

    // root 密码
    auto* pwdRow = new QWidget(this);
    auto* pwdLayout = new QHBoxLayout(pwdRow);
    pwdLayout->setContentsMargins(0, 0, 0, 0);
    pwdLayout->setSpacing(10);
    auto* pwdLabel = makeFieldLabel(QStringLiteral("root 密码"), pwdRow);
    pwdLabel->setFixedWidth(60);
    mysqlRootPassword_ = makeLineEdit(pwdRow);
    mysqlRootPassword_->setText(s.rootPassword);
    pwdLayout->addWidget(pwdLabel);
    pwdLayout->addWidget(mysqlRootPassword_, 1);
    layout->addWidget(pwdRow);

    // 最大连接数
    auto* connRow = new QWidget(this);
    auto* connLayout = new QHBoxLayout(connRow);
    connLayout->setContentsMargins(0, 0, 0, 0);
    connLayout->setSpacing(10);
    auto* connLabel = makeFieldLabel(QStringLiteral("最大连接"), connRow);
    connLabel->setFixedWidth(60);
    mysqlMaxConnections_ = makeSpinBox(connRow, 1, 10000, 10);
    mysqlMaxConnections_->setValue(s.maxConnections);
    connLayout->addWidget(connLabel);
    connLayout->addWidget(mysqlMaxConnections_);
    connLayout->addStretch();
    layout->addWidget(connRow);

    // InnoDB 缓冲池
    auto* poolRow = new QWidget(this);
    auto* poolLayout = new QHBoxLayout(poolRow);
    poolLayout->setContentsMargins(0, 0, 0, 0);
    poolLayout->setSpacing(10);
    auto* poolLabel = makeFieldLabel(QStringLiteral("缓冲池"), poolRow);
    poolLabel->setFixedWidth(60);
    mysqlInnodbPool_ = makeSpinBox(poolRow, 8, 65536, 8);
    mysqlInnodbPool_->setSuffix(QStringLiteral(" MB"));
    mysqlInnodbPool_->setValue(s.innodbBufferPoolMb);
    poolLayout->addWidget(poolLabel);
    poolLayout->addWidget(mysqlInnodbPool_);
    poolLayout->addStretch();
    layout->addWidget(poolRow);

    // 字符集
    auto* charsetRow = new QWidget(this);
    auto* charsetLayout = new QHBoxLayout(charsetRow);
    charsetLayout->setContentsMargins(0, 0, 0, 0);
    charsetLayout->setSpacing(10);
    auto* charsetLabel = makeFieldLabel(QStringLiteral("字符集"), charsetRow);
    charsetLabel->setFixedWidth(60);
    mysqlCharset_ = makeLineEdit(charsetRow);
    mysqlCharset_->setText(s.charset);
    charsetLayout->addWidget(charsetLabel);
    charsetLayout->addWidget(mysqlCharset_);
    charsetLayout->addStretch();
    layout->addWidget(charsetRow);
}

void ConfigDialog::buildNginxFields(QVBoxLayout* layout) {
    const QString dataDir = paths::installDir() + QLatin1Char('/') + instance_.dataDir;
    NginxSettings s = NginxSettings::load(dataDir);

    // 工作进程
    auto* workerRow = new QWidget(this);
    auto* workerLayout = new QHBoxLayout(workerRow);
    workerLayout->setContentsMargins(0, 0, 0, 0);
    workerLayout->setSpacing(10);
    auto* workerLabel = makeFieldLabel(QStringLiteral("工作进程"), workerRow);
    workerLabel->setFixedWidth(60);
    nginxWorkers_ = makeSpinBox(workerRow, 1, 32);
    nginxWorkers_->setValue(s.workerProcesses);
    workerLayout->addWidget(workerLabel);
    workerLayout->addWidget(nginxWorkers_);
    workerLayout->addStretch();
    layout->addWidget(workerRow);

    // 每个进程的最大连接数
    auto* connRow = new QWidget(this);
    auto* connLayout = new QHBoxLayout(connRow);
    connLayout->setContentsMargins(0, 0, 0, 0);
    connLayout->setSpacing(10);
    auto* connLabel = makeFieldLabel(QStringLiteral("最大连接"), connRow);
    connLabel->setFixedWidth(60);
    nginxConnections_ = makeSpinBox(connRow, 64, 65536, 64);
    nginxConnections_->setValue(s.workerConnections);
    connLayout->addWidget(connLabel);
    connLayout->addWidget(nginxConnections_);
    connLayout->addStretch();
    layout->addWidget(connRow);

    // 站点目录：相对安装根目录（默认 WWW），也可以填绝对路径
    auto* rootRow = new QWidget(this);
    auto* rootLayout = new QHBoxLayout(rootRow);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(10);
    auto* rootLabel = makeFieldLabel(QStringLiteral("站点目录"), rootRow);
    rootLabel->setFixedWidth(60);
    nginxRootDir_ = makeLineEdit(rootRow);
    nginxRootDir_->setText(s.rootDir);
    nginxRootDir_->setPlaceholderText(QStringLiteral("WWW（相对安装目录，或填绝对路径）"));
    rootLayout->addWidget(rootLabel);
    rootLayout->addWidget(nginxRootDir_, 1);
    layout->addWidget(rootRow);
}

void ConfigDialog::loadCurrentValues() {
    // 值已在 buildXxxFields 里加载，这里只做端口初始值
    if (portSpin_ && currentPort_ > 0) {
        portSpin_->setValue(currentPort_);
    }
}

bool ConfigDialog::validateInputs(QString* error) {
    // 端口范围由 QSpinBox 保证，不需要额外校验
    // MySQL root 密码不能为空
    if (instance_.kind == QLatin1String("mysql") && mysqlRootPassword_) {
        if (mysqlRootPassword_->text().isEmpty()) {
            if (error) *error = QStringLiteral("root 密码不能为空");
            return false;
        }
    }
    if (instance_.kind == QLatin1String("nginx") && nginxRootDir_) {
        if (nginxRootDir_->text().trimmed().isEmpty()) {
            if (error) *error = QStringLiteral("站点目录不能为空");
            return false;
        }
    }
    return true;
}

bool ConfigDialog::applyChanges(QString* error) {
    if (!engine_) {
        if (error) *error = QStringLiteral("引擎不可用");
        return false;
    }
    if (!validateInputs(error)) return false;

    const QString dataDir = paths::installDir() + QLatin1Char('/') + instance_.dataDir;
    // 服务从没启动过时 datadir 还不存在（目录是 start 时才建的），先把目录建出来
    // 再写设置，否则写盘报「系统找不到指定的路径」。
    QDir().mkpath(dataDir);

    // 1. 保存组件设置
    if (instance_.kind == QLatin1String("redis") && redisPassword_) {
        RedisSettings s = RedisSettings::load(dataDir);
        s.password = redisPassword_->text();
        s.maxMemoryMb = redisMaxMemory_->value();
        s.maxClients = redisMaxClients_->value();
        s.databases = redisDatabases_->value();
        if (!s.save(dataDir)) {
            if (error) *error = QStringLiteral("保存 Redis 设置失败");
            return false;
        }
    } else if (instance_.kind == QLatin1String("mysql") && mysqlRootPassword_) {
        MysqlSettings s = MysqlSettings::load(dataDir);
        s.rootPassword = mysqlRootPassword_->text();
        s.maxConnections = mysqlMaxConnections_->value();
        s.innodbBufferPoolMb = mysqlInnodbPool_->value();
        s.charset = mysqlCharset_->text();
        if (!s.save(dataDir)) {
            if (error) *error = QStringLiteral("保存 MySQL 设置失败");
            return false;
        }
    } else if (instance_.kind == QLatin1String("nginx") && nginxRootDir_) {
        NginxSettings s = NginxSettings::load(dataDir);
        s.workerProcesses = nginxWorkers_->value();
        s.workerConnections = nginxConnections_->value();
        s.rootDir = nginxRootDir_->text().trimmed();
        if (!s.save(dataDir)) {
            if (error) *error = QStringLiteral("保存 Nginx 设置失败");
            return false;
        }
    }

    // 2. 应用端口变更
    const int newPort = portSpin_->value();
    const bool portChanged = newPort != currentPort_;
    if (portChanged) {
        if (!engine_->ports()->setPort(instance_.portKey, newPort)) {
            if (error) *error = QStringLiteral("端口 %1 不可用或已被占用").arg(newPort);
            return false;
        }
        // 端口变了，驱动必须重建（invalidate 要求先停）
        if (engine_->isRunning(instanceId_)) {
            QString stopError;
            if (!engine_->stop(instanceId_, &stopError)) {
                if (error) *error = QStringLiteral("停止服务失败：%1").arg(stopError);
                return false;
            }
        }
        engine_->invalidate(instanceId_);
    }

    // 3. 重启（如果服务之前在运行，或端口变了）
    const bool autoRestart = SettingsStore::instance()
                                 .value(QStringLiteral("ui/autoRestartOnConfig"), true)
                                 .toBool();
    const bool shouldRestart = autoRestart && (portChanged || engine_->isRunning(instanceId_));

    if (shouldRestart) {
        QString startError;
        if (!engine_->start(instanceId_, &startError)) {
            if (error) *error = QStringLiteral("重启失败：%1").arg(startError);
            return false;
        }
        // 等它就绪（和 DefaultSwitcher::waitReady 一样的思路）
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < 15000) {
            const ServiceStatus st = engine_->statusOf(instanceId_);
            if (st.state == RunState::Running) break;
            if (st.state == RunState::Crashed || st.state == RunState::Stopped) {
                if (error) *error = QStringLiteral("服务启动后未能正常运行");
                return false;
            }
            QCoreApplication::processEvents();
            QThread::msleep(100);
        }
    }

    return true;
}

} // namespace devdatapod
