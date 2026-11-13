#include "ui/widgets/TitleBar.h"

#include "ui/theme/Theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QSvgRenderer>
#include <QWindow>

namespace devdatapod {

namespace {
constexpr int kHeight = 46;
constexpr int kControlSize = 46;
constexpr int kGlyphSize = 15;
constexpr int kLogoSize = 22;

// 标题栏左侧的 logo：从 qrc 里的 SVG 渲染成 pixmap，缩放走 SmoothTransformation。
QPixmap logoPixmap(int size) {
    QSvgRenderer renderer(QStringLiteral(":/devdatapod/assets/logo.svg"));
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    renderer.render(&painter, QRectF(0, 0, size, size));
    return pixmap;
}
}

TitleBar::TitleBar(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("TitleBar"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedHeight(kHeight);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(16, 0, 0, 0);
    layout->setSpacing(0);

    auto* identity = new QWidget(this);
    identity->setObjectName(QStringLiteral("AppIdentity"));
    auto* identityLayout = new QHBoxLayout(identity);
    identityLayout->setContentsMargins(0, 0, 0, 0);
    identityLayout->setSpacing(10);

    auto* logo = new QLabel(identity);
    logo->setPixmap(logoPixmap(kLogoSize));
    auto* name = new QLabel(QStringLiteral("devdatapod"), identity);
    name->setObjectName(QStringLiteral("AppTitle"));
    auto* domain = new QLabel(QStringLiteral("本地开发服务面板"), identity);
    domain->setObjectName(QStringLiteral("AppDomain"));
    identityLayout->addWidget(logo);
    identityLayout->addWidget(name);
    identityLayout->addWidget(domain);

    auto* controls = new QWidget(this);
    controls->setObjectName(QStringLiteral("WindowControls"));
    auto* controlsLayout = new QHBoxLayout(controls);
    controlsLayout->setContentsMargins(0, 0, 0, 0);
    controlsLayout->setSpacing(0);

    WindowControlButton* minimize = makeControl(Glyph::Minus, "WindowBtn");
    maximizeButton_ = makeControl(Glyph::Square, "WindowBtn");
    WindowControlButton* close = makeControl(Glyph::Close, "WindowBtnClose");
    close->setHoverColor(Theme::instance().color(QStringLiteral("card")));
    minimize->setToolTip(QStringLiteral("最小化"));
    maximizeButton_->setToolTip(QStringLiteral("最大化"));
    close->setToolTip(QStringLiteral("关闭"));

    connect(minimize, &QPushButton::clicked, this, &TitleBar::minimizeRequested);
    connect(maximizeButton_, &QPushButton::clicked, this, &TitleBar::toggleMaximizeRequested);
    connect(close, &QPushButton::clicked, this, &TitleBar::closeRequested);

    controlsLayout->addWidget(minimize);
    controlsLayout->addWidget(maximizeButton_);
    controlsLayout->addWidget(close);

    layout->addWidget(identity);
    layout->addStretch();
    layout->addWidget(controls);
}

WindowControlButton* TitleBar::makeControl(Glyph glyph, const char* objectName) {
    auto* button = new WindowControlButton(glyph, kGlyphSize, this);
    button->setObjectName(QString::fromLatin1(objectName));
    button->setFixedSize(kControlSize, kControlSize);
    button->setCursor(Qt::ArrowCursor);
    return button;
}

void TitleBar::setMaximized(bool maximized) {
    if (!maximizeButton_) return;
    maximizeButton_->setGlyph(maximized ? Glyph::Restore : Glyph::Square);
    maximizeButton_->setToolTip(maximized ? QStringLiteral("还原") : QStringLiteral("最大化"));
}

void TitleBar::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && window() && window()->windowHandle()) {
        window()->windowHandle()->startSystemMove();
    }
    QWidget::mousePressEvent(event);
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) emit toggleMaximizeRequested();
    QWidget::mouseDoubleClickEvent(event);
}

}
