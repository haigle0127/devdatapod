# devdatapod

绿色便携的本地开发服务面板：nginx / MySQL(MariaDB) / Redis 一键启停，Windows 先行。
本仓库是 **Rust + Dioxus** 重写版；原 Qt6/C++ 实现完整归档在 **qt 分支** 供对照。

## 布局

| 路径 | 是什么 |
|---|---|
| `crates/ddp-core` | UI 无关的核心：端口规划、实例注册表、驱动（nginx/mysql/redis）、引擎 actor、托管块配置、孤儿进程治理、自检 |
| `crates/ddp-app` | Dioxus Desktop 面板（`devdatapod.exe`）：首页/网站/软件管理/设置、托盘、单实例、关窗到托盘 |
| `crates/ddp-cli` | 控制台入口（`ddp-cli.exe`）：`--selftest` 118 项不变量、`--stage` 注册实物目录、`--run` 起停验收 |
| `config/ports.default.json` | 嵌入式端口规划种子（30000–32000，只绑 127.0.0.1） |

## 构建与运行

需要 Rust 工具链（`rustc`/`cargo`，本机 1.98.1，msvc）。

```powershell
# 调试构建 + 118 项自检
powershell -File scripts/build.ps1

# 发布构建，产物拷到 dist\
powershell -File scripts/build.ps1 -Release

# 开发运行（以仓库根为安装根，数据落 data/）
scripts\run-dev.cmd
```

绿色便携：`Paths::discover()` 以 exe 所在目录（其父级若非 `bin`）为安装根；
开发/测试用环境变量 `DEVDATAPOD_ROOT` 覆盖。面板自身端口（ipc 30001 等）取自
`data/ports.json`，全部只绑 `127.0.0.1`。

## 核心规则（移植自 Qt 版，详见 AGENTS.md）

- 端口一律来自 `data/ports.json` 规划（30000–32000 槽位段 + 80/443/3306/6379 规范端口），禁止硬编码。
- 每个 kind 至多一个 `default` 实例；切默认是带事务与回滚的操作。
- 每个实例 datadir 独立（`data/instances/<kind>/<version>/`），永不共享。
- 配置经 schema + 模板渲染 + 托管块标记，不静默覆盖用户手改。
- Windows 孤儿治理：Job Object（`KILL_ON_JOB_CLOSE`）+ 启动时按安装根路径清扫。
- 单实例：回环 TCP（`panel.ipc` 端口）+ PID 锁兜底；二次启动会唤醒已运行的面板。

## 自检

```powershell
ddp-cli --selftest     # 118 passed, 0 failed
ddp-cli --stage Extensions\redis3.0.504   # 注册实物目录并 detect()
ddp-cli --run redis-3.0.504               # 起服务→协议探针→停服务
```
