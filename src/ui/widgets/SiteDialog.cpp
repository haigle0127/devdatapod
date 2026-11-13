#include "ui/widgets/SiteDialog.h"

#include "core/net/PortProbe.h"
#include "core/service/Engine.h"
#include "core/service/ServiceInstance.h"
#include "core/service/SiteStore.h"
#include "core/util/AppPaths.h"
#include "core/util/Log.h"
#include "ui/theme/Theme.h"
#include "ui/widgets/ConfirmDialog.h"

#include <QDate>
#include <QDir>
#include <QFileDialog>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QVBoxLayout>

namespace devdatapod {

namespace {
constexpr int kFieldHeight = 30;
constexpr int kLabelWidth = 72;

QLineEdit* makeInput(QWidget* parent) {
    auto* edit = new QLineEdit(parent);
    edit->setObjectName(QStringLiteral("FieldInput"));
    edit->setFixedHeight(kFieldHeight);
    // 弹窗整体加宽后输入框跟着撑开，没有这行时下限太窄。
    edit->setMinimumWidth(260);
    return edit;
}

QLabel* makeRowLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setFixedWidth(kLabelWidth);
    label->setStyleSheet(QStringLiteral(
        "font-size: 12px; color: %1;").arg(Theme::instance().color(QStringLiteral("ink-2")).name()));
    return label;
}
} // namespace

SiteDialog::SiteDialog(Engine* engine, const QString& domain, QWidget* parent)
    : QDialog(parent), engine_(engine), originalDomain_(domain.trimmed()),
      editing_(!originalDomain_.isEmpty()) {
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setModal(true);
    // 端口行决定了自适应下限，弹窗会显得窄；给个下限让整排字段撑开。
    setMinimumWidth(500);

    buildUi();

    if (editing_) {
        const QVector<Site> all = sites::load();
        if (const Site* site = sites::find(all, originalDomain_)) {
            fillFrom(*site);
            originalPort_ = site->port;
        }
    } else {
        expireEdit_->setText(QDate::currentDate().addYears(10).toString(Qt::ISODate));
        domainEdit_->setFocus();
    }
    adjustSize();
}

