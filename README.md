# devdatapod

本地开发服务面板：**nginx / MySQL(MariaDB) / Redis 多版本共存、一键启停、端口只绑 127.0.0.1**。
Windows 先行，本仓库是 **Rust + Dioxus** 重写版（v2.0.0）；原 Qt6/C++ 实现（v1.0.x）完整归档在
**`qt` 分支** 供对照。

分发形态是 Inno Setup 安装包；应用本体保持绿色特性：**全相对路径、不写注册表、不写死绝对路径、
数据全部自包含在安装目录**（`data/`、`logs/`、`WWW/`），卸载即净，拷贝整个目录到别的机器也能直接用。

## 功能一览

- **首页 · 套件**：每个已安装组件一行，状态点/端口/运行时长/内存一目了然；滑动开关启停、重启、
  配置（端口 + 组件参数一次提交，按设置自动重启生效）、星标切换默认实例（事务 + 失败回滚）。
- **首页 · 运行状态**：CPU / 内存圆环仪表 + 处理器拓扑（P/E 核）。
- **首页 · 资源信息**：操作系统 / 主机名 / 局域网 IP / 物理内存 / 面板版本与安装目录。
- **首页 · 日志文件**：面板自身日志最近 200 行，2 秒自动刷新。
- **网站**：创建/管理/删除本地站点（域名、别名、端口、根目录、到期日期），自动写 nginx 托管块、
  自动建 `WWW/<域名>` 目录与占位页；不与用户手改的配置冲突。
- **软件管理**：组件目录（nginx / Redis / MySQL 多版本），点击下载安装、进度显示、一键卸载。
- **设置**：「改配置后自动重启」「关闭按钮最小化到托盘」、检查更新。
- **托盘**：左键 = 显示面板；右键菜单 = 显示面板 / 退出；关闭按钮只最小化，退出只走托盘菜单。
- **单实例**：重复启动会自动唤醒已运行的面板，不会开第二个。

## 安装与使用

### 用户

发布产物是 Inno Setup 安装包：`DevDataPod-v<版本>-Windows.exe`（见 [Releases](https://github.com/haigle0127/devdatapod/releases)）。

- 双击运行 → 选择安装模式 → **选择安装目录**（默认 `D:\Program Files\devdatapod`，无 D 盘则为
  `C:\Program Files\devdatapod`）→ 可选桌面快捷方式 → 安装完成可立即启动。
- 装过 1.0.x 的话直接覆盖升级：自动沿用原安装目录，**数据（`data/`、站点 `WWW/`）保留**，
  已注册的服务组件原样接管。
- 卸载：开始菜单 / 系统设置的卸载入口；运行时产生的 `data/`、`logs/`、`WWW/` 属于用户数据，
  卸载不会删除（绿色特性的另一面：这些数据拷到任何一台机器都能直接接着用）。

> 注意：装到 `C:\Program Files` 时普通用户写不了安装目录下的 `data/`（Windows 权限），
> 该机器上需要右键「以管理员身份运行」面板；装 D 盘没有此问题。

### 开发者

需要 Rust 工具链（`rustc`/`cargo`，msvc，1.80+）：

```powershell
# 调试构建 + 118 项自检
powershell -File scripts/build.ps1

# 发布构建 + 118 项自检 + 出安装包（dist\DevDataPod-v2.0.0-Windows.exe）
powershell -File scripts/build.ps1 -Release

# 开发运行（DEVDATAPOD_ROOT 指到仓库根，数据落仓库 data/，不进 target/）
scripts\run-dev.cmd
```

## 仓库布局

| 路径 | 说明 |
|---|---|
| `crates/ddp-core` | UI 无关核心：端口规划（`ports.json`）、实例注册表、服务驱动（nginx/mysql/redis）、引擎 actor（命令 channel + snapshot 轮询）、托管块配置渲染、孤儿进程治理、118 项自检 |
| `crates/ddp-app` | Dioxus Desktop 面板（`devdatapod.exe`）：无边框窗口、四页 UI、系统托盘、单实例守卫 |
| `crates/ddp-cli` | 控制台入口（`ddp-cli.exe`）：`--selftest` / `--stage` / `--run` |
| `installer/` | Inno Setup 安装脚本（`devdatapod.iss`，中文向导词条在 `[Messages]`） |
| `scripts/` | `build.ps1`（构建+自检+出包）、`run-dev.cmd`（开发启动） |
| `config/ports.default.json` | 嵌入式端口规划种子（面板首次启动写入 `data/ports.json`） |

## 架构要点

```
ddp-app (UI, Dioxus)  ──命令channel──▶  ddp-core Engine actor  ──▶  服务驱动 (Supervisor/Job Object)
        ▲                                    │
        └── snapshot 轮询 (1.5s) ◀── Arc<Snapshot> ◀── 端口分配/注册表/协议探针
```

- **core 与 UI 隔离**：`ddp-core` 不依赖任何 UI 框架；v1.1 计划把 core 拆成独立进程，只加 IPC。
- **端口全部来自规划文件**：除 80/443/3306/6379（仅默认实例）外一律落在 30000–32000 槽位段，
  代码里无硬编码端口；换端口会级联重渲染引用方（nginx vhost 等）。
- **多版本共存**：每个 kind 至多一个 `default` 实例拿规范端口，其余走槽位；切默认是
  「停旧 → 原子换 role/端口 → 起新并协议握手 → 失败整体回滚」的事务。
- **datadir 永不共享**：`data/instances/<kind>/<version>/`，MySQL 8 的目录 5.7 打不开。
- **配置托管块**：模板渲染 + 托管块标记，用户手改块外内容不会被覆盖。
- **孤儿进程治理**：服务挂在 Job Object（`KILL_ON_JOB_CLOSE`）下，面板崩溃/被杀会连带清理；
  启动时再按安装目录路径前缀扫一遍残留。

## 自检与验收

```powershell
# 118 项不变量：端口规划/分配/互换、注册表、引擎 reconcile、托管块、日志编解码……
ddp-cli --selftest
# 118 passed, 0 failed

# 把现成的组件实物目录注册进面板（按 <kind><version> 解析目录名，注册后跑 detect() 验证）
ddp-cli --stage Extensions\redis3.0.504 Extensions\Nginx1.15.11

# 驱动验收：起服务 → 协议探针（PING/握手/HTTP）→ 停服务
ddp-cli --run redis-3.0.504
```

排查运行问题先看 `logs/devdatapod.log`（引擎动作、端口冲突、panic 都写在这里）。

## 分支

| 分支 | 内容 |
|---|---|
| `main` | 当前 Rust + Dioxus 实现（v2.0.0） |
| `qt` | 原 Qt6/C++ 实现（v1.0.1），只读归档，供对照行为细节 |

## 常见问题

- **双击启动有个黑框？** 不会。v2.0.0 起面板是 GUI 子系统，服务子进程也全部 `CREATE_NO_WINDOW`。
- **点启动没反应？** 多半是单实例拦截——另一个 devdatapod 已在运行并被唤到前台，看任务栏/托盘。
- **80/3306/6379 起不来？** 被别的程序占用（常见：本机 IIS、另一个面板实例）。引擎会把冲突写进
  日志并冻结该分配，换端口或停掉占用方即可。
- **装到 C 盘 Program Files 起不来？** 权限问题，右键管理员运行，或改装 D 盘。
