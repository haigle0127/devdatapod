//! 网站：站点表格（搜索、创建、管理/编辑、删除）。

use dioxus::prelude::*;

use ddp_core::instance::RunState;
use ddp_core::sites::Site;

use crate::icons::Icon;
use crate::state::{AppState, Dialog};

fn site_row(site: &Site, web_up: bool) -> Element {
    let domain = site.domain.clone();
    let aliases = if site.aliases.is_empty() {
        String::new()
    } else {
        format!("（别名：{}）", site.aliases.join("、"))
    };
    let (status_text, status_cls) = if web_up { ("正常", "pill ok") } else { ("已停用", "pill") };
    let expire = if site.expire_at.is_empty() { "—".to_string() } else { site.expire_at.clone() };
    let path = site.root_dir.clone();
    let expired = site.expired();
    rsx! {
        tr {
            td {
                div { class: "cell-strong", "{domain}" }
                if !aliases.is_empty() {
                    div { class: "cell-sub", "{aliases}" }
                }
            }
            td { span { class: "port", "{site.port}" } }
            td {
                div { class: "cell-sub mono", title: "{path}", "{path}" }
            }
            td {
                span { class: "{status_cls}", "{status_text}" }
                if expired {
                    span { class: "pill warn", style: "margin-left:6px", "已到期" }
                }
            }
            td { "{expire}" }
            td {
                button {
                    class: "soft-btn",
                    onclick: move |_| {
                        let mut app = use_context::<AppState>();
                        app.dialog.set(Some(Dialog::Site { domain: Some(domain.clone()) }));
                    },
                    "管理"
                }
            }
        }
    }
}

#[component]
pub fn SitesPage() -> Element {
    let app = use_context::<AppState>();
    let snapshot = app.snapshot.read().clone();
    let query = app.site_query.read().clone();
    let sites = app.sites.read().clone();
    let web_up = snapshot
        .rows_of_kind("nginx")
        .iter()
        .any(|row| row.state == RunState::Running);
    let filtered: Vec<Site> = if query.trim().is_empty() {
        sites.clone()
    } else {
        let needle = query.trim().to_lowercase();
        sites
            .iter()
            .filter(|site| {
                site.domain.to_lowercase().contains(&needle)
                    || site.root_dir.to_lowercase().contains(&needle)
            })
            .cloned()
            .collect()
    };
    rsx! {
        div { class: "page",
            div { class: "card",
                div { class: "toolbar",
                    div {
                        div { class: "title", "网站" }
                        div { class: "sub", "共 {sites.len()} 个站点 · 绑定 127.0.0.1，仅本机可访问" }
                    }
                    span { class: "spacer" }
                    div { class: "search-wrap",
                        span { class: "ico", Icon { name: "search", size: 14 } }
                        input {
                            class: "search",
                            placeholder: "查找",
                            value: "{query}",
                            oninput: move |event| use_context::<AppState>().site_query.set(event.value()),
                        }
                    }
                    button {
                        class: "btn",
                        onclick: move |_| {
                            let mut app = use_context::<AppState>();
                            if snapshot.rows_of_kind("nginx").is_empty() {
                                app.problem("还没有安装 Nginx，请先到「软件管理」下载");
                                return;
                            }
                            app.dialog.set(Some(Dialog::Site { domain: None }));
                        },
                        "＋ 创建网站"
                    }
                }
                if sites.is_empty() {
                    div { class: "empty-hint", "还没有网站。先启动 web 服务，再点「创建网站」部署第一个站点。" }
                } else {
                    table { class: "table",
                        thead {
                            tr {
                                th { "网站域名" }
                                th { "端口" }
                                th { "物理路径" }
                                th { "状态" }
                                th { "到期" }
                                th { "操作" }
                            }
                        }
                        tbody {
                            for site in filtered {
                                {site_row(&site, web_up)}
                            }
                        }
                    }
                }
            }
        }
    }
}
