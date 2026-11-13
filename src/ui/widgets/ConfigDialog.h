#pragma once

#include "core/service/ServiceInstance.h"

#include <QDialog>

class QLineEdit;
class QSpinBox;
class QLabel;
class QPushButton;
class QVBoxLayout;
class QHBoxLayout;

namespace devdatapod {

class Engine;

// 实例配置弹窗：改端口 + 改组件特有设置。
// 样式和 ConfirmDialog 一致：无边框圆角卡片 + 半透明遮罩。
// 点确定后：保存设置 → 应用端口变更 → （可选）重启服务。
// 重启成功才关弹窗；失败留在弹窗里继续改。
class ConfigDialog : public QDialog {
    Q_OBJECT
public:
    ConfigDialog(Engine* engine, const QString& instanceId, QWidget* parent = nullptr);

private:
    void buildUi();
    void buildPortSection(QVBoxLayout* layout);
    void buildRedisFields(QVBoxLayout* layout);
    void buildMysqlFields(QVBoxLayout* layout);
    void buildNginxFields(QVBoxLayout* layout);
    void loadCurrentValues();
    bool validateInputs(QString* error);
    bool applyChanges(QString* error);

    Engine* engine_;
    QString instanceId_;
    ServiceInstance instance_;
    int currentPort_ = 0;
    bool isDefault_ = false;

    // 端口
    QSpinBox* portSpin_ = nullptr;
    QLabel* starLabel_ = nullptr;       // 默认实例的星星标记

    // Redis 字段
    QLineEdit* redisPassword_ = nullptr;
    QSpinBox* redisMaxMemory_ = nullptr;
    QSpinBox* redisMaxClients_ = nullptr;
    QSpinBox* redisDatabases_ = nullptr;

    // MySQL 字段
    QLineEdit* mysqlRootPassword_ = nullptr;
    QSpinBox* mysqlMaxConnections_ = nullptr;
    QSpinBox* mysqlInnodbPool_ = nullptr;
    QLineEdit* mysqlCharset_ = nullptr;

    // Nginx 字段
    QSpinBox* nginxWorkers_ = nullptr;
    QSpinBox* nginxConnections_ = nullptr;
    QLineEdit* nginxRootDir_ = nullptr;

    QPushButton* confirmBtn_ = nullptr;
    QPushButton* cancelBtn_ = nullptr;
    QLabel* errorLabel_ = nullptr;
    QString lastError_;
};

} // namespace devdatapod
