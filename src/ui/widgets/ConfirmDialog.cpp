#include "ui/widgets/ConfirmDialog.h"

#include "ui/theme/Theme.h"

#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace devdatapod {

ConfirmDialog::ConfirmDialog(const QString& title, const QString& message,
                             const QString& confirmText, bool danger, QWidget* parent)
    : QDialog(parent) {
    // Frameless + translucent so the card can have rounded corners; the dimmed backdrop is
    // painted in paintEvent via the stylesheet on this dialog itself.
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setModal(true);
    // 内容自适应宽度会显得挤（提示类弹窗只有一行字），给个下限，长消息仍然能撑更高。
    setMinimumWidth(320);

    Theme& theme = Theme::instance();

    auto* card = new QWidget(this);
    card->setObjectName(QStringLiteral("ConfirmCard"));

    auto* shadow = new QGraphicsDropShadowEffect(card);
    shadow->setBlurRadius(28);
    shadow->setOffset(0, 6);
    shadow->setColor(QColor(38, 32, 64, 60));
    card->setGraphicsEffect(shadow);

    titleLabel_ = new QLabel(title, card);
    titleLabel_->setObjectName(QStringLiteral("ConfirmTitle"));

    messageLabel_ = new QLabel(message, card);
    messageLabel_->setObjectName(QStringLiteral("ConfirmBody"));
    messageLabel_->setWordWrap(true);

    cancelButton_ = new QPushButton(QStringLiteral("取消"), card);
    cancelButton_->setObjectName(QStringLiteral("SoftBtn"));
    cancelButton_->setCursor(Qt::PointingHandCursor);

    confirmButton_ = new QPushButton(confirmText, card);
    confirmButton_->setObjectName(danger ? QStringLiteral("DangerBtn")
                                         : QStringLiteral("PrimaryBtn"));
    confirmButton_->setCursor(Qt::PointingHandCursor);
    confirmButton_->setDefault(true);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(cancelButton_);
    buttons->addWidget(confirmButton_);

    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(22, 20, 22, 18);
    cardLayout->setSpacing(12);
    cardLayout->addWidget(titleLabel_);
    cardLayout->addWidget(messageLabel_);
    cardLayout->addSpacing(6);
    cardLayout->addLayout(buttons);

    // The outer layout leaves a margin so the drop shadow is not clipped.
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 20);
    root->addWidget(card);

    // Colors come straight from tokens; the card itself is styled here because a frameless
    // dialog is not reached by the app-wide window QSS selectors.
    card->setStyleSheet(QStringLiteral(
        "#ConfirmCard { background: %1; border-radius: 12px; }"
        "#ConfirmTitle { color: %2; font-size: 15px; font-weight: 700; }"
        "#ConfirmBody { color: %3; font-size: 13px; line-height: 1.5; }")
                            .arg(theme.color(QStringLiteral("card")).name(),
                                 theme.color(QStringLiteral("ink")).name(),
                                 theme.color(QStringLiteral("ink-2")).name()));

    connect(confirmButton_, &QPushButton::clicked, this, &QDialog::accept);
    connect(cancelButton_, &QPushButton::clicked, this, &QDialog::reject);
}

bool ConfirmDialog::ask(QWidget* parent, const QString& title, const QString& message,
                        const QString& confirmText, bool danger) {
    ConfirmDialog dialog(title, message,
                         confirmText.isEmpty() ? QStringLiteral("确认") : confirmText,
                         danger, parent);
    return dialog.exec() == QDialog::Accepted;
}

void ConfirmDialog::notice(QWidget* parent, const QString& title, const QString& message) {
    ConfirmDialog dialog(title, message, QStringLiteral("知道了"), false, parent);
    dialog.cancelButton_->hide();
    dialog.exec();
}

}
