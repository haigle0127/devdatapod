#include "ui/widgets/GaugeGroup.h"

#include "core/util/Log.h"
#include "ui/theme/Theme.h"
#include "ui/widgets/UiFactory.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickWidget>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QVBoxLayout>

namespace devdatapod {

namespace {
constexpr int kDial = 84;
constexpr int kGaugeSpacing = 30;
constexpr int kGaugeRowHeight = 110;

QVariantList toModel(const QVector<GaugeGroup::Gauge>& gauges) {
    QVariantList list;
    for (const GaugeGroup::Gauge& gauge : gauges) {
        list.append(QVariantMap{
            {QStringLiteral("label"), gauge.label},
            {QStringLiteral("value"), gauge.value},
            {QStringLiteral("fraction"), gauge.fraction},
            {QStringLiteral("colorKey"), gauge.colorKey},
        });
    }
    return list;
}
}

GaugeGroup::GaugeGroup(const QString& title,
                       const QString& barTone,
                       const QVector<Gauge>& gauges,
                       QWidget* parent)
    : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(14);

    auto* head = new QWidget(this);
    head->setObjectName(QStringLiteral("GroupHead"));
    auto* headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(0, 0, 0, 0);
    headLayout->setSpacing(8);
    headLayout->addWidget(ui::makeAccentBar(barTone, 15, head));
    headLayout->addWidget(ui::makeLabel(title, QStringLiteral("GroupTitle"), head));

    view_ = new QQuickWidget(this);
    view_->setResizeMode(QQuickWidget::SizeRootObjectToView);
    view_->setClearColor(Theme::instance().color(QStringLiteral("card")));
    if (QObject* tokens =
            Theme::instance().createTokenObject(view_->engine(), view_)) {
        view_->rootContext()->setContextProperty(QStringLiteral("Tokens"), tokens);
    }
    view_->rootContext()->setContextProperty(QStringLiteral("gauges"), toModel(gauges));
    view_->setSource(QUrl(QStringLiteral("qrc:/devdatapod/ui/qml/Gauges.qml")));
    if (view_->status() == QQuickWidget::Error) {
        for (const QQmlError& error : view_->errors()) {
            log::error(QStringLiteral("Gauges.qml: %1").arg(error.toString()));
        }
    }
    const int count = static_cast<int>(gauges.size());
    view_->setFixedSize(count * kDial + qMax(0, count - 1) * kGaugeSpacing, kGaugeRowHeight);

    layout->addWidget(head);
    layout->addWidget(view_);
}

void GaugeGroup::setGauges(const QVector<Gauge>& gauges) {
    if (!view_) return;
    view_->rootContext()->setContextProperty(QStringLiteral("gauges"), toModel(gauges));
    const int count = static_cast<int>(gauges.size());
    view_->setFixedSize(count * kDial + qMax(0, count - 1) * kGaugeSpacing, kGaugeRowHeight);
}

}
