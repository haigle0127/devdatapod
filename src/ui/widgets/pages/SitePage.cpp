#include "ui/widgets/pages/SitePage.h"

#include "core/service/Engine.h"
#include "core/service/SiteStore.h"
#include "ui/theme/Theme.h"
#include "ui/widgets/UiFactory.h"

#include <QDir>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace devdatapod {

namespace {
constexpr int kDomainWidth = 190;    // 列宽对齐下面 rebuild() 里的表头
constexpr int kPortWidth = 76;
constexpr int kStatusWidth = 76;
constexpr int kExpireWidth = 120;
constexpr int kActionWidth = 72;
constexpr int kPathElidePx = 400;    // 物理路径列的省略宽度

// 物理路径太长就中间省略，整段放进 tooltip 里。
QString elidePath(const QString& path) {
    return QFontMetrics(QFont()).elidedText(path, Qt::ElideMiddle, kPathElidePx);
}

QLabel* makeHeadCell(const QString& text, QWidget* parent, int width = 0) {
    auto* label = ui::makeLabel(text, QStringLiteral("TableHead"), parent);
    if (width > 0) label->setFixedWidth(width);
    return label;
}
} // namespace

SitePage::SitePage(Engine* engine, QWidget* parent) : QWidget(parent), engine_(engine) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 18, 0, 0);
    layout->setSpacing(12);

    auto* card = ui::makeCard(this);
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(18, 14, 18, 14);
    cardLayout->setSpacing(6);

    auto* head = new QWidget(card);
    auto* headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(0, 0, 0, 6);
    headLayout->setSpacing(10);

    auto* createBtn = new QPushButton(QStringLiteral("＋ 创建网站"), head);
    createBtn->setObjectName(QStringLiteral("PrimaryBtn"));
    createBtn->setCursor(Qt::PointingHandCursor);
    connect(createBtn, &QPushButton::clicked, this, &SitePage::createRequested);
    headLayout->addWidget(createBtn);

    headLayout->addStretch();

    search_ = new QLineEdit(head);
    search_->setObjectName(QStringLiteral("FieldInput"));
    search_->setFixedHeight(30);
    search_->setFixedWidth(220);
    search_->setPlaceholderText(QStringLiteral("查找"));
    connect(search_, &QLineEdit::textChanged, this, &SitePage::rebuild);
    headLayout->addWidget(search_);
    cardLayout->addWidget(head);

    // 表头（截图那行灰字：网站域名 / 端口 / 物理路径 / 状态 / 到期 / 操作）。
    auto* tableHead = new QWidget(card);
    auto* tableHeadLayout = new QHBoxLayout(tableHead);
    tableHeadLayout->setContentsMargins(4, 4, 4, 6);
    tableHeadLayout->setSpacing(12);
    tableHeadLayout->addWidget(makeHeadCell(QStringLiteral("网站域名"), tableHead, kDomainWidth));
    tableHeadLayout->addWidget(makeHeadCell(QStringLiteral("端口"), tableHead, kPortWidth));
    auto* pathHead = makeHeadCell(QStringLiteral("物理路径"), tableHead);
    pathHead->setAlignment(Qt::AlignLeft);
    tableHeadLayout->addWidget(pathHead, 1);
    tableHeadLayout->addWidget(makeHeadCell(QStringLiteral("状态"), tableHead, kStatusWidth));
    tableHeadLayout->addWidget(makeHeadCell(QStringLiteral("到期"), tableHead, kExpireWidth));
    tableHeadLayout->addWidget(makeHeadCell(QStringLiteral("操作"), tableHead, kActionWidth));
    cardLayout->addWidget(tableHead);

    auto* scroll = new QScrollArea(card);
    scroll->setObjectName(QStringLiteral("SuiteScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* body = new QWidget(scroll);
    body->setObjectName(QStringLiteral("SuiteBody"));
    body->setAttribute(Qt::WA_StyledBackground, true);
    listLayout_ = new QVBoxLayout(body);
    listLayout_->setContentsMargins(0, 0, 0, 0);
    listLayout_->setSpacing(6);
    scroll->setWidget(body);
    cardLayout->addWidget(scroll, 1);

    emptyHint_ = ui::makeLabel(QStringLiteral("还没有网站。先启动 web 服务，再点「创建网站」部署第一个站点。"),
                               QStringLiteral("EmptyHint"), card);
    cardLayout->addWidget(emptyHint_);

    rebuild();

    layout->addWidget(card, 1);
}

bool SitePage::webRunning() const {
    if (!engine_) return false;
    const ServiceInstance* web = engine_->defaultOf(QStringLiteral("nginx"));
    return web && engine_->isRunning(web->id);
}

void SitePage::reload() {
    rebuild();
}

void SitePage::rebuild() {
    // 清掉旧的行（stretch 也要清，否则新行排到它后面被挤下去）。
    QLayoutItem* child;
    while ((child = listLayout_->takeAt(0)) != nullptr) {
        if (child->widget()) delete child->widget();
        delete child;
    }

    const QString query = search_ ? search_->text().trimmed() : QString();
    const bool webUp = webRunning();
    const QVector<Site> all = sites::load();

    int shown = 0;
    for (const Site& site : all) {
        // 查找：域名 / 第二域名 / 路径 / 端口都能命中。
        if (!query.isEmpty()) {
            const QString haystack = site.domain + QLatin1Char(' ') + site.aliases.join(QLatin1Char(' '))
                                     + QLatin1Char(' ') + site.rootDir + QLatin1Char(' ')
                                     + QString::number(site.port);
            if (!haystack.contains(query, Qt::CaseInsensitive)) continue;
        }

        auto* row = new QWidget(this);
        row->setObjectName(QStringLiteral("ServiceRow"));    // 复用行底部分隔线样式
        row->setAttribute(Qt::WA_StyledBackground, true);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(4, 8, 4, 8);
        rowLayout->setSpacing(12);

        auto* name = ui::makeLabel(site.domain, QStringLiteral("SvcName"), row);
        name->setFixedWidth(kDomainWidth);
        name->setToolTip(site.aliases.isEmpty() ? site.domain
                                                : site.domain + QStringLiteral("（别名：")
                                                      + site.aliases.join(QLatin1Char(' '))
                                                      + QLatin1Char('）'));
        rowLayout->addWidget(name);

        auto* port = ui::makeLabel(QString::number(site.port), QStringLiteral("PortTag"), row);
        port->setFixedWidth(kPortWidth);
        port->setAlignment(Qt::AlignCenter);
        rowLayout->addWidget(port);

        const QString path = QDir::toNativeSeparators(site.rootDir);
        auto* pathLabel = ui::makeLabel(elidePath(path), QStringLiteral("EmptyHint"), row);
        pathLabel->setToolTip(path);
        rowLayout->addWidget(pathLabel, 1);

        // 状态 = Web 服务是否在跑。跑 = 正常（绿），没跑 = 已停用（灰）。
        auto* status = ui::makeLabel(webUp ? QStringLiteral("正常") : QStringLiteral("已停用"),
                                     QStringLiteral("SvcState"), row);
        status->setProperty("state", webUp ? "running" : "stopped");
        status->setFixedWidth(kStatusWidth);
        rowLayout->addWidget(status);

        auto* expire = ui::makeLabel(site.expireAt.isEmpty() ? QStringLiteral("—") : site.expireAt,
                                     QStringLiteral("SvcState"), row);
        expire->setFixedWidth(kExpireWidth);
        rowLayout->addWidget(expire);

        auto* manage = new QPushButton(QStringLiteral("管理"), row);
        manage->setObjectName(QStringLiteral("SoftBtn"));
        manage->setCursor(Qt::PointingHandCursor);
        manage->setFixedWidth(64);
        connect(manage, &QPushButton::clicked, this, [this, domain = site.domain] {
            emit manageRequested(domain);
        });
        rowLayout->addWidget(manage);
        rowLayout->setAlignment(manage, Qt::AlignLeft);

        listLayout_->addWidget(row);
        ++shown;
    }

    emptyHint_->setVisible(shown == 0);

    // 和 SoftwarePage 一样：行之后永远补一个 stretch，否则独占一行的行会被布局拉成
    // 整个视口高（列表内容比视口矮时）。
    listLayout_->addStretch();
}

} // namespace devdatapod
