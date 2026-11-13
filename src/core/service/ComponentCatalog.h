#pragma once

#include <QMetaType>
#include <QString>
#include <QVector>

namespace devdatapod {

// 软件管理页展示的「可下载组件」。当前只内置 redis 的四个版本，下载地址来自用户给的
//  redis-windows  releases（走 ghproxy 加速）。kind 固定是 "redis"，version 用于注册实例。
struct Component {
    QString id;          // 唯一键，例如 "redis-8.10.1"
    QString kind;        // "redis"
    QString name;        // 显示名，例如 "Redis 8.10.1"
    QString version;     // "8.10.1"
    QString description; // 一行简介
    QString url;         // 下载地址
    QString fileName;    // 下载落盘的文件名（决定解压方式：.zip / .tar.gz）
    qint64 sizeBytes = 0; // 仅用于展示，0 表示未知
};

// 返回内置组件清单。现在是静态表，将来 P5 换成从服务器拉 manifest。
QVector<Component> componentCatalog();

} // namespace devdatapod

// 要能跨线程排队传递（QMetaObject::invokeMethod 的 Q_ARG）。
Q_DECLARE_METATYPE(devdatapod::Component)
