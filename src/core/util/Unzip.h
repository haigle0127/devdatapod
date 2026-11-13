#pragma once

#include <QString>

namespace devdatapod::unpack {

// 把压缩包（.zip 或 .tar.gz）解到 destDir，并把 server 二进制所在的目录摊平成
// destDir 本身——不同包的壳层数不一样，所以统一在解开后递归找 serverBinary，
// 用它所在的目录内容填满 destDir。
//
// serverBinary 例如 "redis-server.exe" / "mysqld.exe"。
// 成功返回 true 且 destDir 下能直接找到 serverBinary；失败带 error。
// 用的是 Windows 10+ 系统自带的 bsdtar（C:\Windows\System32\tar.exe），zip 和 tar.gz 通吃，
// 不引第三方库。调用方负责在工作线程里跑（解压大文件会阻塞）。
bool extract(const QString& archivePath, const QString& destDir,
             const QString& serverBinary, QString* error);

// 兼容旧接口：等价于 extract(archivePath, destDir, "redis-server.exe", error)。
bool extractRedis(const QString& archivePath, const QString& destDir, QString* error);

} // namespace devdatapod::unpack
