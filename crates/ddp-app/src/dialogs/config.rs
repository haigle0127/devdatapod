//! 服务配置对话框：端口 + 每种 kind 的字段，一次提交（settings → 端口迁移 → 可选重启）。

use dioxus::prelude::*;

use ddp_core::engine::Row;
use ddp_core::instance::RunState;
use serde_json::{Map, Value};

use crate::state::AppState;

fn field(label: &'static str, placeholder: &'static str, mut value: Signal<String>, mono: bool) -> Element {
    let class = if mono { "input mono" } else { "input" };
    rsx! {
        div { class: "field",
            label { "{label}" }
            div { class: "grow",
                input {
                    class: "{class}",
                    placeholder: "{placeholder}",
                    value: "{value}",
                    oninput: move |event| value.set(event.value()),
                }
            }
        }
    }
}

fn number_field(
    label: &'static str,
    hint: &'static str,
    mut value: Signal<String>,
) -> Element {
    rsx! {
        div { class: "field",
            label { "{label}" }
            div { class: "grow",
                input {
                    class: "input mono",
                    r#type: "number",
                    value: "{value}",
                    oninput: move |event| value.set(event.value()),
                }
                if !hint.is_empty() {
                    span { class: "field-hint", "{hint}" }
                }
            }
        }
    }
}

#[component]
pub fn ConfigDialog(id: String) -> Element {
    let mut app = use_context::<AppState>();
    let row: Option<Row> = app.snapshot.read().row(&id).cloned();
    let Some(row) = row else {
        app.dialog.set(None);
        return rsx! {};
    };
    let kind = row.instance.kind.clone();
    let is_default = row.instance.is_default;
    let current_port = row.port;
    let was_running = row.state == RunState::Running;

    // Initial settings come from the driver (actor round-trip), once per dialog open.
    let initial: Map<String, Value> = engine_settings(&id);
    let get = |key: &str, fallback: &str| -> String {
        initial
            .get(key)
            .and_then(Value::as_str)
            .map(str::to_string)
            .unwrap_or_else(|| fallback.to_string())
    };
    let get_num = |key: &str, fallback: i64| -> String {
        initial
            .get(key)
            .and_then(Value::as_i64)
            .map(|number| number.to_string())
            .unwrap_or_else(|| fallback.to_string())
    };

    let mut port = use_signal(|| current_port.to_string());
    let mut error = use_signal(String::new);
    let password = use_signal(|| get("password", ""));
    let max_memory = use_signal(|| get_num("maxMemoryMb", 256));
    let max_clients = use_signal(|| get_num("maxClients", 10000));
    let databases = use_signal(|| get_num("databases", 16));
    let root_password = use_signal(|| get("rootPassword", ""));
    let max_connections = use_signal(|| get_num("maxConnections", 151));
    let innodb_pool = use_signal(|| get_num("innodbBufferPoolMb", 128));
    let charset = use_signal(|| get("charset", "utf8mb4"));
    let workers = use_signal(|| get_num("workerProcesses", 1));
    let connections = use_signal(|| get_num("workerConnections", 1024));
    let root_dir = use_signal(|| get("rootDir", "WWW"));

    let name = row.instance.display_name();
    let title = format!("{name} 配置");
    rsx! {
        div { class: "overlay", onclick: move |_| use_context::<AppState>().dialog.set(None),
            div { class: "dialog", onclick: move |event| event.stop_propagation(),
                h3 { "{title}" }
                div { class: "field",
                    label { "端口" }
                    div { class: "grow",
                        input {
                            class: "input mono",
                            r#type: "number",
                            value: "{port}",
                            oninput: move |event| port.set(event.value()),
                        }
                        span { class: "field-hint",
                            "{star_text(kind.as_str(), is_default, current_port, port.read().clone())}"
                        }
                    }
                }
                match kind.as_str() {
                    "redis" => rsx! {
                        {field("密码", "留空 = 无密码", password, false)}
                        {number_field("最大内存", "MB", max_memory)}
                        {number_field("最大连接", "", max_clients)}
                        {number_field("数据库数", "", databases)}
                    },
                    "mysql" | "mariadb" => rsx! {
                        {field("root 密码", "", root_password, false)}
                        {number_field("最大连接", "", max_connections)}
                        {number_field("缓冲池", "MB · InnoDB", innodb_pool)}
                        {field("字符集", "如 utf8mb4", charset, true)}
                    },
                    "nginx" => rsx! {
                        {number_field("工作进程", "", workers)}
                        {number_field("最大连接", "每进程", connections)}
                        {field("站点目录", "WWW（相对安装目录，或填绝对路径）", root_dir, true)}
                    },
                    _ => rsx! {},
                }
                if !error.read().is_empty() {
                    div { class: "error", "{error}" }
                }
                div { class: "dialog-actions",
                    button {
                        class: "soft-btn",
                        onclick: move |_| use_context::<AppState>().dialog.set(None),
                        "取消"
                    }
                    button {
                        class: "btn",
                        onclick: move |_| {
                            let mut app = use_context::<AppState>();
                            let settings = match kind.as_str() {
                                "redis" => redis_settings(&password, &max_memory, &max_clients, &databases),
                                "mysql" | "mariadb" => mysql_settings(&root_password, &max_connections, &innodb_pool, &charset),
                                "nginx" => nginx_settings(&workers, &connections, &root_dir),
                                _ => Map::new(),
                            };
                            let new_port: Option<u16> = match port.read().trim().parse::<u16>() {
                                Ok(parsed) if parsed > 0 => Some(parsed),
                                _ => {
                                    error.set("端口必须是 1-65535 的整数".into());
                                    return;
                                }
                            };
                            let restart = app.settings.read().auto_restart && was_running;
                            match crate::state::engine().save_config(&id, Value::Object(settings), new_port, restart) {
                                Ok(()) => {
                                    app.dialog.set(None);
                                    app.toast("配置已保存");
                                }
                                Err(message) => error.set(message),
                            }
                        },
                        "确定"
                    }
                }
            }
        }
    }
}

