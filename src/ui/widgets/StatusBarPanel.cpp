#include "ui/widgets/StatusBarPanel.h"

#include "ui/theme/IconFactory.h"
#include "ui/theme/Theme.h"
#include "ui/widgets/UiFactory.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QPixmap>

namespace devdatapod {

namespace {
constexpr int kHeight = 36;
constexpr int kGap = 18;
constexpr int kChipGap = 8;
constexpr int kDotSize = 8;
}

StatusBarPanel::StatusBarPanel(const QString& version, QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("StatusBar"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedHeight(kHeight);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(14, 0, 14, 0);
    layout->setSpacing(kGap);

    chipRow_ = new QHBoxLayout;
    chipRow_->setContentsMargins(0, 0, 0, 0);
    chipRow_->setSpacing(kGap);
    layout->addLayout(chipRow_);
    layout->addStretch();

    auto* info = new QLabel(this);
    info->setPixmap(icons::pixmap(Glyph::Info, Theme::instance().color(QStringLiteral("ink-2")), 13));
    versionLabel_ =
        ui::makeLabel(QStringLiteral("版本：") + version, QStringLiteral("StatusText"), this);
    layout->addWidget(info);
    layout->addWidget(versionLabel_);
}

void StatusBarPanel::setServices(const QVector<ServiceStatusItem>& items) {
    while (QLayoutItem* item = chipRow_->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    for (const ServiceStatusItem& item : items) {
        auto* chip = new QWidget(this);
        chip->setObjectName(QStringLiteral("StatusChip"));
        auto* chipLayout = new QHBoxLayout(chip);
        chipLayout->setContentsMargins(0, 0, 0, 0);
        chipLayout->setSpacing(kChipGap);
        chipLayout->addWidget(makeDot(item.state, chip, kDotSize));
        chipLayout->addWidget(
            ui::makeLabel(item.name, QStringLiteral("StatusText"), chip));
        chipRow_->addWidget(chip);
    }
}

}
