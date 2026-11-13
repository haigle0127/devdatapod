#include "core/service/ComponentCatalog.h"

namespace devdatapod {

QVector<Component> componentCatalog() {
    // 官方直链 + 官方 Windows zip。文件名后缀决定解压方式；nginx 包里有顶层目录，
    // 解压器按 nginx.exe 定位再摊平。
    return {
        {QStringLiteral("nginx-1.30.6"), QStringLiteral("nginx"), QStringLiteral("Nginx 1.30.6"),
         QStringLiteral("1.30.6"), QStringLiteral("高性能 HTTP 服务器与反向代理，官方 Windows 版"),
         QStringLiteral("https://nginx.org/download/nginx-1.30.6.zip"),
         QStringLiteral("nginx-1.30.6.zip"), 2773293},

        // 四个 redis-windows 版本，GitHub releases 原始地址。文件名后缀决定解压方式。
        // 注意 5.0.7 是 tar.gz 且 redis-server.exe 在壳内的 redis5/ 子目录，解压器会递归定位。
        {QStringLiteral("redis-8.10.1"), QStringLiteral("redis"), QStringLiteral("Redis 8.10.1"),
         QStringLiteral("8.10.1"), QStringLiteral("内存键值数据库，Windows msys2 移植版"),
         QStringLiteral("https://github.com/redis-windows/redis-windows/"
                        "releases/download/8.10.1/Redis-8.10.1-Windows-x64-msys2-with-Service.zip"),
         QStringLiteral("Redis-8.10.1.zip"), 20341313},

        {QStringLiteral("redis-7.4.11"), QStringLiteral("redis"), QStringLiteral("Redis 7.4.11"),
         QStringLiteral("7.4.11"), QStringLiteral("内存键值数据库，Windows msys2 移植版"),
         QStringLiteral("https://github.com/redis-windows/redis-windows/"
                        "releases/download/7.4.11/Redis-7.4.11-Windows-x64-msys2-with-Service.zip"),
         QStringLiteral("Redis-7.4.11.zip"), 17904389},

        {QStringLiteral("redis-6.2.24"), QStringLiteral("redis"), QStringLiteral("Redis 6.2.24"),
         QStringLiteral("6.2.24"), QStringLiteral("内存键值数据库，Windows msys2 移植版"),
         QStringLiteral("https://github.com/redis-windows/redis-windows/"
                        "releases/download/6.2.24/Redis-6.2.24-Windows-x64-msys2-with-Service.zip"),
         QStringLiteral("Redis-6.2.24.zip"), 17988288},

        {QStringLiteral("redis-5.0.7"), QStringLiteral("redis"), QStringLiteral("Redis 5.0.7"),
         QStringLiteral("5.0.7"), QStringLiteral("内存键值数据库，Windows cygwin 移植版"),
         QStringLiteral("https://github.com/redis-windows/redis-windows/"
                        "releases/download/5.0.7/Redis-5.0.7-Windows-x64.tar.gz"),
         QStringLiteral("Redis-5.0.7.tar.gz"), 8723636},

        // MySQL 官方 CDN 直链。zip 有顶层目录，解压后需摊平到 Extensions/MySQL<version>/。
        {QStringLiteral("mysql-8.0.46"), QStringLiteral("mysql"), QStringLiteral("MySQL 8.0.46"),
         QStringLiteral("8.0.46"), QStringLiteral("关系型数据库，主流稳定版"),
         QStringLiteral("https://cdn.mysql.com//Downloads/MySQL-8.0/mysql-8.0.46-winx64.zip"),
         QStringLiteral("mysql-8.0.46-winx64.zip"), 248009349},

        {QStringLiteral("mysql-8.4.11"), QStringLiteral("mysql"), QStringLiteral("MySQL 8.4.11"),
         QStringLiteral("8.4.11"), QStringLiteral("关系型数据库，LTS 长期支持版"),
         QStringLiteral("https://cdn.mysql.com//Downloads/MySQL-8.4/mysql-8.4.11-winx64.zip"),
         QStringLiteral("mysql-8.4.11-winx64.zip"), 281191914},

        {QStringLiteral("mysql-9.7.2"), QStringLiteral("mysql"), QStringLiteral("MySQL 9.7.2"),
         QStringLiteral("9.7.2"), QStringLiteral("关系型数据库，最新创新版"),
         QStringLiteral("https://cdn.mysql.com//Downloads/MySQL-9.7/mysql-9.7.2-winx64.zip"),
         QStringLiteral("mysql-9.7.2-winx64.zip"), 342737674},

        {QStringLiteral("mysql-5.7.44"), QStringLiteral("mysql"), QStringLiteral("MySQL 5.7.44"),
         QStringLiteral("5.7.44"), QStringLiteral("关系型数据库，经典老版本"),
         QStringLiteral("https://cdn.mysql.com/archives/mysql-5.7/mysql-5.7.44-winx64.zip"),
         QStringLiteral("mysql-5.7.44-winx64.zip"), 352891656},
    };
}

} // namespace devdatapod