void SiteDialog::buildUi() {
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

    auto* titleLabel = new QLabel(editing_ ? QStringLiteral("%1 配置").arg(originalDomain_)
                                           : QStringLiteral("创建网站"),
                                  card);
    titleLabel->setStyleSheet(QStringLiteral(
        "font-size: 16px; font-weight: 700; color: %1;")
        .arg(theme.color(QStringLiteral("ink")).name()));
    cardLayout->addWidget(titleLabel);

    auto* divider = new QFrame(card);
    divider->setObjectName(QStringLiteral("Divider"));
    divider->setFixedHeight(1);
    divider->setStyleSheet(QStringLiteral("background: %1; border: none;")
                               .arg(theme.color(QStringLiteral("line")).name()));
    cardLayout->addWidget(divider);

    auto makeFieldRow = [this](QVBoxLayout* layout, const QString& text, QWidget* field) {
        auto* row = new QWidget(this);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(10);
        rowLayout->addWidget(makeRowLabel(text, row));
        rowLayout->addWidget(field, 1);
        layout->addWidget(row);
    };

    domainEdit_ = makeInput(this);
    domainEdit_->setPlaceholderText(QStringLiteral("www.example.com"));
    makeFieldRow(cardLayout, QStringLiteral("域名"), domainEdit_);

    aliasEdit_ = makeInput(this);
    aliasEdit_->setPlaceholderText(QStringLiteral("选填，多个用空格或逗号分隔"));
    makeFieldRow(cardLayout, QStringLiteral("第二域名"), aliasEdit_);

    // 端口行：http / https 单选 + 数字框。证书属于 P6，https 先置灰。
    auto* portRow = new QWidget(this);
    auto* portRowLayout = new QHBoxLayout(portRow);
    portRowLayout->setContentsMargins(0, 0, 0, 0);
    portRowLayout->setSpacing(10);
    portRowLayout->addWidget(makeRowLabel(QStringLiteral("端口"), portRow));
    httpRadio_ = new QRadioButton(QStringLiteral("http"), portRow);
    httpRadio_->setChecked(true);
    httpsRadio_ = new QRadioButton(QStringLiteral("https"), portRow);
    httpsRadio_->setEnabled(false);    // 需要证书，等 P6
    httpsRadio_->setToolTip(QStringLiteral("https 需要证书，后续版本支持"));
    portSpin_ = new QSpinBox(portRow);
    portSpin_->setRange(1, 65535);
    portSpin_->setValue(9999);
    portSpin_->setFixedHeight(kFieldHeight);
    portRowLayout->addWidget(httpRadio_);
    portRowLayout->addWidget(httpsRadio_);
    portRowLayout->addWidget(portSpin_);
    portRowLayout->addStretch();
    cardLayout->addWidget(portRow);

    // 根目录行：输入 + 浏览
    auto* rootRow = new QWidget(this);
    auto* rootRowLayout = new QHBoxLayout(rootRow);
    rootRowLayout->setContentsMargins(0, 0, 0, 0);
    rootRowLayout->setSpacing(10);
    rootRowLayout->addWidget(makeRowLabel(QStringLiteral("根目录"), rootRow));
    rootEdit_ = makeInput(rootRow);
    rootEdit_->setPlaceholderText(QStringLiteral("留空则默认 WWW/<域名>"));
    auto* browseBtn = new QPushButton(QStringLiteral("浏览"), rootRow);
    browseBtn->setObjectName(QStringLiteral("SoftBtn"));
    browseBtn->setCursor(Qt::PointingHandCursor);
    browseBtn->setFixedHeight(kFieldHeight);
    connect(browseBtn, &QPushButton::clicked, this, &SiteDialog::browseRoot);
    rootRowLayout->addWidget(rootEdit_, 1);
    rootRowLayout->addWidget(browseBtn);
    cardLayout->addWidget(rootRow);

    expireEdit_ = makeInput(this);
    expireEdit_->setPlaceholderText(QStringLiteral("选填，如 2031-08-12"));
    makeFieldRow(cardLayout, QStringLiteral("到期日期"), expireEdit_);

    remarkEdit_ = makeInput(this);
    remarkEdit_->setPlaceholderText(QStringLiteral("选填"));
    makeFieldRow(cardLayout, QStringLiteral("备注"), remarkEdit_);

    errorLabel_ = new QLabel(card);
    errorLabel_->setWordWrap(true);
    errorLabel_->setVisible(false);
    errorLabel_->setStyleSheet(QStringLiteral(
        "font-size: 12px; color: %1; padding: 6px 8px;"
        "background: rgba(222, 91, 76, 0.08); border-radius: 4px;")
        .arg(theme.color(QStringLiteral("danger")).name()));
    cardLayout->addWidget(errorLabel_);

    auto* btnLayout = new QHBoxLayout;
    btnLayout->setContentsMargins(0, 0, 0, 0);
    if (editing_) {
        deleteBtn_ = new QPushButton(QStringLiteral("删除站点"), card);
        deleteBtn_->setObjectName(QStringLiteral("DangerBtn"));
        deleteBtn_->setCursor(Qt::PointingHandCursor);
        connect(deleteBtn_, &QPushButton::clicked, this, &SiteDialog::deleteClicked);
        btnLayout->addWidget(deleteBtn_);
    }
    btnLayout->addStretch();

    auto* cancelBtn = new QPushButton(QStringLiteral("取消"), card);
    cancelBtn->setObjectName(QStringLiteral("SoftBtn"));
    cancelBtn->setCursor(Qt::PointingHandCursor);
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);

    auto* confirmBtn = new QPushButton(QStringLiteral("确认"), card);
    confirmBtn->setObjectName(QStringLiteral("PrimaryBtn"));
    confirmBtn->setCursor(Qt::PointingHandCursor);
    confirmBtn->setDefault(true);
    connect(confirmBtn, &QPushButton::clicked, this, &SiteDialog::confirmClicked);

    btnLayout->addWidget(cancelBtn);
    btnLayout->addWidget(confirmBtn);
    cardLayout->addLayout(btnLayout);

    card->setStyleSheet(QStringLiteral(
        "#ConfirmCard { background: %1; border-radius: 12px; }")
        .arg(theme.color(QStringLiteral("card")).name()));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 20);
    root->addWidget(card);
}

void SiteDialog::fillFrom(const Site& site) {
    domainEdit_->setText(site.domain);
    aliasEdit_->setText(site.aliases.join(QLatin1Char(' ')));
    portSpin_->setValue(site.port);
    rootEdit_->setText(site.rootDir);
    expireEdit_->setText(site.expireAt);
    remarkEdit_->setText(site.remark);
}

