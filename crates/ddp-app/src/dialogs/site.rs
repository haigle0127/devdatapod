//! 创建 / 编辑网站对话框。

use dioxus::prelude::*;

use ddp_core::sites::{self, Site};

use crate::state::{Action, AppState};

pub fn delete_site(_app: AppState, domain: String) {
    std::thread::spawn(move || {
        let result = crate::state::engine().remove_site(&domain);
        let text = match &result {
            Ok(()) => format!("{domain} 已删除"),
            Err(error) => format!("删除失败:{error}"),
        };
        crate::state::push_message(crate::state::WorkerMessage {
            ok: result.is_ok(),
            text,
            id: None,
        });
    });
}

#[component]
pub fn SiteDialog(domain: Option<String>) -> Element {
    let app = use_context::<AppState>();
    let editing = domain.clone();
    let existing = app.sites.read().clone();
    let original: Option<Site> = editing
        .as_ref()
        .and_then(|name| sites::find(&existing, name).cloned());

    let defaults = Site {
        domain: String::new(),
        aliases: Vec::new(),
        port: default_port(),
        root_dir: String::new(),
        expire_at: String::new(),
        remark: String::new(),
    };
    let base = original.clone().unwrap_or(defaults);

    let mut domain_value = use_signal(|| base.domain.clone());
    let mut aliases = use_signal(|| base.aliases.join(" "));
    let mut port = use_signal(|| base.port.to_string());
    let mut root_dir = use_signal(|| base.root_dir.clone());
    let mut expire_at = use_signal(|| base.expire_at.clone());
    let mut remark = use_signal(|| base.remark.clone());
    let mut error = use_signal(String::new);

    let title = if editing.is_some() {
        format!("{} 配置", base.domain)
    } else {
        "创建网站".to_string()
    };
    let https = base.port == 443;

    rsx! {
        div { class: "overlay", onclick: move |_| use_context::<AppState>().dialog.set(None),
            div { class: "dialog", onclick: move |event| event.stop_propagation(),
                h3 { "{title}" }
                div { class: "field",
                    label { "域名" }
                    div { class: "grow",
                        input {
                            class: "input",
                            placeholder: "www.example.com",
                            value: "{domain_value}",
                            oninput: move |event| domain_value.set(event.value()),
                        }
                    }
                }
                div { class: "field",
                    label { "第二域名" }
                    div { class: "grow",
                        input {
                            class: "input",
                            placeholder: "选填，多个用空格或逗号分隔",
                            value: "{aliases}",
                            oninput: move |event| aliases.set(event.value()),
                        }
                    }
                }
                div { class: "field",
                    label { "端口" }
                    div { class: "grow radio-row",
                        button {
                            class: if !https { "radio on" } else { "radio" },
                            onclick: move |_| port.set("80".into()),
                            "http"
                        }
                        button {
                            class: if https { "radio on" } else { "radio" },
                            title: "https 需要证书，后续版本支持",
                            onclick: move |_| port.set("443".into()),
                            "https"
                        }
                        input {
                            class: "input mono",
                            style: "max-width:110px",
                            r#type: "number",
                            value: "{port}",
                            oninput: move |event| port.set(event.value()),
                        }
                    }
                }
                div { class: "field",
                    label { "根目录" }
                    div { class: "grow",
                        input {
                            class: "input mono",
                            placeholder: "留空则默认 WWW/<域名>",
                            value: "{root_dir}",
                            oninput: move |event| root_dir.set(event.value()),
                        }
                    }
                }
                div { class: "field",
                    label { "到期日期" }
                    div { class: "grow",
                        input {
                            class: "input mono",
                            placeholder: "选填，如 2031-08-12",
                            value: "{expire_at}",
                            oninput: move |event| expire_at.set(event.value()),
                        }
                    }
                }
                div { class: "field",
                    label { "备注" }
                    div { class: "grow",
                        input {
                            class: "input",
                            placeholder: "选填",
                            value: "{remark}",
                            oninput: move |event| remark.set(event.value()),
                        }
                    }
                }
                if !error.read().is_empty() {
                    div { class: "error", "{error}" }
                }
                div { class: "dialog-actions",
                    if let Some(name) = editing.clone() {
                        button {
                            class: "btn danger",
                            onclick: move |_| {
                                let mut app = use_context::<AppState>();
                                app.dialog.set(None);
                                app.ask_danger(
                                    "删除站点",
                                    format!("确定删除站点 {name}？\n只删除配置，站点文件保留。"),
                                    "删除",
                                    Action::DeleteSite(name.clone()),
                                );
                            },
                            "删除站点"
                        }
                    }
                    span { class: "spacer" }
                    button {
                        class: "soft-btn",
                        onclick: move |_| use_context::<AppState>().dialog.set(None),
                        "取消"
                    }
                    button {
                        class: "btn",
                        onclick: move |_| {
                            let app = use_context::<AppState>();
                            let site = build_site(
                                &domain_value,
                                &aliases,
                                &port,
                                &root_dir,
                                &expire_at,
                                &remark,
                            );
                            let site = match site {
                                Ok(site) => site,
                                Err(message) => {
                                    error.set(message);
                                    return;
                                }
                            };
                            if site.root_dir.trim().is_empty() {
                                let dir = sites::default_root_dir(crate::state::paths().root(), &site.domain);
                                let site = Site { root_dir: dir, ..site };
                                if let Err(message) = validate(&site, editing.as_deref(), &existing) {
                                    error.set(message);
                                    return;
                                }
                                save(app, site, editing.clone());
                            } else {
                                if let Err(message) = validate(&site, editing.as_deref(), &existing) {
                                    error.set(message);
                                    return;
                                }
                                save(app, site, editing.clone());
                            }
                        },
                        "确认"
                    }
                }
            }
        }
    }
}

