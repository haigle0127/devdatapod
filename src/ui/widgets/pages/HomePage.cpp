#include "ui/widgets/pages/HomePage.h"

#include "core/process/SystemStat.h"
#include "core/util/AppPaths.h"
#include "core/util/LogTailReader.h"
#include "ui/theme/IconFactory.h"
#include "ui/theme/Theme.h"
#include "ui/widgets/GaugeGroup.h"
#include "ui/widgets/MeterBar.h"
#include "ui/widgets/UiFactory.h"

#include <QAbstractSocket>
#include <QButtonGroup>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QNetworkInterface>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QSysInfo>
#include <QTextCursor>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace devdatapod {

namespace {
constexpr int kMenuWidth = 168;
constexpr int kMenuCount = 3;

QLabel* makeGlyph(Glyph glyph, const QString& token, int size, QWidget* parent) {
    auto* label = new QLabel(parent);
    label->setPixmap(icons::pixmap(glyph, Theme::instance().color(token), size));
    return label;
}

QString percentText(double fraction) {
    return QStringLiteral("%1%").arg(fraction * 100.0, 0, 'f', 1);
}

QString formatUptime(qint64 ms) {
    if (ms < 0) return QStringLiteral("--");
    const qint64 totalMinutes = ms / 60000;
    const qint64 days = totalMinutes / 1440;
    const qint64 hours = (totalMinutes % 1440) / 60;
    const qint64 minutes = totalMinutes % 60;
    if (days > 0) return QStringLiteral("%1 天 %2 小时").arg(days).arg(hours);
    if (hours > 0) return QStringLiteral("%1 小时 %2 分钟").arg(hours).arg(minutes);
    return QStringLiteral("%1 分钟").arg(qMax<qint64>(minutes, 1));
}

// 第一块活动的非回环 IPv4，就是本机在局域网里的地址；没有就 "--"。
QString lanIPv4() {
    const QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface& iface : interfaces) {
        const QNetworkInterface::InterfaceFlags flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning)
            || (flags & QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol
                && !entry.ip().isLoopback()) {
                return entry.ip().toString();
            }
        }
    }
    return QStringLiteral("--");
}

// 下载缓存（data/downloads 里平铺的压缩包）总体积，MB 一位小数。
QString downloadsCacheText() {
    const QFileInfoList files =
        QDir(paths::dataDir() + QStringLiteral("/downloads"))
            .entryInfoList(QDir::Files);
    qint64 total = 0;
    for (const QFileInfo& file : files) total += file.size();
    if (total <= 0) return QStringLiteral("0 MB");
    return QStringLiteral("%1 MB").arg(total / 1048576.0, 0, 'f', 1);
}

// 值标签：宽度不够时自动省略号截断（而不是硬切），全文放 tooltip。
// 不需要 Q_OBJECT：没有新信号，resize 时按当前宽度重新省略。
class ElideLabel : public QLabel {
public:
    explicit ElideLabel(const QString& text, QWidget* parent = nullptr,
                        Qt::TextElideMode mode = Qt::ElideRight)
        : QLabel(parent), full_(text), mode_(mode) {
        refresh();
    }
    void setTextFull(const QString& text) {
        full_ = text;
        refresh();
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QLabel::resizeEvent(event);
        refresh();
    }

private:
    void refresh() {
        QLabel::setText(fontMetrics().elidedText(full_, mode_, qMax(0, width() - 2)));
        setToolTip(full_);
    }
    QString full_;
    Qt::TextElideMode mode_;
};
}

HomePage::HomePage(const QString& announcement,
                   const QVector<ServiceRowInfo>& services,
                   const QString& version,
                   QWidget* parent)
    : QWidget(parent), version_(version) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 18, 0, 0);
    layout->setSpacing(12);

    layout->addWidget(buildAnnouncement(announcement, this));
    layout->addWidget(buildSuite(services, this), 1);    // 套件列表占大头
    layout->addWidget(buildBottomSection(this));
}

QWidget* HomePage::buildAnnouncement(const QString& text, QWidget* parent) {
    auto* card = ui::makeCard(parent);
    auto* layout = new QHBoxLayout(card);
    layout->setContentsMargins(14, 8, 14, 8);
    layout->setSpacing(10);
    layout->addWidget(makeGlyph(Glyph::Speaker, QStringLiteral("accent"), 15, card));
    layout->addWidget(ui::makeLabel(text, QStringLiteral("AnnText"), card));
    layout->addStretch();
    layout->addWidget(ui::makeLabel(QStringLiteral("公告"), QStringLiteral("AnnLink"), card));
    return card;
}

