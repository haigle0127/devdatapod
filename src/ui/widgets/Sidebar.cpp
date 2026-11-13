#include "ui/widgets/Sidebar.h"

#include "ui/theme/Theme.h"
#include "ui/widgets/UiFactory.h"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPixmap>
#include <QVBoxLayout>

namespace devdatapod {

namespace {
constexpr int kWidth = 216;
constexpr int kGlyphSize = 17;

const struct {
    const char* text;
    Glyph glyph;
} kPages[] = {
    {"首页", Glyph::House},
    {"网站", Glyph::Globe},
    {"软件管理", Glyph::Package},
    {"设置", Glyph::Settings},
};
}

Sidebar::Sidebar(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("Sidebar"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedWidth(kWidth);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 22, 14, 18);
    layout->setSpacing(4);

    auto* brand = new QWidget(this);
    brand->setObjectName(QStringLiteral("SidebarBrand"));
    auto* brandLayout = new QVBoxLayout(brand);
    brandLayout->setContentsMargins(10, 0, 10, 18);
    brandLayout->setSpacing(2);
    brandLayout->addWidget(ui::makeLabel(QStringLiteral("devdatapod"), QStringLiteral("Brand")));
    brandLayout->addWidget(
        ui::makeLabel(QStringLiteral("HAIGLE.CN"), QStringLiteral("BrandSub")));

    layout->addWidget(brand);

    group_ = new QButtonGroup(this);
    group_->setExclusive(true);
    int index = 0;
    for (const auto& page : kPages) {
        QPushButton* button = addButton(QString::fromUtf8(page.text), page.glyph);
        group_->addButton(button, index++);
        layout->addWidget(button);
    }
    connect(group_, &QButtonGroup::idClicked, this, &Sidebar::pageSelected);
    group_->button(0)->setChecked(true);

    auto* promo = new QWidget(this);
    promo->setObjectName(QStringLiteral("PromoCard"));
    promo->setAttribute(Qt::WA_StyledBackground, true);
    auto* promoLayout = new QVBoxLayout(promo);
    promoLayout->setContentsMargins(14, 14, 14, 14);
    promoLayout->setSpacing(6);
    promoLayout->addWidget(
        ui::makeLabel(QStringLiteral("devdatapod 面板"), QStringLiteral("PromoTitle")));
    auto* promoSub =
        ui::makeLabel(QStringLiteral("简单的数据服务集成环境"), QStringLiteral("PromoSub"));
    // 新文案比旧的长，卡片宽度固定，超了自动折到第二行而不是溢出。
    promoSub->setWordWrap(true);
    promoLayout->addWidget(promoSub);

    layout->addStretch();
    layout->addWidget(promo);
}

QPushButton* Sidebar::addButton(const QString& text, Glyph glyph) {
    Theme& theme = Theme::instance();
    auto* button = new QPushButton(text, this);
    button->setObjectName(QStringLiteral("NavBtn"));
    button->setCheckable(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);

    // QPushButton cannot retint an icon through QSS, so the two states are baked from
    // the same tokens: dim while idle, white while checked.
    const QSize iconSize(kGlyphSize, kGlyphSize);
    QIcon icon;
    icon.addPixmap(icons::pixmap(glyph, theme.color(QStringLiteral("sidebar-text")), kGlyphSize),
                   QIcon::Normal, QIcon::Off);
    icon.addPixmap(icons::pixmap(glyph, theme.color(QStringLiteral("card")), kGlyphSize),
                   QIcon::Normal, QIcon::On);
    button->setIcon(icon);
    button->setIconSize(iconSize);
    return button;
}

void Sidebar::setCurrentPage(int index) {
    if (QAbstractButton* button = group_->button(index)) button->setChecked(true);
}

}
