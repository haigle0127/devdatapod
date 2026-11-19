//! 软件管理：组件目录（下载 / 安装进度 / 卸载）。

use dioxus::prelude::*;

use ddp_core::catalog::{catalog, Component};
use ddp_core::instance::ServiceInstance;

use crate::icons::Icon;
use crate::state::{Action, AppState, InstallView};

fn instance_for(component: &Component) -> ServiceInstance {
    let install_dir = ddp_core::install::install_dir(crate::state::paths(), component);
    let install_path = install_dir
        .strip_prefix(crate::state::paths().root())
        .map(|relative| relative.display().to_string())
        .unwrap_or_else(|_| install_dir.display().to_string());
    ServiceInstance {
        id: ServiceInstance::make_id(&component.kind, &component.version),
        kind: component.kind.clone(),
        version: component.version.clone(),
        is_default: false,
        port_key: ServiceInstance::make_port_key(&component.kind, &component.version),
        install_path,
        data_dir: ServiceInstance::make_data_dir(&component.kind, &component.version),
    }
}

/// Download + unpack + register on a worker thread; progress lands in `INSTALLS`.
pub fn install_component(mut app: AppState, component: Component) {
    let id = component.id.clone();
    app.mark_busy(&id);
    ddp_core::download::clear_cancel();
    crate::state::set_install(
        &id,
        InstallView {
            stage: "准备下载…".into(),
            ..Default::default()
        },
    );
    std::thread::spawn(move || {
        let paths = crate::state::paths();
        let component_id = component.id.clone();
        let result = ddp_core::install::install_component(paths, &component, |progress| {
            let stage = if progress.total > 0 {
                format!("下载中 {}%", (progress.fraction() * 100.0) as i32)
            } else {
                format!("下载中 {:.1} MB", progress.received as f64 / 1048576.0)
            };
            crate::state::set_install(
                &component_id,
                InstallView {
                    stage,
                    fraction: progress.fraction(),
                    ..Default::default()
                },
            );
        });
        let message = match result {
            Ok(_) => {
                crate::state::set_install(
                    &component.id,
                    InstallView {
                        stage: "注册服务…".into(),
                        fraction: 1.0,
                        ..Default::default()
                    },
                );
                let instance = instance_for(&component);
                let id = instance.id.clone();
                match crate::state::engine().register(instance) {
                    Ok(()) => {
                        crate::state::set_install(
                            &component.id,
                            InstallView {
                                stage: "已安装".into(),
                                fraction: 1.0,
                                done: true,
                                ok: true,
                                error: String::new(),
                            },
                        );
                        format!("{id} 安装完成")
                    }
                    Err(error) => {
                        crate::state::set_install(
                            &component.id,
                            InstallView {
                                stage: error.clone(),
                                fraction: 1.0,
                                done: true,
                                ok: false,
                                error: error.clone(),
                            },
                        );
                        format!("注册失败：{error}")
                    }
                }
            }
            Err(error) => {
                crate::state::set_install(
                    &component.id,
                    InstallView {
                        stage: error.clone(),
                        fraction: 1.0,
                        done: true,
                        ok: false,
                        error: error.clone(),
                    },
                );
                format!("安装失败：{error}")
            }
        };
        let ok = !message.contains("失败");
        crate::state::push_message(crate::state::WorkerMessage {
            ok,
            text: message,
            id: Some(id),
        });
    });
}

/// Stop + unregister + delete the install directory.
pub fn uninstall_component(mut app: AppState, component_id: String, instance_id: String) {
    app.mark_busy(&component_id);
    std::thread::spawn(move || {
        let result = (|| -> Result<String, String> {
            let instance = crate::state::engine()
                .snapshot()
                .row(&instance_id)
                .map(|row| row.instance.clone());
            crate::state::engine().unregister(&instance_id)?;
            if let Some(instance) = instance {
                ddp_core::install::remove_install_dir(crate::state::paths(), &instance.install_path)?;
            }
            Ok(format!("{instance_id} 已卸载"))
        })();
        let text = match &result {
            Ok(text) => text.clone(),
            Err(error) => format!("卸载失败:{error}"),
        };
        crate::state::push_message(crate::state::WorkerMessage {
            ok: result.is_ok(),
            text,
            id: Some(component_id),
        });
    });
}

fn component_row(
    component: Component,
    installed: bool,
    busy: bool,
    view: InstallView,
    instance_id: String,
) -> Element {
    let id = component.id.clone();
    let progress_width = format!("width:{}%", (view.fraction * 100.0) as i32);
    let error_style = format!("color:{}", crate::theme::DANGER);
    rsx! {
        div { class: "rowline",
            div { class: "svc-id",
                div { class: "svc-name",
                    "{component.name}"
                    if installed {
                        span { class: "badge default", "已安装" }
                    }
                }
                div { class: "svc-meta", "{component.description}" }
            }
            span { class: "port", "{component.size_label()}" }
            if busy && !view.done {
                span { class: "hint", "{view.stage}" }
                div { class: "meter", style: "width:120px;flex:none",
                    i { style: "{progress_width}" }
                }
            } else if view.done && !view.ok {
                span { class: "hint", style: "{error_style}", "{view.stage}" }
            }
            if installed {
                button {
                    class: "soft-btn",
                    disabled: busy,
                    onclick: move |_| {
                        let mut app = use_context::<AppState>();
                        app.ask_danger(
                            "卸载组件",
                            format!("确定卸载 {instance_id}？\n会停止该服务并删除其安装目录（数据目录保留）。"),
                            "卸载",
                            Action::Uninstall { component: id.clone(), instance: instance_id.clone() },
                        );
                    },
                    Icon { name: "trash", size: 14 }
                    "卸载"
                }
            } else {
                button {
                    class: "btn",
                    disabled: busy,
                    onclick: move |_| {
                        let app = use_context::<AppState>();
                        let component = catalog()
                            .into_iter()
                            .find(|candidate| candidate.id == id)
                            .expect("catalog row");
                        install_component(app, component);
                    },
                    Icon { name: "download", size: 14 }
                    if busy { "处理中…" } else { "下载" }
                }
            }
        }
    }
}

#[component]
pub fn SoftwarePage() -> Element {
    let app = use_context::<AppState>();
    let snapshot = app.snapshot.read().clone();
    let installs = app.installs.read().clone();
    let installed_kinds: Vec<(String, String)> = snapshot
        .rows
        .iter()
        .map(|row| (row.instance.kind.clone(), row.instance.version.clone()))
        .collect();
    rsx! {
        div { class: "page",
            div { class: "card suite",
                div { class: "section-head",
                    span { class: "accent-bar" }
                    span { class: "section-title", "软件管理" }
                    span { class: "hint", "点击下载，装好后会出现在首页套件列表" }
                }
                div { class: "suite-body",
                    for component in catalog() {
                        {
                            let id = component.id.clone();
                            let installed = installed_kinds
                                .contains(&(component.kind.clone(), component.version.clone()));
                            let instance_id = ServiceInstance::make_id(&component.kind, &component.version);
                            let busy = app.is_busy(&id);
                            let view = installs.get(&id).cloned().unwrap_or_default();
                            component_row(component, installed, busy, view, instance_id)
                        }
                    }
                }
            }
        }
    }
}