QWidget* HomePage::buildSuite(const QVector<ServiceRowInfo>& services, QWidget* parent) {
    auto* card = ui::makeCard(parent);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(18, 14, 18, 14);
    layout->setSpacing(6);

    auto* head = new QWidget(card);
    auto* headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(0, 0, 0, 6);
    headLayout->setSpacing(10);
    headLayout->addWidget(ui::makeAccentBar(QStringLiteral("accent"), 18, head));
    headLayout->addWidget(ui::makeLabel(QStringLiteral("套件"),
                                        QStringLiteral("SectionTitle"), head));

    suiteTotal_ = ui::makeLabel(QString(), QStringLiteral("SectionHint"), head);
    suiteRunning_ = ui::makeLabel(QString(), QStringLiteral("SectionHint"), head);
    headLayout->addWidget(suiteTotal_);
    headLayout->addStretch();
    headLayout->addWidget(suiteRunning_);
    layout->addWidget(head);

    // 行多了会挤，把行容器放进滚动区：头部固定，行列表上下滑。
    auto* scroll = new QScrollArea(card);
    scroll->setObjectName(QStringLiteral("SuiteScroll"));
    scroll->setWidgetResizable(true);          // 内容宽度跟随滚动区
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* body = new QWidget(scroll);
    body->setObjectName(QStringLiteral("SuiteBody"));
    body->setAttribute(Qt::WA_StyledBackground, true);
    suiteLayout_ = new QVBoxLayout(body);
    suiteLayout_->setContentsMargins(0, 0, 0, 0);
    suiteLayout_->setSpacing(2);
    scroll->setWidget(body);
    layout->addWidget(scroll, 1);              // 占满卡片剩余高度

    setServices(services);
    return card;
}

void HomePage::replaceRows(const QVector<ServiceRowInfo>& services) {
    // 连 stretch 一起清掉，否则每次重建都多一个 stretch，行被挤到中间。
    QLayoutItem* child;
    while ((child = suiteLayout_->takeAt(0)) != nullptr) {
        if (child->widget()) delete child->widget();
        delete child;
    }
    rows_.clear();

    QWidget* card = suiteLayout_->parentWidget();
    for (const ServiceRowInfo& info : services) {
        auto* row = new ServiceRow(info, card);
        const QString id = info.id;
        connect(row, &ServiceRow::primaryRequested, this,
                [this, id] { emit servicePrimaryRequested(id); });
        connect(row, &ServiceRow::restartRequested, this,
                [this, id] { emit serviceRestartRequested(id); });
        connect(row, &ServiceRow::configureRequested, this,
                [this, id] { emit serviceConfigureRequested(id); });
        connect(row, &ServiceRow::promoteRequested, this,
                [this, id] { emit servicePromoteRequested(id); });
        suiteLayout_->addWidget(row);
        rows_.append(row);
    }

    if (services.isEmpty()) {
        suiteLayout_->addWidget(ui::makeLabel(
            QStringLiteral("还没有已安装的服务实例，套件列表会在装好第一个组件后出现"),
            QStringLiteral("EmptyHint"), suiteLayout_->parentWidget()));
    }
    // 行贴顶部排，多余空白沉到底部，别把行均分拉开。
    suiteLayout_->addStretch();
}

void HomePage::setServices(const QVector<ServiceRowInfo>& services) {
    if (!suiteLayout_) return;

    const int count = static_cast<int>(services.size());
    bool reusable = rows_.size() == count;
    // Rows capture their instance id when connected, so reuse is only sound while the id
    // sequence matches position for position. Any install/removal rebuilds them.
    for (int i = 0; reusable && i < count; ++i) {
        if (rows_.at(i)->id() != services.at(i).id) reusable = false;
    }

    if (reusable) {
        for (int i = 0; i < count; ++i) {
            rows_.at(i)->setInfo(services.at(i));
            rows_.at(i)->setState(services.at(i).state);
        }
    } else {
        replaceRows(services);
    }

    int running = 0;
    for (const ServiceRowInfo& info : services) {
        if (info.state == RunState::Running) ++running;
    }
    suiteTotal_->setText(QStringLiteral("共 %1 个服务").arg(count));
    suiteRunning_->setText(
        QStringLiteral("运行 %1 / 停止 %2").arg(running).arg(count - running));
}

