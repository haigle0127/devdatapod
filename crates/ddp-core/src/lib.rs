//! ddp-core — the UI-free half of devdatapod.
//!
//! Nothing in this crate may depend on a windowing toolkit: the engine is meant to be
//! embeddable behind an IPC boundary later (see docs/REWRITE-PLAN.md).

pub mod atomic;
pub mod catalog;
pub mod download;
pub mod driver;
pub mod engine;
pub mod error;
pub mod install;
pub mod instance;
pub mod ipc;
pub mod kinds;
pub mod log_tail;
pub mod managed_block;
pub mod orphan;
pub mod paths;
pub mod ports;
pub mod probe;
pub mod procstat;
pub mod registry;
pub mod run_once;
pub mod selftest;
pub mod settings;
pub mod sites;
pub mod stage;
pub mod supervisor;
pub mod switcher;
pub mod sysstat;

pub mod log {
    //! Panel log sink. Re-exported under `ddp_core::log` to mirror the Qt layout.

    use crate::paths::Paths;
    use chrono::Local;
    use std::fs::{self, File, OpenOptions};
    use std::io::Write;
    use std::path::PathBuf;
    use std::sync::{Mutex, OnceLock};

    const MAX_BYTES: u64 = 2 * 1024 * 1024;
    const MAX_FILES: u32 = 3;

    struct Logger {
        path: PathBuf,
        file: File,
    }

    static LOGGER: OnceLock<Mutex<Option<Logger>>> = OnceLock::new();

    fn slot() -> &'static Mutex<Option<Logger>> {
        LOGGER.get_or_init(|| Mutex::new(None))
    }

    fn rotate(path: &std::path::Path) {
        let _ = fs::remove_file(path.with_extension("log.3"));
        for index in (1..MAX_FILES).rev() {
            let from = path.with_extension(format!("log.{index}"));
            let to = path.with_extension(format!("log.{}", index + 1));
            let _ = fs::rename(from, to);
        }
        let _ = fs::rename(path, path.with_extension("log.1"));
    }

    /// Open `logs/devdatapod.log`. Safe to call twice; the second call is a no-op.
    pub fn init(paths: &Paths) -> bool {
        let mut guard = match slot().lock() {
            Ok(guard) => guard,
            Err(poisoned) => poisoned.into_inner(),
        };
        if guard.is_some() {
            return true;
        }
        if fs::create_dir_all(paths.logs_dir()).is_err() {
            return false;
        }
        let path = paths.logs_dir().join("devdatapod.log");
        if let Ok(meta) = fs::metadata(&path) {
            if meta.len() > MAX_BYTES {
                rotate(&path);
            }
        }
        match OpenOptions::new().create(true).append(true).open(&path) {
            Ok(file) => {
                *guard = Some(Logger { path, file });
                true
            }
            Err(_) => false,
        }
    }

    pub fn shutdown() {
        if let Ok(mut guard) = slot().lock() {
            if let Some(mut logger) = guard.take() {
                let _ = logger.file.flush();
            }
        }
    }

    fn write(level: &str, message: &str) {
        let line = format!(
            "[{}] [{}] {}\n",
            Local::now().format("%Y-%m-%d %H:%M:%S%.3f"),
            level,
            message
        );
        if let Ok(mut guard) = slot().lock() {
            if let Some(logger) = guard.as_mut() {
                let _ = logger.file.write_all(line.as_bytes());
                let _ = logger.file.flush();
                if let Ok(meta) = logger.file.metadata() {
                    if meta.len() > MAX_BYTES {
                        let path = logger.path.clone();
                        let _ = logger.file.flush();
                        rotate(&path);
                        if let Ok(file) = OpenOptions::new().create(true).append(true).open(&path) {
                            logger.file = file;
                        }
                    }
                }
            }
        }
        // A failed log write must never take the panel down; stderr is the fallback.
        #[cfg(debug_assertions)]
        eprint!("{line}");
    }

    pub fn info(message: &str) {
        write("info", message);
    }
    pub fn warn(message: &str) {
        write("warning", message);
    }
    pub fn error(message: &str) {
        write("error", message);
    }
    pub fn debug(message: &str) {
        write("debug", message);
    }
}
