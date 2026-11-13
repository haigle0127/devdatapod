#include "ui/widgets/pages/SoftwarePage.h"

#include "core/service/Engine.h"
#include "core/service/InstanceRegistry.h"
#include "ui/theme/Theme.h"
#include "ui/widgets/UiFactory.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace devdatapod {

namespace {
constexpr int kNameWidth = 150;    // 和首页套件列表对齐，名称列固定宽

QString formatSize(qint64 bytes) {
    if (bytes <= 0) return QString();
    const double mb = bytes / 1048576.0;
    return QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
}
}

SoftwarePage::SoftwarePage(Engine* engine, QWidget* parent) : QWidget(parent), engine_(engine) {
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
    headLayout->addWidget(ui::makeAccentBar(QStringLiteral("accent"), 18, head));
    headLayout->addWidget(ui::makeLabel(QStringLiteral("软件管理"),
                                        QStringLiteral("SectionTitle"), head));
    headLayout->addStretch();
    headLayout->addWidget(ui::makeLabel(QStringLiteral("点击下载，装好后会出现在首页套件列表"),
                                        QStringLiteral("SectionHint"), head));
    cardLayout->addWidget(head);

    // 组件多了会挤，行容器放进滚动区：头部固定，列表上下滑。
    auto* scroll = new QScrollArea(card);
    scroll->setObjectName(QStringLiteral("SuiteScroll"));    // 复用滚动条样式
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

    rebuild();

    listLayout_->addStretch();
    layout->addWidget(card, 1);
}

void SoftwarePage::rebuild() {
    // 已安装的 kind+version 集合，用来把按钮换成「已安装」。
    QSet<QString> installed;
    if (engine_) {
        for (const ServiceInstance& inst : engine_->instances()) {
            installed.insert(inst.kind + QLatin1Char(':') + inst.version);
        }
    }

    for (const Component& c : componentCatalog()) {
        auto* row = new QWidget(this);
        row->setObjectName(QStringLiteral("ServiceRow"));    // 复用行底部分隔线样式
        row->setAttribute(Qt::WA_StyledBackground, true);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(4, 8, 4, 8);
        rowLayout->setSpacing(12);

        auto* name = ui::makeLabel(c.name, QStringLiteral("SvcName"), row);
        name->setFixedWidth(kNameWidth);

        const QString sizeText = formatSize(c.sizeBytes);
        auto* size = ui::makeLabel(sizeText.isEmpty() ? QStringLiteral("—") : sizeText,
                                   QStringLiteral("PortTag"), row);
        size->setFixedWidth(86);
        size->setAlignment(Qt::AlignCenter);

        auto* desc = ui::makeLabel(c.description, QStringLiteral("EmptyHint"), row);

        auto* stage = ui::makeLabel(QString(), QStringLiteral("SectionHint"), row);
        stageLabels_.insert(c.id, stage);

        auto* button = new QPushButton(QStringLiteral("下载"), row);
        button->setObjectName(QStringLiteral("PrimaryBtn"));
        button->setCursor(Qt::PointingHandCursor);
        buttons_.insert(c.id, button);

        const bool isInstalled = installed.contains(c.kind + QLatin1Char(':') + c.version);
        if (isInstalled) {
            button->setText(QStringLiteral("已安装"));
            button->setEnabled(false);
        } else {
            const Component component = c;
            connect(button, &QPushButton::clicked, this,
                    [this, component] { emit installRequested(component); });
        }

        rowLayout->addWidget(name);
        rowLayout->addWidget(size);
        rowLayout->addWidget(desc, 1);
        rowLayout->addWidget(stage);
        rowLayout->addWidget(button);
        if (isInstalled) {
            auto* uninstallBtn = new QPushButton(QStringLiteral("卸载"), row);
            uninstallBtn->setObjectName(QStringLiteral("DangerBtn"));
            uninstallBtn->setCursor(Qt::PointingHandCursor);
            const Component component = c;
            connect(uninstallBtn, &QPushButton::clicked, this,
                    [this, component] { emit uninstallRequested(component); });
            rowLayout->addWidget(uninstallBtn);
        }
        listLayout_->addWidget(row);
    }
}

void SoftwarePage::reload() {
    // 全部清掉（包括 stretch），重建后再补 stretch，否则新行排在 stretch 后面被挤到下面。
    QLayoutItem* child;
    while ((child = listLayout_->takeAt(0)) != nullptr) {
        if (child->widget()) delete child->widget();
        delete child;
    }
    stageLabels_.clear();
    buttons_.clear();
    rebuild();
    listLayout_->addStretch();
}

void SoftwarePage::setRowStage(const QString& componentId, const QString& stage) {
    if (QLabel* label = stageLabels_.value(componentId, nullptr)) {
        label->setText(stage);
    }
    if (QPushButton* button = buttons_.value(componentId, nullptr)) {
        button->setEnabled(false);    // 下载中禁止重复点
    }
}

void SoftwarePage::setRowProgress(const QString& componentId, qint64 received, qint64 total) {
    QLabel* label = stageLabels_.value(componentId, nullptr);
    if (!label) return;
    if (total > 0) {
        const int percent = static_cast<int>(received * 100 / total);
        label->setText(QStringLiteral("下载中 %1%").arg(percent));
    } else {
        label->setText(QStringLiteral("下载中 %1").arg(formatSize(received)));
    }
}

void SoftwarePage::setRowFinished(const QString& componentId, bool ok, const QString& error) {
    QLabel* label = stageLabels_.value(componentId, nullptr);
    QPushButton* button = buttons_.value(componentId, nullptr);
    if (ok) {
        if (label) label->setText(QStringLiteral("已安装"));
        if (button) {
            button->setText(QStringLiteral("已安装"));
            button->setEnabled(false);
        }
    } else {
        if (label) label->setText(error);
        if (button) {
            button->setText(QStringLiteral("重试"));
            button->setEnabled(true);
        }
    }
}

} // namespace devdatapod
