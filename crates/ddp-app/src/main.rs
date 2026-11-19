//! devdatapod desktop panel — Dioxus rewrite of the Qt shell.
//!
//! Startup order matters and mirrors the Qt build:
//! 1. resolve the green install root, init the log file
//! 2. single-instance guard (loopback ping wakes the running panel)
//! 3. orphan sweep by install-root path prefix (AGENTS.md rule 5)
//! 4. spawn the engine actor
//! 5. tray icon + menu, then the window

mod components;
mod dialogs;
mod icons;
mod pages;
mod state;
mod sysinfo;
mod theme;

use std::sync::{Mutex, OnceLock};

use dioxus::desktop::{Config, LogicalSize, WindowBuilder};
use dioxus::prelude::*;

use state::{AppState, Page, WorkerMessage};

// --------------------------------------------------------------------------- app icon

const LOGO_ICO: &[u8] = include_bytes!("../assets/logo.ico");

/// Extract one image entry from a multi-size .ico (entries here are PNG-compressed).
/// `image::load_from_memory` on an ico picks whatever entry it likes, and at tray sizes a
/// wrongly-sized source turns into mud — we want the 48px one.
fn ico_entry(width: u8) -> Option<&'static [u8]> {
    if LOGO_ICO.len() < 6 {
        return None;
    }
    let count = u16::from_le_bytes([LOGO_ICO[4], LOGO_ICO[5]]) as usize;
    for index in 0..count {
        let base = 6 + index * 16;
        if base + 16 > LOGO_ICO.len() {
            break;
        }
        if LOGO_ICO[base] != width {
            continue;
        }
        let size = u32::from_le_bytes([
            LOGO_ICO[base + 8],
            LOGO_ICO[base + 9],
            LOGO_ICO[base + 10],
            LOGO_ICO[base + 11],
        ]) as usize;
        let offset = u32::from_le_bytes([
            LOGO_ICO[base + 12],
            LOGO_ICO[base + 13],
            LOGO_ICO[base + 14],
            LOGO_ICO[base + 15],
        ]) as usize;
        if offset.checked_add(size)? <= LOGO_ICO.len() {
            return Some(&LOGO_ICO[offset..offset + size]);
        }
    }
    None
}

/// The panel logo as RGBA at a chosen size, shared by the window icon and the tray icon.
/// The tray is 16px at 96 DPI — feeding it the 48px art makes Windows' downscale muddy.
fn logo_rgba(entry: u8) -> Option<(Vec<u8>, u32, u32)> {
    let entry = ico_entry(entry).or_else(|| ico_entry(32))?;
    let image = image::load_from_memory(entry).ok()?.to_rgba8();
    let (width, height) = image.dimensions();
    Some((image.into_raw(), width, height))
}

fn window_icon() -> Option<dioxus::desktop::tao::window::Icon> {
    let (rgba, width, height) = logo_rgba(48)?;
    dioxus::desktop::tao::window::Icon::from_rgba(rgba, width, height).ok()
}

/// The logo as a PNG data URI for `<img src>` (title bar / sidebar brand). Decoded and
/// encoded once, then cached for the process lifetime.
fn logo_data_uri() -> &'static str {
    static URI: std::sync::OnceLock<String> = std::sync::OnceLock::new();
    URI.get_or_init(|| {
        let fallback = "data:image/png;base64,".to_string();
        let Some((rgba, width, height)) = logo_rgba(48) else {
            return fallback;
        };
        let image = image::DynamicImage::ImageRgba8(
            image::ImageBuffer::from_raw(width, height, rgba)
                .unwrap_or_else(|| image::ImageBuffer::new(1, 1)),
        );
        let mut bytes = std::io::Cursor::new(Vec::new());
        if image.write_to(&mut bytes, image::ImageFormat::Png).is_err() {
            return fallback;
        }
        format!("data:image/png;base64,{}", base64(&bytes.into_inner()))
    })
}

// --------------------------------------------------------------------------- tray plumbing

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum TrayCommand {
    Show,
    Quit,
}

static TRAY_COMMANDS: OnceLock<Mutex<Vec<TrayCommand>>> = OnceLock::new();

fn tray_commands() -> &'static Mutex<Vec<TrayCommand>> {
    TRAY_COMMANDS.get_or_init(|| Mutex::new(Vec::new()))
}

fn push_tray(command: TrayCommand) {
    if let Ok(mut queue) = tray_commands().lock() {
        queue.push(command);
    }
}

fn drain_tray() -> Vec<TrayCommand> {
    tray_commands()
        .lock()
        .map(|mut queue| std::mem::take(&mut *queue))
        .unwrap_or_default()
}