fn default_port() -> u16 {
    crate::state::engine()
        .snapshot()
        .ports
        .iter()
        .find(|view| view.key == "http")
        .map(|view| view.port)
        .unwrap_or(80)
}

fn build_site(
    domain: &Signal<String>,
    aliases: &Signal<String>,
    port: &Signal<String>,
    root_dir: &Signal<String>,
    expire_at: &Signal<String>,
    remark: &Signal<String>,
) -> Result<Site, String> {
    let port: u16 = port
        .read()
        .trim()
        .parse()
        .map_err(|_| "端口必须是 1-65535 的整数".to_string())?;
    let alias_list: Vec<String> = aliases
        .read()
        .split(|c: char| c.is_whitespace() || c == ',' || c == ';' || c == '，' || c == '；')
        .map(str::trim)
        .filter(|part| !part.is_empty())
        .map(str::to_string)
        .collect();
    Ok(Site {
        domain: domain.read().trim().to_string(),
        aliases: alias_list,
        port,
        root_dir: root_dir.read().trim().to_string(),
        expire_at: expire_at.read().trim().to_string(),
        remark: remark.read().trim().to_string(),
    })
}

fn validate(site: &Site, editing: Option<&str>, existing: &[Site]) -> Result<(), String> {
    sites::validate(site, editing, existing)?;
    if site.port == default_port() {
        return Err(format!("端口 {} 已被 Web 服务默认站点占用，请换一个", site.port));
    }
    Ok(())
}

fn save(mut app: AppState, site: Site, previous: Option<String>) {
    std::thread::spawn(move || {
        let result = crate::state::engine().save_site(site.clone(), previous);
        let text = match &result {
            Ok(()) => format!("站点 {} 已保存", site.domain),
            Err(error) => format!("保存失败:{error}"),
        };
        crate::state::push_message(crate::state::WorkerMessage { ok: result.is_ok(), text, id: None });
    });
    app.dialog.set(None);
}
