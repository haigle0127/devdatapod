//! Window chrome and shared chrome-level widgets: title bar, sidebar, status bar,
//! the modal dialog host and the toast.

use dioxus::desktop::{window, WindowCloseBehaviour};
use dioxus::prelude::*;

use crate::icons::Icon;
use crate::state::{Action, AppState, Dialog, Page};
use crate::theme;

fn close_behaviour() -> WindowCloseBehaviour {
    if crate::state::load_settings().close_to_tray {
        WindowCloseBehaviour::WindowHides
    } else {
        WindowCloseBehaviour::WindowCloses
    }
}

pub fn apply_close_behaviour() {
    window().set_close_behavior(close_behaviour());
}

#[component]
pub fn TitleBar() -> Element {
    rsx! {
        div { class: "titlebar",
            div {
                class: "drag",
                onmousedown: move |_| { let _ = window().drag(); },
                img { class: "brand-img", src: "{crate::logo_data_uri()}", alt: "devdatapod" }
                span { class: "brand-name", "devdatapod" }
                span { class: "app-tagline", "本地开发服务面板" }
            }
            div { class: "win-controls",
                button {
                    class: "winbtn",
                    title: "最小化",
                    onclick: move |_| window().set_minimized(true),
                    Icon { name: "minus", size: 14 }
                }
                button {
                    class: "winbtn",
                    title: "最大化 / 还原",
                    onclick: move |_| window().toggle_maximized(),
                    Icon { name: "restore", size: 13 }
                }
                button {
                    class: "winbtn close",
                    title: "关闭",
                    onclick: move |_| window().close(),
                    Icon { name: "close", size: 14 }
                }
            }
        }
    }
}

#[component]
pub fn Sidebar() -> Element {
    let app = use_context::<AppState>();
    let page = *app.page.read();
    let item = |target: Page, icon: &'static str, label: &'static str, current: Page| {
        let active = target == current;
        rsx! {
            button {
                class: if active { "nav-item active" } else { "nav-item" },
                onclick: move |_| use_context::<AppState>().page.set(target),
                span { class: "ico", Icon { name: icon, size: 17 } }
                "{label}"
            }
        }
    };
    rsx! {
        div { class: "sidebar",
            div { class: "side-brand",
                div {
                    div { class: "side-name", "devdatapod" }
                    div { class: "side-cap", "HAIGLE.CN" }
                }
            }
            nav { class: "nav",
                {item(Page::Home, "house", "首页", page)}
                {item(Page::Sites, "globe", "网站", page)}
                {item(Page::Software, "package", "软件管理", page)}
                {item(Page::Settings, "settings", "设置", page)}
            }
            div { class: "promo",
                h4 { "devdatapod 面板" }
                p { "简单的数据服务集成环境" }
            }
        }
    }
}

#[component]
pub fn StatusBar() -> Element {
    let app = use_context::<AppState>();
    let snapshot = app.snapshot.read().clone();
    let version = app.version.read().clone();
    let running = snapshot.rows.iter().filter(|row| row.state == ddp_core::instance::RunState::Running).count();
    let busy = !app.busy.read().is_empty();
    let (dot_class, chip_text) = if !snapshot.ready {
        ("crashed", "引擎不可用")
    } else if busy {
        ("starting", "处理中…")
    } else {
        ("running", "就绪")
    };
    rsx! {
        div { class: "statusbar",
            span { class: "chip", span { class: "dot {dot_class}" } "{chip_text}" }
            if !snapshot.ready && !snapshot.problem.is_empty() {
                span { class: "chip", title: "{snapshot.problem}", "查看日志了解详情" }
            }
            span { class: "chip", "运行 {running} / 共 {snapshot.rows.len()}" }
            span { class: "spacer" }
            span { class: "chip", "服务绑定 127.0.0.1" }
            span { class: "version", "v{version}" }
        }
    }
}

#[component]
pub fn Gauge(label: &'static str, value: String, fraction: f64, violet: bool) -> Element {
    let pct = (fraction.clamp(0.0, 1.0) * 100.0).round();
    let angle = fraction.clamp(0.0, 1.0) * 360.0;
    let dash = format!("{angle} 360");
    let color = if violet { theme::ACCENT2 } else { theme::ACCENT };
    let track = theme::TRACK;
    rsx! {
        div { class: "gauge",
            svg {
                width: "84",
                height: "84",
                view_box: "0 0 84 84",
                circle {
                    cx: "42",
                    cy: "42",
                    r: "34",
                    fill: "none",
                    stroke: "{track}",
                    stroke_width: "9",
                }
                circle {
                    cx: "42",
                    cy: "42",
                    r: "34",
                    fill: "none",
                    stroke: "{color}",
                    stroke_width: "9",
                    stroke_linecap: "round",
                    stroke_dasharray: "{dash}",
                    transform: "rotate(-90 42 42)",
                }
                text {
                    x: "42",
                    y: "47",
                    text_anchor: "middle",
                    font_size: "16",
                    font_weight: "600",
                    fill: theme::INK,
                    "{value}"
                }
            }
            div { class: "g-label", "{label}" }
            div { class: "g-value", "{pct}%" }
        }
    }
}

/// Renders whichever dialog the app state currently holds.
#[component]
pub fn DialogHost() -> Element {
    let app = use_context::<AppState>();
    let dialog = app.dialog.read().clone();
    match dialog {
        None => rsx! {},
        Some(Dialog::Confirm { title, message, confirm, danger, action }) => {
            let cls = if danger { "btn danger" } else { "btn" };
            rsx! {
                div { class: "overlay", onclick: move |_| use_context::<AppState>().dialog.set(None),
                    div { class: "dialog", onclick: move |event| event.stop_propagation(),
                        h3 { "{title}" }
                        div { class: "msg", "{message}" }
                        div { class: "dialog-actions",
                            button {
                                class: "soft-btn",
                                onclick: move |_| use_context::<AppState>().dialog.set(None),
                                "取消"
                            }
                            button {
                                class: "{cls}",
                                onclick: move |_| {
                                    let mut app = use_context::<AppState>();
                                    app.dialog.set(None);
                                    match &action {
                                        Action::Uninstall { component, instance } => {
                                            crate::pages::software::uninstall_component(
                                                app,
                                                component.clone(),
                                                instance.clone(),
                                            );
                                        }
                                        Action::DeleteSite(domain) => {
                                            crate::dialogs::site::delete_site(app, domain.clone());
                                        }
                                        Action::Start(id) => crate::state::engine_action(app, id, "启动", action.clone()),
                                        Action::Stop(id) => crate::state::engine_action(app, id, "停止", action.clone()),
                                        Action::Restart(id) => crate::state::engine_action(app, id, "重启", action.clone()),
                                        Action::Promote(id) => crate::state::engine_action(app, id, "切换默认", action.clone()),
                                    }
                                },
                                "{confirm}"
                            }
                        }
                    }
                }
            }
        }
        Some(Dialog::Config { id }) => rsx! { crate::dialogs::config::ConfigDialog { id } },
        Some(Dialog::Site { domain }) => rsx! { crate::dialogs::site::SiteDialog { domain } },
    }
}

#[component]
pub fn ToastHost() -> Element {
    let app = use_context::<AppState>();
    let toast = app.toast.read().clone();
    match toast {
        Some(toast) => {
            let cls = if toast.error { "toast error" } else { "toast" };
            rsx! {
                div { class: "{cls}", "{toast.text}" }
            }
        }
        None => rsx! {},
    }
}
