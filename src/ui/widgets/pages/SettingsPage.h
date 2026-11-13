#pragma once

#include <QWidget>

class QLabel;
class QNetworkAccessManager;
class QPushButton;
class QVBoxLayout;

namespace devdatapod {

class ToggleSwitch;

// 设置页：面板级偏好设置。
// 目前两项：编辑配置后是否自动重启服务（默认开）；检查更新（查 GitHub Releases，
// 按 devdatapod-x.y.z.exe 的版本号与当前 DEVDATAPOD_VERSION 比大小）。
class SettingsPage : public QWidget {
    Q_OBJECT
public:
    explicit SettingsPage(QWidget* parent = nullptr);

private:
    void buildUi();
    void loadSettings();
    void saveSettings();
    void checkUpdate();

    ToggleSwitch* autoRestartSwitch_ = nullptr;
    QLabel* savedHint_ = nullptr;
    QPushButton* checkUpdateBtn_ = nullptr;
    QLabel* updateStatus_ = nullptr;
    QNetworkAccessManager* nam_ = nullptr;
};

} // namespace devdatapod
