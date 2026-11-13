#include "ui/widgets/pages/PlaceholderPage.h"

#include "ui/widgets/UiFactory.h"

#include <QLabel>
#include <QVBoxLayout>

namespace devdatapod {

PlaceholderPage::PlaceholderPage(const QString& title, QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 18, 0, 0);

    auto* card = ui::makeCard(this);
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(18, 14, 18, 14);
    cardLayout->addWidget(ui::makeLabel(title, QStringLiteral("SectionTitle"), card));
    cardLayout->addStretch();
    cardLayout->addWidget(ui::makeLabel(
        QStringLiteral("此页面按 docs/DESIGN-SPEC.md 的分期补上，当前只提供外壳。"),
        QStringLiteral("EmptyHint"), card));
    cardLayout->addStretch();

    layout->addWidget(card, 1);
}

}
