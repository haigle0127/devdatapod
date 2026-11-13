#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace devdatapod {

// 一个「网站」= 一条 nginx vhost：域名 + 端口 + 站点根目录。
//
// 只做部署，不碰 PHP / 数据库 / FTP。截图里那几项（程序类型、PHP 版本、创建环境）是
// phpStudy 的捆绑玩法，这里刻意不要：解释器由 kind 单独管，站点只管把一个目录挂到
// 127.0.0.1 的某个端口上（AGENTS.md 规则 1：面板自己的端口全在 30000-32000，
// 但站点端口是用户网站自己的，不受那条约束）。
struct Site {
    QString domain;        // 主域名，同时也是这条记录的唯一键（小写）
    QStringList aliases;   // 第二域名，一并写进 server_name
    int port = 0;          // 监听端口；站点之间、以及与 web 服务自身端口都不能撞
    QString rootDir;       // 站点根目录，存绝对路径
    QString expireAt;      // 到期日，自由文本（空 = 长期）
    QString remark;
};

namespace sites {

// <安装根>/data/sites.json
QString filePath();

// 读全部站点。文件不存在 = 没有站点（不是错误）；解析失败时 *problem 带原因。
QVector<Site> load(QString* problem = nullptr);

// 原子写回。纯数据，不碰 nginx。
bool save(const QVector<Site>& items, QString* error = nullptr);

// 校验一条记录：域名/第二域名格式、端口范围、根目录非空，以及「不能和别的站点撞域名、
// 撞别名、撞端口」。self 是被编辑的原记录（新建传 nullptr），用来放过「域名/端口没改」。
bool validate(const Site& site, const Site* self, const QVector<Site>& existing, QString* error);

// 域名 → 默认站点目录：<安装根>/WWW/<域名>。
QString defaultRootDir(const QString& domain);

// 建站点目录；目录里没有 index.* 时放一页占位，创建完就能直接访问。
bool prepareRoot(const Site& site, QString* error);

// 按域名找，找不到返回 nullptr。
const Site* find(const QVector<Site>& items, const QString& domain);

} // namespace sites
} // namespace devdatapod