fn engine_settings(id: &str) -> Map<String, Value> {
    crate::state::engine()
        .settings(id)
        .ok()
        .and_then(|value| value.as_object().cloned())
        .unwrap_or_default()
}

fn star_text(kind: &str, is_default: bool, current: u16, raw: String) -> String {
    let canonical = ddp_core::kinds::find(kind)
        .filter(|meta| !meta.canonical.is_empty())
        .and_then(|meta| {
            crate::state::engine()
                .snapshot()
                .ports
                .iter()
                .find(|view| view.key == meta.canonical)
                .map(|view| view.port)
        });
    let wanted: u16 = raw.trim().parse().unwrap_or(current);
    match (is_default, canonical) {
        (false, Some(canonical)) if wanted == canonical => "★ 将设为默认端口".into(),
        (true, Some(canonical)) if wanted != canonical => "将失去默认标记".into(),
        (true, _) => "★ 默认".into(),
        _ => "备用".into(),
    }
}

fn num(map: &mut Map<String, Value>, key: &str, raw: &str, fallback: i64) {
    let value = raw.trim().parse::<i64>().unwrap_or(fallback);
    map.insert(key.into(), Value::from(value));
}

fn redis_settings(
    password: &Signal<String>,
    memory: &Signal<String>,
    clients: &Signal<String>,
    databases: &Signal<String>,
) -> Map<String, Value> {
    let mut map = Map::new();
    map.insert("password".into(), Value::from(password.read().trim()));
    num(&mut map, "maxMemoryMb", &memory.read(), 256);
    num(&mut map, "maxClients", &clients.read(), 10000);
    num(&mut map, "databases", &databases.read(), 16);
    map
}

fn mysql_settings(
    password: &Signal<String>,
    connections: &Signal<String>,
    pool: &Signal<String>,
    charset: &Signal<String>,
) -> Map<String, Value> {
    let mut map = Map::new();
    map.insert("rootPassword".into(), Value::from(password.read().trim()));
    num(&mut map, "maxConnections", &connections.read(), 151);
    num(&mut map, "innodbBufferPoolMb", &pool.read(), 128);
    map.insert("charset".into(), Value::from(charset.read().trim()));
    map
}

fn nginx_settings(
    workers: &Signal<String>,
    connections: &Signal<String>,
    root_dir: &Signal<String>,
) -> Map<String, Value> {
    let mut map = Map::new();
    num(&mut map, "workerProcesses", &workers.read(), 1);
    num(&mut map, "workerConnections", &connections.read(), 1024);
    map.insert("rootDir".into(), Value::from(root_dir.read().trim()));
    map
}
