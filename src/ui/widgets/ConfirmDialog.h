#pragma once

#include <QDialog>

class QLabel;
class QPushButton;

namespace devdatapod {

// A token-styled confirm dialog to replace the stock QMessageBox, which does not match the
// devdatapod look. Frameless white card with rounded corners over a dimmed modal backdrop.
//
// Usage:
//   if (ConfirmDialog::ask(this, "设为默认", "……", "设为默认")) { ... }
class ConfirmDialog : public QDialog {
    Q_OBJECT
public:
    // danger = true paints the confirm button in the danger color (destructive actions).
    ConfirmDialog(const QString& title, const QString& message, const QString& confirmText,
                  bool danger, QWidget* parent = nullptr);

    // Returns true when the user confirmed. `confirmText` defaults to 确认.
    static bool ask(QWidget* parent, const QString& title, const QString& message,
                    const QString& confirmText = QString(), bool danger = false);

    // Single-button informational dialog (no cancel). Replaces QMessageBox::warning.
    static void notice(QWidget* parent, const QString& title, const QString& message);

private:
    QLabel* titleLabel_;
    QLabel* messageLabel_;
    QPushButton* confirmButton_;
    QPushButton* cancelButton_;
};

}
