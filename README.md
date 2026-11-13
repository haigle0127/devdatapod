# devdatapod

绿色便携的本地开发服务面板 —— 集成 Nginx / MySQL(MariaDB) / Redis / PHP / Python 等常用服务，思路源自小皮面板（phpStudy），Qt 6 实现，Windows 先行，macOS 规划中。

## 功能

- **服务管理**：Nginx / MySQL / Redis 一键启停、重启、实例配置；多版本实例共存，每类至多一个「默认」实例拿规范端口（80 / 3306 / 6379），其余自动走 30000+ 槽位端口，「设为默认」是带回滚的事务切换。
- **网站部署**：创建网站 = 域名 + 端口 + 根目录，Nginx vhost 生成 + `nginx -t` 校验 + 热重载（不断连接），删除站点不删磁盘文件。
- **软件管理**：组件清单下载 → 解压 → 注册一条龙（内置 Redis 5 个版本、MySQL 4 个版本、Nginx 1.30.6），支持卸载。
- **首页**：套件运行状态、系统监控（CPU/内存占用、核心/线程、P 核/E 核拓扑）、资源信息（系统/网络/面板）、面板日志实时 tail。
- **孤儿进程治理**：Windows 下服务全部收进 Job Object（`KILL_ON_JOB_CLOSE`），面板崩溃/被杀不会留下游离的 nginx/mysql/redis。
- **绿色便携**：不写注册表、全相对路径、安装目录支持空格与非中文外字符（`D:\小皮面板\` 也能跑）。
- **配置不静默覆盖**：面板渲染的配置带托管块标记，块外的用户手改内容原样保留。

## 技术栈

Qt 6.8 LTS（Widgets 为主 + 局部 QML 仪表）· CMake + Ninja · C++17 · spdlog。

分层：`src/core/` 服务引擎（驱动 / 端口分配 / 进程监管 / 配置托管块，**不依赖任何 UI 头文件**，selftest 目标机械执法）→ `src/ui/` 界面与主题 → `src/app/` 入口、单实例、托盘。

## 构建

要求：Windows 10+ · Qt 6.8.x `msvc2022_64` · CMake ≥ 3.24 · Ninja · VS Build Tools 2022。

```powershell
# 首次：安装工具链（可重复执行，已完成步骤自动跳过）
scripts\setup-toolchain.cmd

# 构建（Debug）
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1 -Config Debug

# 运行（Qt 运行时注入 PATH；DEVDATAPOD_ROOT 指向仓库根）
$env:PATH = "D:\Qt\6.8.3\msvc2022_64\bin;$env:PATH"
$env:DEVDATAPOD_ROOT = (Get-Location).Path
.\build\devdatapod.exe
```

自检：`.\build\devdatapod-selftest.exe` —— 98 项端口/实例模型不变量，退出码 0 即通过。

## 目录结构

```
src/core/      服务引擎：驱动（Nginx/MySQL/Redis）、PortAllocator、Supervisor、托管块
src/ui/        Widgets 界面、主题 token（单一来源同时喂 QSS 与 QML）
src/app/       程序入口、单实例锁、托盘、selftest / --stage / --run
scripts/       构建脚本与工具链安装
data/          运行期生成：实例注册、端口分配、站点表（已 gitignore）
Extensions/    软件管理下载安装的组件实物（已 gitignore）
```

## 端口约定

- 面板自身端口一律落在 **30000–32000** 且只绑 `127.0.0.1`。
- 规范端口（80/443/3306/6379）**只属于该类的默认实例**，非默认实例走本类槽位段。
- 所有端口由 `data/ports.json` 经 `PortAllocator` 分配，代码里禁止硬编码端口数字。

## 平台

Windows 10 / 11 实测可用；macOS 为下一步目标（计划端口 30080 / 30443，Privileged Helper 留到 v2）。

## 许可证

[MIT](LICENSE) © devdatapod contributors
