#include "ui/widgets/pages/SettingsPage.h"

#include "core/util/SettingsStore.h"
#include "ui/theme/Theme.h"
#include "ui/widgets/ConfirmDialog.h"
#include "ui/widgets/ToggleSwitch.h"
#include "ui/widgets/UiFactory.h"

#include <QDesktopServices>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace devdatapod {

namespace {

constexpr const char* kReleaseApi =
    "https://api.github.com/repos/haigle0127/devdatapod/releases/latest";

// 版本号按 . 分段数值比较（"1.0.10" > "1.0.9"），忽略前导 v。返回 1 / 0 / -1。
int compareVersions(const QString& a, const QString& b) {
    QString ta = a;
    QString tb = b;
    const QStringList pa = ta.remove(QLatin1Char('v')).split(QLatin1Char('.'));
    const QStringList pb = tb.remove(QLatin1Char('v')).split(QLatin1Char('.'));
    const int count = qMax(pa.size(), pb.size());
    for (int i = 0; i < count; ++i) {
        const int da = i < pa.size() ? pa.at(i).toInt() : 0;
        const int db = i < pb.size() ? pb.at(i).toInt() : 0;
        if (da != db) return da > db ? 1 : -1;
    }
    return 0;
}

// 从 release JSON 里提取版本号：优先 tag_name（剥前导 v），找不到就从资产名
// DevDataPod-vX.Y.Z-Windows.exe 里抠（发布命名约定，见 installer/devdatapod.iss）。
// 下载地址优先取那个 exe 资产的直链。
QString extractVersion(const QJsonObject& release, QString* downloadUrl) {
    QString version = release.value(QStringLiteral("tag_name")).toString();
    const QJsonArray assets = release.value(QStringLiteral("assets")).toArray();
    static const QRegularExpression pattern(
        QStringLiteral("DevDataPod-v([0-9.]+)-Windows\\.exe"),
        QRegularExpression::CaseInsensitiveOption);
    QString assetVersion;
    for (const QJsonValue& value : assets) {
        const QJsonObject asset = value.toObject();
        const QString name = asset.value(QStringLiteral("name")).toString();
        const auto match = pattern.match(name);
        if (match.hasMatch()) {
            assetVersion = match.captured(1);
            *downloadUrl = asset.value(QStringLiteral("browser_download_url")).toString();
            break;
        }
    }
    if (version.isEmpty()) version = assetVersion;
    return version;
}

} // namespace

SettingsPage::SettingsPage(QWidget* parent) : QWidget(parent) {
    buildUi();
    loadSettings();
}

