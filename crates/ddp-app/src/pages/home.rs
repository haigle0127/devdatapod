//! 首页：公告条、套件列表（服务行）、底部「运行状态 / 资源信息 / 日志文件」三页签。

use dioxus::prelude::*;

use ddp_core::engine::Row;
use ddp_core::instance::RunState;

use crate::components::Gauge;
use crate::icons::Icon;
use crate::state::{Action, AppState, Dialog, HomeTab};
use crate::sysinfo;

fn percent(fraction: f64) -> String {
    format!("{:.1}%", fraction * 100.0)
}

fn format_uptime(ms: Option<u64>) -> String {
    let Some(ms) = ms else { return "--".into() };
    let total_minutes = ms / 60000;
    let days = total_minutes / 1440;
    let hours = (total_minutes % 1440) / 60;
    let minutes = total_minutes % 60;
    if days > 0 {
        format!("{days} 天 {hours} 小时")
    } else if hours > 0 {
        format!("{hours} 小时 {minutes} 分钟")
    } else {
        format!("{} 分钟", minutes.max(1))
    }
}

fn row_meta(row: &Row) -> String {
    let mut parts = Vec::new();
    if row.port > 0 {
        parts.push(format!("端口 {}", row.port));
    }
    if !row.detail.is_empty() {
        parts.push(row.detail.clone());
    }
    if row.uptime_ms > 0 {
        parts.push(format!("运行 {}", format_uptime(Some(row.uptime_ms))));
    }
    if row.rss_bytes > 0 {
        parts.push(format!("内存 {:.0} MB", row.rss_bytes as f64 / 1048576.0));
    }
    if parts.is_empty() {
        row.instance.role_label().to_string()
    } else {
        parts.join(" · ")
    }
}

#[component]
fn ServiceRowComp(row: Row) -> Element {
    let app = use_context::<AppState>();
    let id = row.instance.id.clone();
    let name = row.instance.display_name();
    let busy = app.is_busy(&id);
    let state = row.state;
    let transitioning = matches!(state, RunState::Starting | RunState::Switching);
    let locked = busy || transitioning;
    let switch_on = matches!(state, RunState::Running | RunState::Starting);
    let dot = crate::theme::state_key(state);
    let role_badge = if row.instance.is_default { "badge default" } else { "badge secondary" };
    let role_text = row.instance.role_label();
    let port_text = if row.port > 0 { format!("端口 {}", row.port) } else { String::new() };
    let meta = row_meta(&row);
    let promotable = !row.instance.is_default;
    let promote_id = id.clone();
    let start_id = id.clone();
    let restart_id = id.clone();
    let config_id = id;
    rsx! {
        div { class: "rowline",
            span { class: "dot {dot}" }
            div { class: "svc-id",
                div { class: "svc-name",
                    "{name}"
                    span { class: "{role_badge}", "{role_text}" }
                }
                div { class: "svc-meta", "{meta}" }
            }
            if !port_text.is_empty() {
                span { class: "port", "{port_text}" }
            }
            if promotable {
                button {
                    class: "icon-btn",
                    title: "设为默认",
                    onclick: move |_| {
                        let mut app = use_context::<AppState>();
                        app.ask("设为默认", format!("将 {name} 设为默认实例？\n切换是带事务的操作，失败会自动回滚。"), "设为默认", Action::Promote(promote_id.clone()));
                    },
                    Icon { name: "star", size: 15 }
                }
            } else {
                span { class: "icon-btn", title: "当前默认实例", Icon { name: "star", size: 15 } }
            }
            button {
                class: if switch_on { "switch on" } else { "switch" },
                disabled: locked,
                title: if switch_on { "停止" } else { "启动" },
                onclick: move |_| {
                    let app = use_context::<AppState>();
                    if switch_on {
                        crate::state::engine_action(app, &start_id, "停止", Action::Stop(start_id.clone()));
                    } else {
                        crate::state::engine_action(app, &start_id, "启动", Action::Start(start_id.clone()));
                    }
                },
                span { class: "knob" }
            }
            button {
                class: "soft-btn",
                disabled: locked,
                onclick: move |_| {
                    let app = use_context::<AppState>();
                    crate::state::engine_action(app, &restart_id, "重启", Action::Restart(restart_id.clone()));
                },
                "重启"
            }
            button {
                class: "soft-btn",
                onclick: move |_| {
                    let mut app = use_context::<AppState>();
                    app.dialog.set(Some(Dialog::Config { id: config_id.clone() }));
                },
                "配置"
            }
        }
    }
}