void HomePage::setSystemStats(const sys::Snapshot& snapshot, bool hasCpuBaseline) {
    const bool cpuReady = snapshot.valid && hasCpuBaseline;
    const QString cpuValue = cpuReady ? percentText(snapshot.cpuFraction) : QStringLiteral("--");
    const QString memoryValue =
        snapshot.valid ? percentText(snapshot.memoryFraction()) : QStringLiteral("--");

    if (serverGauges_) {
        serverGauges_->setGauges(
            {{QStringLiteral("CPU使用率"), cpuValue,
              cpuReady ? snapshot.cpuFraction : 0.0, QStringLiteral("accent2")},
             {QStringLiteral("内存使用率"), memoryValue,
              snapshot.valid ? snapshot.memoryFraction() : 0.0, QStringLiteral("accent")}});
    }

    if (cpuBar_) {
        cpuBar_->setFraction(cpuReady ? snapshot.cpuFraction : 0.0);
    }
    if (cpuPercentLabel_) {
        cpuPercentLabel_->setText(cpuValue);
    }
    if (memoryPercentLabel_) {
        const auto gb = [](quint64 bytes) {
            return QString::number(bytes / 1073741824.0, 'f', 1);
        };
        memoryPercentLabel_->setText(
            snapshot.valid ? QStringLiteral("%1　%2 / %3 GB")
                                 .arg(memoryValue)
                                 .arg(gb(snapshot.totalPhysBytes - snapshot.availPhysBytes),
                                      gb(snapshot.totalPhysBytes))
                           : QStringLiteral("--"));
    }
    if (memoryBar_) {
        memoryBar_->setFraction(snapshot.valid ? snapshot.memoryFraction() : 0.0);
    }

    // 「资源信息」页的两个动态格：物理内存可用量随 3 分钟统计刷新；运行时长一并带。
    if (memoryDetailKv_) {
        const auto gb = [](quint64 bytes) {
            return QString::number(bytes / 1073741824.0, 'f', 1);
        };
        static_cast<ElideLabel*>(memoryDetailKv_)
            ->setTextFull(snapshot.valid
                              ? QStringLiteral("可用 %1 / %2 GB")
                                    .arg(gb(snapshot.availPhysBytes),
                                         gb(snapshot.totalPhysBytes))
                              : QStringLiteral("--"));
    }
    if (uptimeKv_) {
        static_cast<ElideLabel*>(uptimeKv_)->setTextFull(formatUptime(sys::uptimeMs()));
    }
}

