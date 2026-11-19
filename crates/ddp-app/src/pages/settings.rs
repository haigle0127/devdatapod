//! 设置：服务行为开关 + 关于 / 检查更新。

use dioxus::prelude::*;

use crate::icons::Icon;
use crate::state::{AppState, WorkerMessage};

fn toggle_row(
    name: &'static str,
    desc: &'static str,
    on: bool,
    on_change: EventHandler<bool>,
) -> Element {
    rsx! {
        div { class: "setting-row",
            div { class: "text",
                div { class: "name", "{name}" }
                div { class: "desc", "{desc}" }
            }
            button {
                class: if on { "switch on" } else { "switch" },
                onclick: move |_| on_change.call(!on),
                span { class: "knob" }
            }
        }
    }
}

fn check_update(version: String, mut status: Signal<String>, mut busy: Signal<bool>) {
    busy.set(true);
    status.set("正在检查…".into());
    std::thread::spawn(move || {
        let result = (|| -> Result<String, String> {
            let response = ureq::get("https://api.github.com/repos/haigle0127/devdatapod/releases/latest")
                .set("User-Agent", "devdatapod")
                .call()
                .map_err(|error| format!("检查失败：{error}"))?;
            let body = response
                .into_string()
                .map_err(|error| format!("检查失败：{error}"))?;
            let parsed: serde_json::Value =
                serde_json::from_str(&body).map_err(|_| "检查失败：解析不到版本号".to_string())?;
            let latest = parsed["tag_name"]
                .as_str()
                .map(|tag| tag.trim_start_matches('v').to_string())
                .filter(|tag| !tag.is_empty())
                .ok_or_else(|| "检查失败：解析不到版本号".to_string())?;
            let current = version.trim_start_matches('v');
            if latest == current {
                Ok(format!("已是最新版本（v{current}）"))
            } else {
                Ok(format!("发现新版本 v{latest} · 当前 v{current}"))
            }
        })();
        let (ok, text) = match result {
            Ok(text) => (true, text),
            Err(text) => (false, text),
        };
        crate::state::push_message(WorkerMessage { ok, text, id: None });
    });
}

#[component]
pub fn SettingsPage() -> Element {
    let app = use_context::<AppState>();
    let settings = *app.settings.read();
    let version = app.version.read().clone();
    let update_status = app.update_status.read().clone();
    let update_busy = *app.update_busy.read();
    rsx! {
        div { class: "page",
            div { class: "card card-pad",
                div { class: "section-head",
                    span { class: "accent-bar" }
                    span { class: "section-title", "服务" }
                }
                div { class: "settings-block",
                    {toggle_row(
                        "编辑配置后自动重启服务",
                        "修改端口或组件设置后自动重启该服务使配置生效；关闭则只保存配置，需手动重启",
                        settings.auto_restart,
                        {
                            let mut app = app;
                            EventHandler::new(move |on: bool| {
                                app.settings.write().auto_restart = on;
                                if !crate::state::store_setting("ui/autoRestartOnConfig", on) {
                                    app.problem("设置保存失败");
                                }
                            })
                        },
                    )}
                    {toggle_row(
                        "关闭按钮最小化到托盘",
                        "开启：点关闭把面板藏进系统托盘；关闭：点关闭最小化到任务栏。退出只走托盘右键菜单",
                        settings.close_to_tray,
                        {
                            let mut app = app;
                            EventHandler::new(move |on: bool| {
                                app.settings.write().close_to_tray = on;
                                if !crate::state::store_setting("ui/closeToTray", on) {
                                    app.problem("设置保存失败");
                                }
                            })
                        },
                    )}
                }
            }
            div { class: "card card-pad",
                div { class: "section-head",
                    span { class: "accent-bar violet" }
                    span { class: "section-title", "关于" }
                }
                div { class: "setting-row",
                    div { class: "text",
                        div { class: "name", "检查更新" }
                        div { class: "desc", "从 GitHub Releases 检查 devdatapod 新版本" }
                    }
                    div { class: "update-line",
                        span { "{update_status}" }
                        if update_status.starts_with("发现新版本") {
                            button {
                                class: "link-btn",
                                onclick: move |_| {
                                    let _ = std::process::Command::new("cmd")
                                        .args(["/c", "start", "", "https://github.com/haigle0127/devdatapod/releases/latest"])
                                        .spawn();
                                },
                                "打开下载页"
                            }
                        }
                        button {
                            class: "soft-btn",
                            disabled: update_busy,
                            onclick: move |_| check_update(version.clone(), app.update_status, app.update_busy),
                            Icon { name: "refresh", size: 14 }
                            "检查更新"
                        }
                    }
                }
            }
            div { class: "hint", style: "text-align:center;padding:6px 0",
                "devdatapod v{version} · 绿色便携 · 服务端口只绑定 127.0.0.1"
            }
        }
    }
}