#[component]
pub fn HomePage() -> Element {
    let app = use_context::<AppState>();
    let snapshot = app.snapshot.read().clone();
    let tab = *app.home_tab.read();
    let _sys = app.sys.read().clone();
    let rows = snapshot.rows.clone();
    let running = rows.iter().filter(|row| row.state == RunState::Running).count();

    let tab_button = |target: HomeTab, icon: &'static str, label: &'static str| {
        let active = tab == target;
        rsx! {
            button {
                class: if active { "menu-btn active" } else { "menu-btn" },
                onclick: move |_| use_context::<AppState>().home_tab.set(target),
                Icon { name: icon, size: 15 }
                "{label}"
            }
        }
    };

    rsx! {
        div { class: "page",
            div { class: "card announce",
                span { class: "ico", color: crate::theme::ACCENT, Icon { name: "speaker", size: 15 } }
                span { "欢迎使用 devdatapod —— 绿色便携的本地开发服务面板" }
                button {
                    class: "link link-btn",
                    onclick: move |_| {
                        let _ = std::process::Command::new("cmd")
                            .args(["/c", "start", "", "https://github.com/haigle0127/devdatapod"])
                            .spawn();
                    },
                    "公告"
                }
            }
            div { class: "card suite",
                div { class: "section-head",
                    span { class: "accent-bar" }
                    span { class: "section-title", "套件" }
                    span { class: "hint", "共 {rows.len()} 个服务" }
                    span { class: "spacer" }
                    span { class: "hint", "运行 {running} / 停止 {rows.len() - running}" }
                }
                div { class: "suite-body",
                    if rows.is_empty() {
                        div { class: "empty-hint", "还没有已安装的服务实例，套件列表会在装好第一个组件后出现" }
                    } else {
                        for row in rows {
                            ServiceRowComp { key: "{row.instance.id}", row }
                        }
                    }
                }
            }
            div { class: "bottom",
                div { class: "card menu-card",
                    {tab_button(HomeTab::Status, "activity", "运行状态")}
                    {tab_button(HomeTab::Resource, "chart-pie", "资源信息")}
                    {tab_button(HomeTab::Log, "file-text", "日志文件")}
                }
                div { class: "card pane",
                    match tab {
                        HomeTab::Status => rsx! { StatusPane {} },
                        HomeTab::Resource => rsx! { ResourcePane {} },
                        HomeTab::Log => rsx! { LogPane {} },
                    }
                }
            }
        }
    }
}