fn tray_icon_rgba() -> (Vec<u8>, u32, u32) {
    if let Some(logo) = logo_rgba(32) {
        return logo;
    }
    // Fallback only if the bundled ico ever fails to decode: rounded purple square.
    const SIZE: u32 = 32;
    let mut rgba = vec![0u8; (SIZE * SIZE * 4) as usize];
    for y in 0..SIZE {
        for x in 0..SIZE {
            let index = ((y * SIZE + x) * 4) as usize;
            // rounded-corner purple square with a soft diagonal gradient
            let corner = (x < 6 && y < 6) || (x >= SIZE - 6 && y < 6)
                || (x < 6 && y >= SIZE - 6) || (x >= SIZE - 6 && y >= SIZE - 6);
            let inside_corner = corner
                && ((x < 6 && y < 6 && x + y < 6)
                    || (x >= SIZE - 6 && y < 6 && (SIZE - 1 - x) + y < 6)
                    || (x < 6 && y >= SIZE - 6 && x + (SIZE - 1 - y) < 6)
                    || (x >= SIZE - 6 && y >= SIZE - 6 && (SIZE - 1 - x) + (SIZE - 1 - y) < 6));
            if corner && !inside_corner {
                continue;
            }
            let t = (x + y) as f32 / (2.0 * SIZE as f32);
            rgba[index] = (108.0 + (167.0 - 108.0) * t) as u8;
            rgba[index + 1] = (92.0 + (139.0 - 92.0) * t) as u8;
            rgba[index + 2] = (231.0 + (250.0 - 231.0) * t) as u8;
            rgba[index + 3] = 255;
        }
    }
    (rgba, SIZE, SIZE)
}

fn setup_tray() {
    use tray_icon::menu::{Menu, MenuEvent, MenuItem, PredefinedMenuItem};
    use tray_icon::{TrayIconBuilder, TrayIconEvent};

    let show_item = MenuItem::with_id("ddp.show", "显示面板", true, None);
    let quit_item = MenuItem::with_id("ddp.quit", "退出", true, None);
    let separator = PredefinedMenuItem::separator();
    let menu = Menu::with_items(&[&show_item, &separator, &quit_item]).expect("tray menu");
    let (rgba, width, height) = tray_icon_rgba();
    let icon = tray_icon::Icon::from_rgba(rgba, width, height).expect("tray icon");

    TrayIconEvent::set_event_handler(Some(|event| {
        if let TrayIconEvent::DoubleClick { .. } = event {
            push_tray(TrayCommand::Show);
        }
    }));
    MenuEvent::set_event_handler(Some(|event: tray_icon::menu::MenuEvent| {
        match event.id.0.as_str() {
            "ddp.show" => push_tray(TrayCommand::Show),
            "ddp.quit" => push_tray(TrayCommand::Quit),
            _ => {}
        }
    }));

    let _tray = TrayIconBuilder::new()
        .with_tooltip("devdatapod")
        .with_icon(icon)
        .with_menu(Box::new(menu))
        .build()
        .expect("tray icon");
    // The tray must outlive the event loop; leak it like the Qt build's static tray object.
    std::mem::forget(_tray);
}

// ----------------------------------------------------------------------------- window head

const BASE64: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

fn base64(data: &[u8]) -> String {
    let mut out = String::with_capacity(data.len() / 3 * 4 + 4);
    for chunk in data.chunks(3) {
        let b0 = chunk[0] as u32;
        let b1 = chunk.get(1).copied().unwrap_or(0) as u32;
        let b2 = chunk.get(2).copied().unwrap_or(0) as u32;
        let n = (b0 << 16) | (b1 << 8) | b2;
        out.push(BASE64[(n >> 18) as usize & 63] as char);
        out.push(BASE64[(n >> 12) as usize & 63] as char);
        out.push(if chunk.len() > 1 { BASE64[(n >> 6) as usize & 63] as char } else { '=' });
        out.push(if chunk.len() > 2 { BASE64[n as usize & 63] as char } else { '=' });
    }
    out
}

fn font_face(family: &str, weight: u32, bytes: &[u8]) -> String {
    format!(
        "@font-face {{ font-family: '{family}'; font-weight: {weight}; \
         font-display: swap; src: url(data:font/ttf;base64,{}) format('truetype'); }}",
        base64(bytes)
    )
}

fn custom_head() -> String {
    let regular = include_bytes!("../assets/fonts/MiSans-Regular.ttf");
    let medium = include_bytes!("../assets/fonts/MiSans-Medium.ttf");
    format!(
        "<style>{}</style><style>{}</style><style>{}</style>",
        font_face("MiSans", 400, regular),
        font_face("MiSans", 500, medium),
        theme::STYLESHEET
    )
}

// ------------------------------------------------------------------------------------- app

