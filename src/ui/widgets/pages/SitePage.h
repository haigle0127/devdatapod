#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;
class QVBoxLayout;

namespace devdatapod {

class Engine;

// 网站页：一个站点表（域名 / 端口 / 物理路径 / 状态 / 到期 / 操作），
// 头部是「创建网站」+ 查找框。布局对齐截图，配色和行样式用现有的 ServiceRow 那一套。
class SitePage : public QWidget {
    Q_OBJECT
public:
    explicit SitePage(Engine* engine, QWidget* parent = nullptr);

public slots:
    void reload();

signals:
    // MainWindow 接住后弹创建 / 编辑弹窗（要挡「web 服务没启动」的提示，得握 engine）。
    void createRequested();
    void manageRequested(const QString& domain);

private:
    void rebuild();
    bool webRunning() const;

    Engine* engine_;
    QLineEdit* search_;
    QLabel* emptyHint_;
    QVBoxLayout* listLayout_;
};

} // namespace devdatapod
