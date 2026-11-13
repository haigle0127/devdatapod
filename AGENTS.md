# devdatapod — 项目约定（所有 agent 先读这个）

绿色便携的本地开发服务面板，集成 nginx / MySQL(MariaDB) / Redis / PHP / Python 等，思路源自小皮面板（phpStudy），Qt6 实现，Windows 与 macOS 双平台。

## 真源与参考

| 路径 | 是什么 |
|---|---|
| `D:\phpstudy_pro` | 只读参考实现（Qt5 编译产物）。看目录布局和服务组织方式，**不要修改它** |

## 硬约束（不要违反，违反过 = 返工）

1. **端口**：除服务自身默认端口（80/443、3306、6379）外，devdatapod 自己的端口一律落在 **30000–32000** 且只绑 `127.0.0.1`。上限卡 32767 以下，避免撞上系统动态端口区。规范默认端口**只属于该类的默认实例**，非默认实例一律走本类的高端口槽位段。
2. **端口禁止硬编码**：所有端口从 `data/ports.json` 经 `PortAllocator` 取；端口迁移必须级联重渲染引用方配置（nginx `fastcgi_pass` 等）。
3. **core 与 UI 隔离**：`src/core/**` 不得 include 任何 QtWidgets/QtQuick 头文件。v1.1 要把 core 拆成独立进程，届时只加一层 IPC。
4. **QML 边界**：只有首页资源仪表等动效区用 `QQuickWidget`；表格、表单、对话框、设置页一律 Widgets。主题 token 单一来源同时喂 QSS 和 QML context，禁止两处硬编码颜色。
5. **孤儿进程治理是必须项**：Windows 用 Job Object（`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`），macOS 用进程组 + 启动时按安装目录路径扫描清理。面板崩溃后不能留下 nginx/php 在跑。
6. **配置不能静默覆盖用户手改**：schema + 模板渲染 + 托管块标记；检测到托管块外有改动则降级为纯文本编辑模式。
7. **绿色 = 全相对路径**：不写注册表（除开机自启项）、不写死绝对路径、支持含空格与非 ASCII 的安装目录（`D:\小皮面板\` 必须能跑）。
8. **中文字体**：设计稿的 Funnel Sans / Inter / Geist 都没有 CJK 字形，必须显式配思源黑体 / Noto Sans SC 回退链，否则两平台中文排版不一致。
9. **编码**：`.cmd` / `.ps1` 一律**只用 ASCII** —— Windows PowerShell 5.1 对无 BOM 的 `.ps1` 按系统 ANSI 代码页（中文 Windows = GBK）解码，中文会变乱码并直接破坏解析（2026-09-02 安装脚本闪退就是这个原因）。C++ 源码里写中文注释/字符串必须给 MSVC 加 `/utf-8`：`add_compile_options($<$<CXX_COMPILER_ID:MSVC>:/utf-8>)`，否则同样乱码外加 C4819。
10. **多版本共存，但只认一个默认**：Python / MySQL / MariaDB / Redis / Nginx 同类可装多版本，每个 kind **至多一个 `default` 实例**，default 才拿规范端口（3306 / 6379 / 80），其余走槽位端口，可随时切换。三条不可违反的细节：① 每个实例的 datadir 独立（`data/instances/<kind>/<version>/`），**永不共享**，MySQL 8 的目录 5.7 打不开；② 切默认是**带回滚的事务**（停旧 → 原子换 role 与端口 → 起新并协议握手 → 级联重渲染引用方），失败就把旧实例拉回来，不许留中间态；③ 换默认 Python **不会**动已有 venv（venv 钉死在建它的解释器上），只能显式提供「重建 venv」。

## 当前环境状态（2026-09-03）

- **P0 工具链已装齐**（2026-09-02 第二轮，`SkipVs=False` 全跑通）：CMake 4.4.3 + Ninja 1.13.2 在 `C:\Users\haigle\AppData\Roaming\Python\Python314\Scripts`（已在用户 PATH）；Qt **6.8.3 msvc2022_64** 在 `D:\Qt\6.8.3\msvc2022_64`；Qt Creator 在 `D:\Qt\Tools\QtCreator\bin`；VS Build Tools 在 `D:\BuildTools`，实测 `cl` 报 **19.44.35228**（v143 / 17.14）。构建时给 `CMAKE_PREFIX_PATH=D:\Qt\6.8.3\msvc2022_64`。
- **P0 已验收（2026-09-03 08:36）**：`scripts/verify-toolchain.ps1 -Stage all` 六项全 `[ok]`，launcher 退出码 0，证据日志 `logs/verify-20260903-083542.log`。CMake 识别到 `MSVC 19.44.35228.0` → `find_package(Qt6)` 通过 → Ninja 真链接出 `probe.exe` → 运行输出 `devdatapod P0 probe: Qt runtime 6.8.3 compiled against 6.8.3`。头文件 / 导入库 / CRT / `Qt6Core.dll` 解析全链路打通，**不再是「文件都在」级别的证据**。
- 构建要在**已 call 过 `vcvars64.bat` 的 cmd** 里跑（面板脚本用 `Invoke-Env` 那个模式：configure / build / run 分三次调用，别用一条 `&&` 链把失败点藏起来）。探测工程落在 `.p0-probe/`（已 gitignore），改它等于改 P0 验收标准，改之前想清楚再动。
- **360 安全卫士在装**（`C:\Program Files (x86)\360\360Safe`）。它的注册表防护会拦 `HKCR\CLSID\*\InprocServer32` 写入 —— VS Build Tools 第一次装就死在这里（MSI 报 1406 / 系统错误 5 = ACCESS DENIED，回滚后外层返回 1603，33 秒就完事，跟下载无关）。**退出 360 后同一条命令一次通过，诊断已证实。** 任何需要注册 COM 组件的 MSI 都要先彻底退出 360（托盘图标退出，关窗口没用）。本轮 P0 编译 + 运行 exe 没有被 360 拦，说明拦的是 MSI 注册表写入，不是编译器行为。
- 本机**没有宝塔面板**（2026-09-03 核实：`D:\BtSoft` 与 `C:\BtSoft` 都不存在，早先记录的「有宝塔 + PHP 8.5.3」已失效）。所以 80 / 3306 / 6379 有没有被占用**必须实测**，用 `PortProbe` / `net::isPortFree` 看，不要按某个面板在不在来推断。
- 本机现成的**组件实物**只有 `D:\phpstudy_pro\Extensions`（只读参考，别动它）：`redis3.0.504`、`MySQL5.7.26` + `MySQL8.0.12`、`Nginx1.15.11`、`php\php7.3.4nts`、`Apache2.4.39`。**没有 redis 7.2.5** —— P2 验收要求的两个版本里只有一半是现成的，另一半仍然要下载。
- 注册实物目录进 `data/services.json` 的入口：`build\devdatapod-selftest.exe --stage Extensions\redis3.0.504 ...`（目录名按 `<kind><version>` 解析，注册完还会跑一次 `detect()` 确认二进制真能起、版本与目录名一致）。安装器是 P5 的事，别在 P2 里造。`scripts/build.ps1` 会把 `DEVDATAPOD_ROOT` 指到仓库根，`data/` 与 `Extensions/` 因此落在 `.gitignore` 期望的位置，不会跑到 `build/` 里去。
- 安装脚本：`scripts/setup-toolchain.cmd`（启动器）+ `scripts/setup-toolchain.ps1`，日志写 `logs/setup-<ts>.log`。可重复执行，已完成的步骤会自动跳过。
- **本机没有 Visual Studio IDE**（2026-09-03 核实：`D:\BuildTools\Common7\IDE\devenv.exe` 不存在，装的只是 Build Tools 命令行工具链）。所以「用 VS 打开 CMakeLists.txt」这类建议在机器上不成立。本机可用的 C++ IDE 有两个：**CLion 2026.2.2**（`C:\Program Files\JetBrains\CLion\bin\clion64.exe`，自带 Qt/QML 插件，读代码首选）与 **Qt Creator 9.0.0**（`D:\Qt\Tools\QtCreator\bin`，构建与断点调试的参考路线）。VS Code 在 `C:\Users\haigle\AppData\Local\Programs\Microsoft VS Code\`，适合 grep、看 diff 与 Markdown。
- **断点可用**（2026-11-12 核实：`C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe` 已装，10.0.26100.7705；Qt Creator 在 Windows 上只支持 CDB 这一个调试后端）。已实测 attach 运行中的 `devdatapod.exe`；退出用 `.detach`，不要用 `q`。不接调试器时的备选证据手段：`devdatapod-selftest` 退出码、`--stage` 的 detect 表、`logs/` 运行日志、事件查看器。

## 已锁定决策（2026-09-02 / 09-03，勿重开讨论）

| 决策 | 结论 |
|---|---|
| 项目名 | **devdatapod**。早期叫 devConsole，已废弃 |
| Qt 版本 | Qt 6.8 LTS + CMake/Ninja，不用 qmake，不碰已 EOL 的 6.5 |
| UI | Widgets 为主 + 局部 QML |
| 组件分发 | 按需下载 + manifest（本体几 MB） |
| 平台优先级 | Windows 先行，macOS 第二 |
| macOS 端口 | 30080 / 30443，Privileged Helper 留到 v2 |
| 内置 DB 工具 | Adminer（单 PHP 文件），不用 phpMyAdmin |
| macOS 默认 DB | MariaDB（MySQL 社区版 ZIP 再分发受限） |
| **多版本共存**（2026-09-03） | 实例制：kind 内多版本并存，`role = default / secondary`，**每 kind 至多一个 default**；default 拿规范端口（DB 3306、Redis 6379），secondary 走槽位段；「设为默认」随时可切，切的是事务不是改个字符串 |
| **Python 默认环境**（2026-09-03） | 多版本解释器共存，但**配置的环境只留一个** = 默认解释器，只有它注入 PATH、只有它新建 venv 与跑站点；已有 venv 不随默认迁移 |
| **MariaDB 与 MySQL**（2026-09-03） | 算两个 kind，不互换 datadir、不互换驱动 |