fn main() {
    // A panic on any thread (engine actor, installers) lands in the panel log — the default
    // stderr-only hook is invisible for a windowed app started from Explorer.
    std::panic::set_hook(Box::new(|info| {
        ddp_core::log::error(&format!("panic: {info}"));
        eprintln!("panic: {info}");
    }));

    let paths = ddp_core::paths::Paths::discover();
    let _ = state::PATHS.set(paths.clone());
    if !paths.ensure_writable_dirs() {
        eprintln!("devdatapod: cannot create data/logs under {}", paths.root().display());
    }
    ddp_core::log::init(&paths);
    ddp_core::log::info(&format!("devdatapod {} starting", env!("CARGO_PKG_VERSION")));

    // Single instance: the socket is the authority; on conflict we ping the running panel
    // and exit so the user lands on the existing window.
    let ipc_port = ddp_core::ipc::panel_port(&paths, "ipc");
    match ddp_core::ipc::acquire(&paths, ipc_port.max(0) as u16) {
        Ok(guard) => state::set_guard(guard),
        Err(()) => {
            if ipc_port > 0 {
                ddp_core::ipc::request_activation(ipc_port as u16);
            }
            ddp_core::log::info("another panel instance is running; activated it and exiting");
            return;
        }
    }

    // Kill whatever a crashed predecessor left behind (job object covers the normal path).
    let (killed, report) = ddp_core::orphan::sweep_install_root(paths.root());
    for line in report {
        ddp_core::log::warn(&line);
    }
    if killed > 0 {
        ddp_core::log::warn(&format!("orphan sweep: killed {killed} leftover process(es)"));
    }

    let engine = ddp_core::engine::Engine::spawn(paths);
    let _ = state::ENGINE.set(engine);
    ddp_core::log::info("engine actor started");

    setup_tray();

    let mut config = Config::new()
        .with_window(
            WindowBuilder::new()
                .with_title("devdatapod")
                .with_decorations(false)
                .with_resizable(true)
                .with_inner_size(LogicalSize::new(1280.0, 800.0))
                .with_min_inner_size(LogicalSize::new(1080.0, 720.0)),
        )
        .with_custom_head(custom_head());
    if let Some(icon) = window_icon() {
        config = config.with_icon(icon);
    }

    LaunchBuilder::desktop().with_cfg(config).launch(App);
}

#[component]
fn App() -> Element {
    use_context_provider(AppState::new);
    let mut app = use_context::<AppState>();

    use_hook(move || {
        app.reload_sites();
    });

    use_future(move || async move {
        let mut sampler = ddp_core::sysstat::Sampler::default();
        loop {
            // Engine snapshot drives rows, ports, readiness.
            let snapshot = state::engine().snapshot();
            app.snapshot.set(snapshot);

            // Machine counters (CPU is a delta, so sample on a steady cadence).
            let sample = sampler.sample();
            app.sys.set(state::SysView {
                valid: sample.valid,
                cpu: sample.cpu_fraction,
                mem: sample.memory_fraction(),
                mem_used_gb: (sample.total_phys_bytes.saturating_sub(sample.avail_phys_bytes))
                    as f64
                    / 1073741824.0,
                mem_total_gb: sample.total_phys_bytes as f64 / 1073741824.0,
                avail_gb: sample.avail_phys_bytes as f64 / 1073741824.0,
            });

            // Worker results: toasts + busy flags; id-less messages are update-check status.
            for message in state::drain_messages() {
                handle_message(&mut app, message);
            }

            // Installer progress mirror.
            app.installs.set(state::install_snapshot());

            // Second launch asked us to come forward.
            if state::take_activation() {
                show_window();
            }
            for command in drain_tray() {
                match command {
                    TrayCommand::Show => show_window(),
                    TrayCommand::Quit => state::quit_now(),
                }
            }

            // Toast lifetime.
            let now = chrono::Local::now().timestamp();
            let toast_expired = app
                .toast
                .read()
                .as_ref()
                .map(|toast| now - toast.created > 6)
                .unwrap_or(false);
            if toast_expired {
                app.toast.set(None);
            }

            // Log tail only while the tab is visible.
            if *app.page.read() == Page::Home && *app.home_tab.read() == state::HomeTab::Log {
                let tail = ddp_core::log_tail::read_tail(
                    &state::paths().logs_dir().join("devdatapod.log"),
                    200,
                    256 * 1024,
                );
                let text = tail.text.trim().to_string();
                app.log_text.set(if text.is_empty() {
                    "（还没有日志）".into()
                } else {
                    text
                });
            }

            tokio::time::sleep(std::time::Duration::from_millis(1500)).await;
        }
    });

    let page = *app.page.read();
    rsx! {
        div { class: "app",
            components::TitleBar {}
            div { class: "body",
                components::Sidebar {}
                div { class: "content",
                    match page {
                        Page::Home => rsx! { pages::home::HomePage {} },
                        Page::Sites => rsx! { pages::sites::SitesPage {} },
                        Page::Software => rsx! { pages::software::SoftwarePage {} },
                        Page::Settings => rsx! { pages::settings::SettingsPage {} },
                    }
                    components::StatusBar {}
                }
            }
            components::DialogHost {}
            components::ToastHost {}
        }
    }
}

fn handle_message(app: &mut AppState, message: WorkerMessage) {
    match message.id {
        Some(id) => {
            app.clear_busy(&id);
            if message.ok {
                app.toast(message.text);
            } else {
                app.problem(message.text);
            }
        }
        None => {
            app.update_busy.set(false);
            app.update_status.set(message.text.clone());
            if !message.ok {
                app.problem(message.text);
            }
            // Site save/delete results come back id-less; refresh the table only after the
            // engine has actually written sites.json (avoids racing the worker thread).
            app.reload_sites();
        }
    }
}

fn show_window() {
    let window = dioxus::desktop::window();
    window.set_minimized(false);
    window.set_visible(true);
    let _ = window.webview.focus();
}