QWidget* HomePage::buildBottomSection(QWidget* parent) {
    auto* section = new QWidget(parent);
    auto* layout = new QHBoxLayout(section);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(14);

    auto* menu = ui::makeCard(section);
    menu->setFixedWidth(kMenuWidth);
    auto* menuLayout = new QVBoxLayout(menu);
    menuLayout->setContentsMargins(10, 10, 10, 10);
    menuLayout->setSpacing(4);

    auto* pages = new QStackedWidget(section);
    auto* menuGroup = new QButtonGroup(menu);
    menuGroup->setExclusive(true);

    const struct {
        const char* text;
        Glyph glyph;
    } items[kMenuCount] = {
        {"运行状态", Glyph::Activity},
        {"资源信息", Glyph::ChartPie},
        {"日志文件", Glyph::FileText},
    };
    for (int i = 0; i < kMenuCount; ++i) {
        auto* button = new QPushButton(QString::fromUtf8(items[i].text), menu);
        button->setObjectName(QStringLiteral("MenuBtn"));
        button->setCheckable(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::NoFocus);

        // Same reason as the sidebar: only a baked icon can change color when checked.
        QIcon icon;
        icon.addPixmap(icons::pixmap(items[i].glyph,
                                     Theme::instance().color(QStringLiteral("ink-2")), 15),
                       QIcon::Normal, QIcon::Off);
        icon.addPixmap(icons::pixmap(items[i].glyph,
                                     Theme::instance().color(QStringLiteral("accent")), 15),
                       QIcon::Normal, QIcon::On);
        button->setIcon(icon);
        button->setIconSize(QSize(15, 15));
        menuGroup->addButton(button, i);
        menuLayout->addWidget(button);

        auto* page = ui::makeCard(pages);
        auto* pageLayout = new QHBoxLayout(page);
        // 1080 是窗口下限，比设计稿画布窄 200px；底部这块的边距间距要省着用，
        // 否则右边的「处理器与内存」卡内容会被压缩（环被挤没、文字截断）。
        pageLayout->setContentsMargins(16, 14, 16, 14);
        pageLayout->setSpacing(24);
        if (i == 0) {
            serverGauges_ = new GaugeGroup(
                QStringLiteral("服务器状态"), QStringLiteral("accent"),
                {{QStringLiteral("CPU使用率"), QStringLiteral("--"), 0.0,
                  QStringLiteral("accent2")},
                 {QStringLiteral("内存使用率"), QStringLiteral("--"), 0.0,
                  QStringLiteral("accent")}},
                page);
            pageLayout->addWidget(serverGauges_);
            auto* divider = new QFrame(page);
            divider->setObjectName(QStringLiteral("Divider"));
            divider->setFixedWidth(1);
            pageLayout->addWidget(divider, 0, Qt::AlignVCenter);
            // 右卡：CPU / 内存详情。拓扑（核心线程、大小核）是静态数据，建卡时取一次。
            auto* hwCard = ui::makeCard(page);
            auto* hwLayout = new QVBoxLayout(hwCard);
            hwLayout->setContentsMargins(20, 14, 20, 14);
            hwLayout->setSpacing(14);

            auto* hwHead = new QWidget(hwCard);
            auto* hwHeadLayout = new QHBoxLayout(hwHead);
            hwHeadLayout->setContentsMargins(0, 0, 0, 0);
            hwHeadLayout->setSpacing(8);
            hwHeadLayout->addWidget(
                ui::makeAccentBar(QStringLiteral("accent3"), 15, hwHead));
            hwHeadLayout->addWidget(
                ui::makeLabel(QStringLiteral("处理器与内存"), QStringLiteral("GroupTitle"), hwHead));
            hwLayout->addWidget(hwHead);

            auto* content = new QWidget(hwCard);
            auto* contentLayout = new QHBoxLayout(content);
            contentLayout->setContentsMargins(0, 0, 0, 0);
            contentLayout->setSpacing(18);

            // 右侧单列：型号 / 核心线程+大小核 / CPU 仪表 / 内存仪表。
            // 1080 下限宽度里没有空间摆多列，纵向堆叠一行一条反而宽松；
            // CPU / 内存用同一款横向仪表条，CPU 的百分比放大加粗做主视觉。
            auto* rightColumn = new QWidget(content);
            auto* rightLayout = new QVBoxLayout(rightColumn);
            rightLayout->setContentsMargins(0, 2, 0, 2);
            rightLayout->setSpacing(10);

            const sys::CpuTopology topo = sys::cpuTopology();
            const QString model = topo.model.isEmpty() ? QStringLiteral("未知型号") : topo.model;
            cpuModelLabel_ = ui::makeLabel(
                QFontMetrics(QFont()).elidedText(model, Qt::ElideRight, 300),
                QStringLiteral("SvcState"), rightColumn);
            cpuModelLabel_->setToolTip(model);
            coreThreadLabel_ = ui::makeLabel(
                QStringLiteral("核心 / 线程　%1 / %2　·　%3")
                    .arg(topo.physicalCores)
                    .arg(topo.logicalProcessors)
                    .arg(topo.hybrid()
                            ? QStringLiteral("P 核 %1 · E 核 %2")
                                  .arg(topo.performanceCores)
                                  .arg(topo.efficientCores)
                            : QStringLiteral("同构无大小核")),
                QStringLiteral("SvcState"), rightColumn);
            rightLayout->addWidget(cpuModelLabel_);
            rightLayout->addWidget(coreThreadLabel_);

            auto* cpuRow = new QWidget(rightColumn);
            auto* cpuRowLayout = new QHBoxLayout(cpuRow);
            cpuRowLayout->setContentsMargins(0, 0, 0, 0);
            cpuRowLayout->setSpacing(10);
            cpuRowLayout->addWidget(ui::makeLabel(QStringLiteral("CPU 总占用"),
                                                  QStringLiteral("SvcState"), cpuRow));
            cpuBar_ = new MeterBar(cpuRow);
            cpuRowLayout->addWidget(cpuBar_, 1);
            cpuPercentLabel_ = ui::makeLabel(QStringLiteral("--"),
                                             QStringLiteral("SvcState"), cpuRow);
            cpuPercentLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            cpuRowLayout->addWidget(cpuPercentLabel_);
            rightLayout->addWidget(cpuRow);

            auto* memRow = new QWidget(rightColumn);
            auto* memRowLayout = new QHBoxLayout(memRow);
            memRowLayout->setContentsMargins(0, 0, 0, 0);
            memRowLayout->setSpacing(10);
            memRowLayout->addWidget(ui::makeLabel(QStringLiteral("内存使用率"),
                                                  QStringLiteral("SvcState"), memRow));
            memoryBar_ = new MeterBar(memRow);
            memRowLayout->addWidget(memoryBar_, 1);
            memoryPercentLabel_ = ui::makeLabel(QStringLiteral("--"),
                                                QStringLiteral("SvcState"), memRow);
            memoryPercentLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            memRowLayout->addWidget(memoryPercentLabel_);
            rightLayout->addWidget(memRow);
            rightLayout->addStretch();

            contentLayout->addWidget(rightColumn, 1);

            hwLayout->addWidget(content, 1);
            // 占满「服务器状态」圆环剩下的宽度，否则型号和核心线程数会被截断。
            pageLayout->addWidget(hwCard, 1);
        } else if (i == 1) {
            // 「资源信息」：本机与面板的静态信息 + 两个随轮询刷新的计数。
            pageLayout->addWidget(buildResourceInfoPage(page));
        } else {
            // 「日志文件」：面板自身日志的 tail 视图。
            pageLayout->addWidget(buildLogPage(page));
        }
        pages->addWidget(page);
    }
    menuLayout->addStretch();

    connect(menuGroup, &QButtonGroup::idClicked, pages, &QStackedWidget::setCurrentIndex);
    menuGroup->button(0)->setChecked(true);

    layout->addWidget(menu);
    layout->addWidget(pages, 1);
    return section;
}