void SettingsPage::buildUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 18, 0, 0);
    layout->setSpacing(12);

    auto* card = ui::makeCard(this);
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(18, 14, 18, 14);
    cardLayout->setSpacing(6);

    // 头部
    auto* head = new QWidget(card);
    auto* headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(0, 0, 0, 6);
    headLayout->setSpacing(10);
    headLayout->addWidget(ui::makeAccentBar(QStringLiteral("accent"), 18, head));
    headLayout->addWidget(ui::makeLabel(QStringLiteral("设置"),
                                        QStringLiteral("SectionTitle"), head));
    headLayout->addStretch();
    cardLayout->addWidget(head);

    // ── 服务设置组 ──
    cardLayout->addWidget(ui::makeLabel(QStringLiteral("服务"),
                                        QStringLiteral("GroupTitle"), card));

    // 自动重启开关行
    auto* restartRow = new QWidget(card);
    auto* restartLayout = new QHBoxLayout(restartRow);
    restartLayout->setContentsMargins(0, 4, 0, 4);
    restartLayout->setSpacing(12);

    auto* restartText = new QWidget(restartRow);
    auto* restartTextLayout = new QVBoxLayout(restartText);
    restartTextLayout->setContentsMargins(0, 0, 0, 0);
    restartTextLayout->setSpacing(2);
    auto* restartTitle = ui::makeLabel(QStringLiteral("编辑配置后自动重启服务"),
                                       QStringLiteral("SvcName"), restartText);
    auto* restartDesc = ui::makeLabel(
        QStringLiteral("修改端口或组件设置后自动重启该服务使配置生效；关闭则只保存配置，需手动重启"),
        QStringLiteral("EmptyHint"), restartText);
    restartTextLayout->addWidget(restartTitle);
    restartTextLayout->addWidget(restartDesc);

    autoRestartSwitch_ = new ToggleSwitch(restartRow);
    autoRestartSwitch_->setIcon(ToggleSwitch::CheckCross);

    restartLayout->addWidget(restartText, 1);
    restartLayout->addWidget(autoRestartSwitch_, 0, Qt::AlignVCenter);
    cardLayout->addWidget(restartRow);

    // 已保存提示（初始隐藏）
    savedHint_ = ui::makeLabel(QStringLiteral("已保存"), QStringLiteral("SectionHint"), card);
    savedHint_->setStyleSheet(QStringLiteral("color: %1;")
                                  .arg(Theme::instance().color(QStringLiteral("success")).name()));
    savedHint_->setVisible(false);
    cardLayout->addWidget(savedHint_, 0, Qt::AlignRight);

    // ── 关于与更新组 ──
    auto* aboutTitle = ui::makeLabel(QStringLiteral("关于"), QStringLiteral("GroupTitle"), card);
    cardLayout->addWidget(aboutTitle);

    auto* versionRow = new QWidget(card);
    auto* versionLayout = new QHBoxLayout(versionRow);
    versionLayout->setContentsMargins(0, 4, 0, 4);
    versionLayout->setSpacing(12);
    auto* versionText = new QWidget(versionRow);
    auto* versionTextLayout = new QVBoxLayout(versionText);
    versionTextLayout->setContentsMargins(0, 0, 0, 0);
    versionTextLayout->setSpacing(2);
    auto* versionTitle = ui::makeLabel(QStringLiteral("检查更新"), QStringLiteral("SvcName"),
                                       versionText);
    auto* versionDesc = ui::makeLabel(
        QStringLiteral("从 GitHub Releases 检查 devdatapod 新版本"),
        QStringLiteral("EmptyHint"), versionText);
    versionTextLayout->addWidget(versionTitle);
    versionTextLayout->addWidget(versionDesc);
    updateStatus_ = ui::makeLabel(QStringLiteral("当前版本 %1")
                                      .arg(QString::fromLatin1(DEVDATAPOD_VERSION)),
                                  QStringLiteral("SectionHint"), versionRow);
    checkUpdateBtn_ = new QPushButton(QStringLiteral("检查更新"), versionRow);
    checkUpdateBtn_->setObjectName(QStringLiteral("SoftBtn"));
    checkUpdateBtn_->setCursor(Qt::PointingHandCursor);
    versionLayout->addWidget(versionText, 1);
    versionLayout->addWidget(updateStatus_, 0, Qt::AlignVCenter);
    versionLayout->addWidget(checkUpdateBtn_, 0, Qt::AlignVCenter);
    cardLayout->addWidget(versionRow);

    connect(checkUpdateBtn_, &QPushButton::clicked, this, &SettingsPage::checkUpdate);

    cardLayout->addStretch();
    layout->addWidget(card, 1);

    // 开关切换即保存
    connect(autoRestartSwitch_, &ToggleSwitch::toggled, this, [this](bool on) {
        SettingsStore::instance().setValue(QStringLiteral("ui/autoRestartOnConfig"), on);
        SettingsStore::instance().sync();
        savedHint_->setVisible(true);
        QTimer::singleShot(2000, this, [this] { savedHint_->setVisible(false); });
    });
}

void SettingsPage::loadSettings() {
    const bool autoRestart = SettingsStore::instance()
                                 .value(QStringLiteral("ui/autoRestartOnConfig"), true)
                                 .toBool();
    autoRestartSwitch_->setOn(autoRestart, false);
}

void SettingsPage::saveSettings() {
    // 切换时已即时保存，这里预留后续批量设置的场景
}

void SettingsPage::checkUpdate() {
    if (!nam_) nam_ = new QNetworkAccessManager(this);
    checkUpdateBtn_->setEnabled(false);
    updateStatus_->setText(QStringLiteral("正在检查…"));

    QNetworkRequest request{QUrl(QString::fromLatin1(kReleaseApi))};
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("devdatapod"));
    QNetworkReply* reply = nam_->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        checkUpdateBtn_->setEnabled(true);
        if (reply->error() != QNetworkReply::NoError) {
            // 详细原因放 tooltip，行内只放短文案，不然会把按钮顶没。
            updateStatus_->setText(QStringLiteral("检查失败"));
            updateStatus_->setToolTip(reply->errorString());
            return;
        }
        QJsonParseError parse{};
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &parse);
        QString downloadUrl;
        const QString latest =
            (parse.error == QJsonParseError::NoError && doc.isObject())
                ? extractVersion(doc.object(), &downloadUrl)
                : QString();
        if (latest.isEmpty()) {
            updateStatus_->setText(QStringLiteral("检查失败：解析不到版本号"));
            return;
        }
        const QString current = QString::fromLatin1(DEVDATAPOD_VERSION);
        if (compareVersions(latest, current) > 0) {
            updateStatus_->setText(QStringLiteral("发现新版本 %1").arg(latest));
            if (ConfirmDialog::ask(this, QStringLiteral("发现新版本"),
                                   QStringLiteral("当前版本 %1，最新版本 %2。\n\n"
                                                  "是否打开下载页面？")
                                       .arg(current, latest),
                                   QStringLiteral("打开下载页"))) {
                const QString url =
                    downloadUrl.isEmpty()
                        ? QStringLiteral(
                              "https://github.com/haigle0127/devdatapod/releases/latest")
                        : downloadUrl;
                QDesktopServices::openUrl(QUrl(url));
            }
        } else {
            updateStatus_->setText(
                QStringLiteral("已是最新版本（%1）").arg(current));
        }
    });
}

} // namespace devdatapod
