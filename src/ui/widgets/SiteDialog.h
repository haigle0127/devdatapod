#pragma once

#include <QDialog>

class QLineEdit;
class QSpinBox;
class QLabel;
class QPushButton;
class QRadioButton;

namespace devdatapod {

class Engine;
class Site;

// 创建 / 修改一个网站。字段对齐截图的「基本配置」：域名、第二域名、端口（http/https 单选）、
// 根目录（带浏览）、到期日期、备注。PHP 类型 / PHP 版本 / 创建环境（FTP、数据库、同步
// hosts、生产环境）按需求不做——这个弹窗就是单纯把目录挂到 127.0.0.1 的一个端口上。
//
// https 的证书属于 P6，所以 https 选项目前置灰并给提示，模型里也不存 scheme。
class SiteDialog : public QDialog {
    Q_OBJECT
public:
    // domain 为空 = 新建；否则编辑这个域名对应的站点。
    SiteDialog(Engine* engine, const QString& domain, QWidget* parent = nullptr);

private:
    void buildUi();
    void fillFrom(const Site& site);
    Site collect() const;
    bool apply(QString* error);     // 校验 + 建目录 + 写 sites.json
    void browseRoot();
    void confirmClicked();
    void deleteClicked();

    Engine* engine_;
    QString originalDomain_;        // 空 = 新建
    int originalPort_ = 0;          // 编辑时的旧端口，用来放行「端口没变但已被自己占用」
    bool editing_ = false;

    QLineEdit* domainEdit_ = nullptr;
    QLineEdit* aliasEdit_ = nullptr;
    QRadioButton* httpRadio_ = nullptr;
    QRadioButton* httpsRadio_ = nullptr;
    QSpinBox* portSpin_ = nullptr;
    QLineEdit* rootEdit_ = nullptr;
    QLineEdit* expireEdit_ = nullptr;
    QLineEdit* remarkEdit_ = nullptr;
    QPushButton* deleteBtn_ = nullptr;
    QLabel* errorLabel_ = nullptr;
};

} // namespace devdatapod