QWidget* HomePage::buildResourceInfoPage(QWidget* parent) {
    auto* card = ui::makeCard(parent);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(18, 14, 18, 14);
    layout->setSpacing(14);

    auto* head = new QWidget(card);
    auto* headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(0, 0, 0, 0);
    headLayout->setSpacing(8);
    headLayout->addWidget(ui::makeAccentBar(QStringLiteral("accent2"), 15, head));
    headLayout->addWidget(
        ui::makeLabel(QStringLiteral("资源信息"), QStringLiteral("GroupTitle"), head));
    headLayout->addStretch();
    headLayout->addWidget(ui::makeLabel(QStringLiteral("静态信息建卡时读取，计数随轮询刷新"),
                                        QStringLiteral("SectionHint"), head));
    layout->addWidget(head);

    auto* content = new QWidget(card);
    auto* contentLayout = new QHBoxLayout(content);
    contentLayout->setContentsMargins(4, 0, 4, 0);
    contentLayout->setSpacing(16);

    // 键值行：key 固定宽对齐，value 用 ElideLabel（不够宽自动省略号）。
    // 返回 value 便于动态刷新。
    const int kKeyWidth = 60;
    auto addKv = [kKeyWidth](QVBoxLayout* columnLayout, QWidget* column,
                             const QString& key, const QString& value,
                             Qt::TextElideMode mode = Qt::ElideRight) {
        auto* row = new QWidget(column);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(10);
        auto* keyLabel = ui::makeLabel(key, QStringLiteral("KvKey"), row);
        keyLabel->setFixedWidth(kKeyWidth);
        auto* valueLabel = new ElideLabel(value, row, mode);
        rowLayout->addWidget(keyLabel);
        rowLayout->addWidget(valueLabel, 1);
        columnLayout->addWidget(row);
        return valueLabel;
    };
    auto makeColumn = [content, &contentLayout](const QString& title) {
        auto* column = new QWidget(content);
        auto* columnLayout = new QVBoxLayout(column);
        columnLayout->setContentsMargins(0, 0, 0, 0);
        columnLayout->setSpacing(9);
        columnLayout->addWidget(ui::makeLabel(title, QStringLiteral("SectionHint"), column));
        contentLayout->addWidget(column, 1);
        return columnLayout;
    };

    // 系统全名可能很长（"Windows 11 Version 23H2"），列里只放短名，全称在 tooltip。
    const QString osFull = QSysInfo::prettyProductName();
    const QString osShort = osFull.section(QStringLiteral(" Version"), 0, 0);

    // 列一：系统。
    QVBoxLayout* systemCol = makeColumn(QStringLiteral("系统"));
    addKv(systemCol, content, QStringLiteral("操作系统"), osShort);
    addKv(systemCol, content, QStringLiteral("主机名"), QSysInfo::machineHostName());
    addKv(systemCol, content, QStringLiteral("架构"), QSysInfo::currentCpuArchitecture());
    uptimeKv_ = addKv(systemCol, content, QStringLiteral("运行时长"),
                      formatUptime(sys::uptimeMs()));

    // 列二：网络与内存。
    QVBoxLayout* networkCol = makeColumn(QStringLiteral("网络与内存"));
    addKv(networkCol, content, QStringLiteral("局域网 IP"), lanIPv4());
    addKv(networkCol, content, QStringLiteral("服务绑定"), QStringLiteral("127.0.0.1"))
        ->setToolTip(QStringLiteral("所有服务只绑定回环地址（规则 1），局域网无法直接访问"));
    memoryDetailKv_ = addKv(networkCol, content, QStringLiteral("物理内存"),
                            QStringLiteral("--"));

    // 列三：面板。
    QVBoxLayout* panelCol = makeColumn(QStringLiteral("面板"));
    addKv(panelCol, content, QStringLiteral("版本"), version_);
    addKv(panelCol, content, QStringLiteral("安装目录"),
          QDir::toNativeSeparators(paths::installDir()), Qt::ElideMiddle);
    countsKv_ = addKv(panelCol, content, QStringLiteral("实例与站点"),
                      QStringLiteral("--"));
    addKv(panelCol, content, QStringLiteral("下载缓存"), downloadsCacheText());

    layout->addWidget(content, 1);
    return card;
}