#[component]
fn StatusPane() -> Element {
    let sys = use_context::<AppState>().sys.read().clone();
    let topo = sysinfo::topology();
    let model = if topo.model.is_empty() { "未知型号" } else { topo.model.as_str() };
    let cores = if topo.hybrid() {
        format!(
            "核心 / 线程　{} / {}　·　P 核 {} · E 核 {}",
            topo.physical_cores, topo.logical_processors, topo.performance_cores, topo.efficient_cores
        )
    } else {
        format!("核心 / 线程　{} / {}　·　同构无大小核", topo.physical_cores, topo.logical_processors)
    };
    let cpu_text = if sys.valid { percent(sys.cpu) } else { "--".into() };
    let mem_text = if sys.valid { percent(sys.mem) } else { "--".into() };
    let mem_detail = if sys.valid {
        format!("{:.1} / {:.1} GB", sys.mem_used_gb, sys.mem_total_gb)
    } else {
        "--".into()
    };
    let cpu_width = format!("width:{}%", (sys.cpu * 100.0).clamp(0.0, 100.0) as i32);
    let mem_width = format!("width:{}%", (sys.mem * 100.0).clamp(0.0, 100.0) as i32);
    rsx! {
        Gauge { label: "CPU使用率", value: cpu_text.clone(), fraction: if sys.valid { sys.cpu } else { 0.0 }, violet: true }
        Gauge { label: "内存使用率", value: mem_text.clone(), fraction: if sys.valid { sys.mem } else { 0.0 }, violet: false }
        div { class: "divider" }
        div { class: "hw",
            div { class: "section-head",
                span { class: "accent-bar pink" }
                span { class: "section-title", "处理器与内存" }
            }
            div { class: "model", title: "{model}", "{model}" }
            div { class: "sub", "{cores}" }
            div { class: "meter-row",
                span { class: "name", "CPU 总占用" }
                div { class: "meter",
                    i { style: "{cpu_width}" }
                }
                span { class: "pct", "{cpu_text}" }
            }
            div { class: "meter-row",
                span { class: "name", "内存使用率" }
                div { class: "meter violet",
                    i { style: "{mem_width}" }
                }
                span { class: "pct", "{mem_detail}" }
            }
        }
    }
}

#[component]
fn ResourcePane() -> Element {
    let app = use_context::<AppState>();
    let sys = app.sys.read().clone();
    let snapshot = app.snapshot.read().clone();
    let version = app.version.read().clone();
    let sites_count = app.sites.read().len();
    let memory_text = if sys.valid {
        format!("可用 {:.1} / {:.1} GB", sys.avail_gb, sys.mem_total_gb)
    } else {
        "--".into()
    };
    let kv = |key: &'static str, value: String| {
        rsx! {
            div { class: "kv",
                span { class: "k", "{key}" }
                span { class: "v", title: "{value}", "{value}" }
            }
        }
    };
    rsx! {
        div { class: "resource",
            div { class: "col",
                div { class: "col-title", "系统" }
                {kv("操作系统", sysinfo::os_name())}
                {kv("主机名", sysinfo::host_name())}
                {kv("架构", sysinfo::cpu_arch())}
                {kv("运行时长", format_uptime(ddp_core::sysstat::uptime_ms()))}
            }
            div { class: "col",
                div { class: "col-title", "网络与内存" }
                {kv("局域网 IP", sysinfo::lan_ip())}
                {kv("服务绑定", "127.0.0.1".into())}
                {kv("物理内存", memory_text)}
            }
            div { class: "col",
                div { class: "col-title", "面板" }
                {kv("版本", format!("v{version}"))}
                {kv("安装目录", crate::state::paths().root().display().to_string())}
                {kv("实例与站点", format!("{} 个实例 · {} 个站点", snapshot.rows.len(), sites_count))}
                {kv("下载缓存", sysinfo::downloads_cache_text(crate::state::paths()))}
            }
        }
    }
}

#[component]
fn LogPane() -> Element {
    let app = use_context::<AppState>();
    let text = app.log_text.read().clone();
    rsx! {
        div { class: "pane-head",
            span { class: "accent-bar" }
            span { class: "section-title", "日志文件" }
            span { class: "hint", "最近 200 行 · 每 2 秒自动刷新" }
            span { class: "spacer" }
            button {
                class: "soft-btn",
                onclick: move |_| {
                    let dir = crate::state::paths().logs_dir();
                    let _ = std::process::Command::new("explorer").arg(&dir).spawn();
                },
                Icon { name: "folder-open", size: 14 }
                "打开目录"
            }
        }
        pre { class: "logview", "{text}" }
    }
}
