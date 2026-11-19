//! Signals the whole UI reads from, plus the handful of process-wide statics that outlive a
//! render (engine handle, single-instance guard, in-flight installs, worker messages).
//!
//! `AppState` is provided through Dioxus context from the root component; worker threads
//! (engine commands, installers) never touch signals, they push `WorkerMessage`s that the
//! root poll loop drains.

use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, OnceLock};

use ddp_core::engine::{Engine, Snapshot};
use ddp_core::paths::Paths;
use ddp_core::sites::Site;
use dioxus::prelude::*;

pub static PATHS: OnceLock<Paths> = OnceLock::new();
pub static ENGINE: OnceLock<Engine> = OnceLock::new();
pub static GUARD: OnceLock<Mutex<Option<ddp_core::ipc::Guard>>> = OnceLock::new();
pub static QUIT: AtomicBool = AtomicBool::new(false);

pub fn paths() -> &'static Paths {
    PATHS.get().expect("paths initialised in main")
}

pub fn engine() -> &'static Engine {
    ENGINE.get().expect("engine initialised in main")
}

pub fn set_guard(guard: ddp_core::ipc::Guard) {
    *GUARD
        .get_or_init(|| Mutex::new(None))
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner()) = Some(guard);
}

/// Returns true when a second launch asked this process to come to the front.
pub fn take_activation() -> bool {
    GUARD
        .get()
        .and_then(|slot| {
            slot.lock()
                .ok()
                .and_then(|guard| guard.as_ref().map(|guard| guard.take_activation()))
        })
        .unwrap_or(false)
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Page {
    Home,
    Sites,
    Software,
    Settings,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum HomeTab {
    Status,
    Resource,
    Log,
}

#[derive(Clone, Debug, PartialEq)]
pub enum Action {
    Start(String),
    Stop(String),
    Restart(String),
    Promote(String),
    Uninstall { component: String, instance: String },
    DeleteSite(String),
}

#[derive(Clone, Debug, PartialEq)]
pub enum Dialog {
    /// A modal question. `action` runs when confirmed.
    Confirm {
        title: String,
        message: String,
        confirm: String,
        danger: bool,
        action: Action,
    },
    Config { id: String },
    Site { domain: Option<String> },
}

#[derive(Clone, Debug, PartialEq)]
pub struct Toast {
    pub text: String,
    pub error: bool,
    /// `chrono::Local::now().timestamp()`; the poll loop hides the toast after a while.
    pub created: i64,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct InstallView {
    pub stage: String,
    pub fraction: f64,
    pub done: bool,
    pub ok: bool,
    pub error: String,
}

pub static INSTALLS: OnceLock<Mutex<HashMap<String, InstallView>>> = OnceLock::new();

pub fn install_snapshot() -> HashMap<String, InstallView> {
    INSTALLS
        .get()
        .and_then(|slot| slot.lock().ok().map(|map| map.clone()))
        .unwrap_or_default()
}

pub fn set_install(id: &str, view: InstallView) {
    if let Ok(mut map) = INSTALLS.get_or_init(|| Mutex::new(HashMap::new())).lock() {
        map.insert(id.to_string(), view);
    }
}

/// Messages from worker threads (engine commands, installer) back to the UI loop.
#[derive(Clone, Debug)]
pub struct WorkerMessage {
    pub ok: bool,
    pub text: String,
    /// Instance id whose busy flag should clear.
    pub id: Option<String>,
}

pub static MESSAGES: OnceLock<Mutex<Vec<WorkerMessage>>> = OnceLock::new();

pub fn push_message(message: WorkerMessage) {
    if let Ok(mut queue) = MESSAGES.get_or_init(|| Mutex::new(Vec::new())).lock() {
        queue.push(message);
    }
}

pub fn drain_messages() -> Vec<WorkerMessage> {
    MESSAGES
        .get()
        .and_then(|slot| slot.lock().ok().map(|mut queue| std::mem::take(&mut *queue)))
        .unwrap_or_default()
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct SysView {
    pub valid: bool,
    pub cpu: f64,
    pub mem: f64,
    pub mem_used_gb: f64,
    pub mem_total_gb: f64,
    pub avail_gb: f64,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SettingsView {
    pub auto_restart: bool,
    pub close_to_tray: bool,
}

pub fn load_settings() -> SettingsView {
    let mut store = ddp_core::settings::SettingsStore::under(paths().root());
    store.load();
    SettingsView {
        auto_restart: store.bool_value("ui/autoRestartOnConfig", true),
        close_to_tray: store.bool_value("ui/closeToTray", true),
    }
}

pub fn store_setting(key: &str, value: bool) -> bool {
    let mut store = ddp_core::settings::SettingsStore::under(paths().root());
    store.load();
    store.update(key, if value { "true" } else { "false" })
}

#[derive(Clone, Copy)]
pub struct AppState {
    pub page: Signal<Page>,
    pub home_tab: Signal<HomeTab>,
    pub snapshot: Signal<Arc<Snapshot>>,
    pub sites: Signal<Vec<Site>>,
    pub sys: Signal<SysView>,
    pub log_text: Signal<String>,
    pub busy: Signal<Vec<String>>,
    pub dialog: Signal<Option<Dialog>>,
    pub toast: Signal<Option<Toast>>,
    pub settings: Signal<SettingsView>,
    pub installs: Signal<HashMap<String, InstallView>>,
    pub site_query: Signal<String>,
    pub update_status: Signal<String>,
    pub update_busy: Signal<bool>,
    pub version: Signal<String>,
}

impl AppState {
    pub fn new() -> Self {
        AppState {
            page: Signal::new(Page::Home),
            home_tab: Signal::new(HomeTab::Status),
            snapshot: Signal::new(Arc::new(Snapshot::default())),
            sites: Signal::new(Vec::new()),
            sys: Signal::new(SysView::default()),
            log_text: Signal::new(String::new()),
            busy: Signal::new(Vec::new()),
            dialog: Signal::new(None),
            toast: Signal::new(None),
            settings: Signal::new(load_settings()),
            installs: Signal::new(install_snapshot()),
            site_query: Signal::new(String::new()),
            update_status: Signal::new(String::new()),
            update_busy: Signal::new(false),
            version: Signal::new(env!("CARGO_PKG_VERSION").to_string()),
        }
    }

    pub fn is_busy(&self, id: &str) -> bool {
        self.busy.read().iter().any(|item| item == id)
    }

    pub fn mark_busy(&mut self, id: &str) {
        let mut list = self.busy.write();
        if !list.iter().any(|item| item == id) {
            list.push(id.to_string());
        }
    }

    pub fn clear_busy(&mut self, id: &str) {
        self.busy.write().retain(|item| item != id);
    }

    pub fn toast(&mut self, text: impl Into<String>) {
        self.toast.set(Some(Toast {
            text: text.into(),
            error: false,
            created: chrono::Local::now().timestamp(),
        }));
    }

    pub fn problem(&mut self, text: impl Into<String>) {
        self.toast.set(Some(Toast {
            text: text.into(),
            error: true,
            created: chrono::Local::now().timestamp(),
        }));
    }

    pub fn ask(&mut self, title: &str, message: impl Into<String>, confirm: &str, action: Action) {
        self.dialog.set(Some(Dialog::Confirm {
            title: title.to_string(),
            message: message.into(),
            confirm: confirm.to_string(),
            danger: false,
            action,
        }));
    }

    pub fn ask_danger(
        &mut self,
        title: &str,
        message: impl Into<String>,
        confirm: &str,
        action: Action,
    ) {
        self.dialog.set(Some(Dialog::Confirm {
            title: title.to_string(),
            message: message.into(),
            confirm: confirm.to_string(),
            danger: true,
            action,
        }));
    }

    pub fn reload_sites(&mut self) {
        let (sites, _) = ddp_core::sites::load(paths().root());
        self.sites.set(sites);
    }
}

/// Run an engine command on a worker thread; the result comes back as a `WorkerMessage`.
pub fn engine_action(mut app: AppState, id: &str, verb: &str, action: Action) {
    app.mark_busy(id);
    let id_string = id.to_string();
    let verb = verb.to_string();
    std::thread::spawn(move || {
        let result = match &action {
            Action::Start(id) => engine().start(id),
            Action::Stop(id) => engine().stop(id),
            Action::Restart(id) => engine().restart(id),
            Action::Promote(id) => engine().switch_default(id),
            _ => Err("未知的操作".to_string()),
        };
        let text = match &result {
            Ok(()) => format!("{verb}成功"),
            Err(error) => format!("{verb}失败：{error}"),
        };
        push_message(WorkerMessage {
            ok: result.is_ok(),
            text,
            id: Some(id_string),
        });
    });
}

pub fn quit_now() {
    QUIT.store(true, Ordering::Relaxed);
    std::thread::spawn(|| {
        engine().shutdown();
        std::process::exit(0);
    });
}