void HomePage::setResourceCounts(int instanceCount, int siteCount) {
    if (countsKv_) {
        static_cast<ElideLabel*>(countsKv_)
            ->setTextFull(QStringLiteral("%1 个实例 · %2 个站点")
                              .arg(instanceCount)
                              .arg(siteCount));
    }
}

QWidget* HomePage::buildLogPage(QWidget* parent) {
    auto* card = ui::makeCard(parent);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(18, 14, 18, 14);
    layout->setSpacing(10);

    auto* head = new QWidget(card);
    auto* headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(0, 0, 0, 0);
    headLayout->setSpacing(8);
    headLayout->addWidget(ui::makeAccentBar(QStringLiteral("accent"), 15, head));
    headLayout->addWidget(
        ui::makeLabel(QStringLiteral("日志文件"), QStringLiteral("GroupTitle"), head));
    headLayout->addStretch();
    headLayout->addWidget(ui::makeLabel(QStringLiteral("最近 200 行 · 每 2 秒自动刷新"),
                                        QStringLiteral("SectionHint"), head));

    auto* openBtn = new QPushButton(QStringLiteral("打开目录"), head);
    openBtn->setObjectName(QStringLiteral("SoftBtn"));
    openBtn->setCursor(Qt::PointingHandCursor);
    connect(openBtn, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(paths::logsDir()));
    });
    headLayout->addWidget(openBtn);
    layout->addWidget(head);

    logView_ = new QPlainTextEdit(card);
    logView_->setObjectName(QStringLiteral("LogView"));
    logView_->setReadOnly(true);
    logView_->setMaximumBlockCount(500);    // 防止长时间运行内存膨胀
    // QPlainTextEdit 默认想要十几行高，会把底部区域顶得比「运行状态/资源信息」两个
    // 页签高出一截；卡到 110px 让三个页签的卡片同高。
    logView_->setMaximumHeight(110);
    layout->addWidget(logView_);

    logTimer_ = new QTimer(this);
    logTimer_->setInterval(2000);
    connect(logTimer_, &QTimer::timeout, this, &HomePage::refreshLogView);
    logTimer_->start();
    refreshLogView();
    return card;
}

void HomePage::refreshLogView() {
    if (!logView_ || !logView_->isVisible()) return;    // 不切到这页就不读盘
    const text::Tail tail =
        text::readTail(paths::logsDir() + QStringLiteral("/devdatapod.log"), 200);
    const QString content = tail.text.trimmed();
    if (content == lastLogText_) return;
    lastLogText_ = content;
    // 只有本来就停在底部才跟着滚，用户往上翻时不抢滚动条。
    const QScrollBar* bar = logView_->verticalScrollBar();
    const bool atBottom = bar->value() >= bar->maximum() - 4;
    logView_->setPlainText(content.isEmpty() ? QStringLiteral("（还没有日志）") : content);
    if (atBottom) logView_->moveCursor(QTextCursor::End);
}

} // namespace devdatapod