Site SiteDialog::collect() const {
    Site site;
    site.domain = domainEdit_->text().trimmed().toLower();
    const QRegularExpression separators(QStringLiteral("[\\s,;，；]+"));
    for (const QString& raw : aliasEdit_->text().split(separators, Qt::SkipEmptyParts)) {
        const QString alias = raw.trimmed().toLower();
        if (!alias.isEmpty() && !site.aliases.contains(alias)) site.aliases.append(alias);
    }
    site.port = portSpin_->value();
    site.rootDir = QDir::cleanPath(rootEdit_->text().trimmed());
    // 没填根目录就和 phpStudy 一样落到默认站点目录（WWW/<域名>），不强迫用户先想路径。
    if (site.rootDir.isEmpty()) site.rootDir = sites::defaultRootDir(site.domain);
    // 相对路径按安装根目录解析，绿色安装不能假设用户的盘符布局。
    if (!QDir::isAbsolutePath(site.rootDir)) {
        site.rootDir = QDir::cleanPath(paths::installDir() + QLatin1Char('/') + site.rootDir);
    }
    site.expireAt = expireEdit_->text().trimmed();
    site.remark = remarkEdit_->text().trimmed();
    return site;
}

bool SiteDialog::apply(QString* error) {
    const Site site = collect();
    const QVector<Site> all = sites::load();
    const Site* self = editing_ ? sites::find(all, originalDomain_) : nullptr;

    if (!sites::validate(site, self, all, error)) return false;

    if (engine_) {
        // 站点端口不能和 Web 服务自身端口撞（默认站点占着 80）。
        if (const ServiceInstance* web = engine_->defaultOf("nginx")) {
            const int webPort = engine_->portOf(web->id);
            if (webPort > 0 && webPort == site.port) {
                if (error) {
                    *error = QStringLiteral("端口 %1 已被 Web 服务默认站点占用，请换一个")
                                 .arg(site.port);
                }
                return false;
            }
        }
        // 端口被别的进程占着就点名（端口没变就跳过——nginx 自己可能正监听它）。
        const bool portChanged = !self || self->port != site.port;
        if (portChanged && !net::isPortFree(quint16(site.port))) {
            const net::PortOccupier holder = net::identifyPortOwner(quint16(site.port));
            QString who = holder.processName.isEmpty()
                              ? QStringLiteral("pid %1").arg(holder.pid)
                              : QStringLiteral("%1 (pid %2)").arg(holder.processName).arg(holder.pid);
            if (!holder.executable.isEmpty()) who += QStringLiteral(" from %1").arg(holder.executable);
            if (error) *error = QStringLiteral("端口 %1 已被 %2 占用").arg(site.port).arg(who);
            return false;
        }
    }

    if (!sites::prepareRoot(site, error)) return false;

    QVector<Site> out;
    for (const Site& existing : all) {
        const bool isSelf = self
                            && existing.domain.compare(originalDomain_, Qt::CaseInsensitive) == 0;
        if (!isSelf) out.append(existing);
    }
    out.append(site);
    if (!sites::save(out, error)) return false;
    log::info(QStringLiteral("网站：%1 %2（%3:%4 → %5）")
                  .arg(editing_ ? QStringLiteral("更新") : QStringLiteral("创建"))
                  .arg(site.domain)
                  .arg(site.port)
                  .arg(site.rootDir));
    return true;
}

void SiteDialog::browseRoot() {
    const QString start = rootEdit_->text().trimmed().isEmpty()
                              ? paths::wwwDir()
                              : rootEdit_->text().trimmed();
    const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("选择站点目录"), start);
    if (!dir.isEmpty()) rootEdit_->setText(QDir::toNativeSeparators(dir));
}

void SiteDialog::confirmClicked() {
    QString error;
    if (apply(&error)) {
        accept();
        return;
    }
    errorLabel_->setText(error);
    errorLabel_->setVisible(true);
    adjustSize();
}

void SiteDialog::deleteClicked() {
    if (!ConfirmDialog::ask(this, QStringLiteral("删除站点"),
                            QStringLiteral("站点 %1 会从列表移除，Web 服务不再托管它。\n\n"
                                           "磁盘上的文件不会被删除。")
                                .arg(originalDomain_),
                            QStringLiteral("删除"), true)) {
        return;
    }
    QVector<Site> out;
    for (const Site& site : sites::load()) {
        if (site.domain.compare(originalDomain_, Qt::CaseInsensitive) != 0) out.append(site);
    }
    QString error;
    if (!sites::save(out, &error)) {
        errorLabel_->setText(error);
        errorLabel_->setVisible(true);
        adjustSize();
        return;
    }
    log::info(QStringLiteral("网站：删除 %1").arg(originalDomain_));
    accept();
}

} // namespace devdatapod
