//! The product's heart: one implementation per service kind.
//!
//! Every method takes an *instance*. There is deliberately no kind-level "start MySQL" entry
//! point — a request that vague is resolved against the registry by the caller first.
//!
//! Contract every driver honours:
//!   * `start` returns as soon as the child is spawned; readiness is decided by `status`,
//!     which probes the port and speaks the protocol, because "process exists" is not
//!     "service is up";
//!   * `stop` is graceful first (the service's own shutdown verb) and only escalates to
//!     killing the container after a timeout;
//!   * nothing here touches a widget — core is UI-free (AGENTS.md rule 3).
//!
//! Ports always come from the caller (PortAllocator). A driver never invents a number.

pub mod mysql;
pub mod nginx;
pub mod redis;

use std::path::PathBuf;

use serde_json::{Map, Value};

use crate::instance::{RunState, ServiceInstance};
use crate::log;
use crate::log_tail::LogTail;
use crate::paths::Paths;
use crate::procstat::{ResourceSample, ResourceSampler};

/// What `detect()` learned about an installed version.
#[derive(Clone, Debug, Default)]
pub struct Detection {
    pub installed: bool,
    pub executable: PathBuf,
    /// As reported by the binary, not as filed.
    pub version: String,
    pub message: String,
}

#[derive(Clone, Debug)]
pub struct ServiceStatus {
    pub state: RunState,
    pub pid: u32,
    /// Shown verbatim in the failure tooltip.
    pub detail: String,
}

impl Default for ServiceStatus {
    fn default() -> Self {
        ServiceStatus {
            state: RunState::Stopped,
            pid: 0,
            detail: String::new(),
        }
    }
}

#[derive(Clone, Copy, Debug, Default)]
pub struct ServiceStats {
    pub valid: bool,
    /// Across every process in the instance, not system-wide.
    pub cpu_percent: f64,
    pub rss_bytes: u64,
    pub uptime_ms: u64,
}

pub trait ServiceDriver {
    fn instance(&self) -> &ServiceInstance;
    fn port(&self) -> u16;

    fn detect(&mut self) -> Detection;
    fn prepare_config(&mut self) -> Result<(), String>;
    fn start(&mut self) -> Result<(), String>;
    fn stop(&mut self) -> Result<(), String>;
    fn status(&mut self) -> ServiceStatus;
    fn logs(&self, lines: usize) -> LogTail;
    fn stats(&mut self) -> ServiceStats;

    /// Make a running service re-read its configuration without dropping connections. The
    /// default is "not supported", so a driver with no such verb is not forced to write an
    /// empty shell.
    fn reload(&mut self) -> Result<(), String> {
        Err(format!("{} 不支持热重载", self.instance().id))
    }

    /// Absolute install directory (`Extensions/<kind><version>`).
    fn install_root(&self) -> PathBuf;
    /// Absolute per-instance data directory.
    fn data_root(&self) -> PathBuf;

    /// Current settings as the JSON shape stored in `data/instances/.../<kind>.settings.json`.
    fn settings_json(&self) -> Value;
    /// Apply a settings object (unknown keys ignored, ranges clamped), persist it, and
    /// re-render the service config. Does not restart anything.
    fn apply_settings(&mut self, value: &Value) -> Result<(), String>;
}

/// Build the driver for an instance, or explain why this kind has none.
pub fn create(
    paths: &Paths,
    instance: &ServiceInstance,
    port: u16,
) -> Result<Box<dyn ServiceDriver>, String> {
    match instance.kind.as_str() {
        "nginx" => Ok(Box::new(nginx::NginxDriver::new(paths.clone(), instance.clone(), port))),
        "mysql" => Ok(Box::new(mysql::MysqlDriver::new(paths.clone(), instance.clone(), port))),
        "redis" => Ok(Box::new(redis::RedisDriver::new(paths.clone(), instance.clone(), port))),
        other => Err(format!(
            "{} 还没有驱动（kind = {other}）",
            instance.id
        )),
    }
}

// ------------------------------------------------------------------ shared driver plumbing

/// Render a settings file from only the keys we know. A typo in the file therefore cannot
/// silently change what gets rendered into the service config.
pub(crate) fn load_settings(path: &PathBuf) -> Map<String, Value> {
    if !path.exists() {
        return Map::new();
    }
    match std::fs::read(path) {
        Ok(raw) => match serde_json::from_slice::<Value>(&raw) {
            Ok(value) => value.as_object().cloned().unwrap_or_default(),
            Err(error) => {
                log::warn(&format!("忽略 {}（{error}）", path.display()));
                Map::new()
            }
        },
        Err(error) => {
            log::warn(&format!("{}：{error}", path.display()));
            Map::new()
        }
    }
}

pub(crate) fn save_settings(path: &PathBuf, object: Map<String, Value>) -> bool {
    let mut bytes = serde_json::to_vec_pretty(&Value::Object(object)).unwrap_or_default();
    bytes.push(b'\n');
    crate::atomic::write_atomic(path, &bytes)
}

pub(crate) fn get_str(map: &Map<String, Value>, key: &str, fallback: &str) -> String {
    map.get(key)
        .and_then(Value::as_str)
        .unwrap_or(fallback)
        .to_string()
}

pub(crate) fn get_i64(map: &Map<String, Value>, key: &str, fallback: i64) -> i64 {
    map.get(key).and_then(Value::as_i64).unwrap_or(fallback)
}

pub(crate) fn get_bool(map: &Map<String, Value>, key: &str, fallback: bool) -> bool {
    map.get(key).and_then(Value::as_bool).unwrap_or(fallback)
}

/// `D:\a b\` -> `D:/a b/`; backslashes are escape characters in nginx.conf and my.ini.
pub(crate) fn to_conf_path(path: &std::path::Path) -> String {
    path.to_string_lossy().replace('\\', "/")
}

/// Trim a log tail down to a startup-failure message. `error.log` is written by nginx
/// itself and preferred; only when it is empty does the captured console log win.
pub(crate) fn startup_failure(error_log: &std::path::Path, console_log: &std::path::Path) -> String {
    let tail = crate::log_tail::read_tail(error_log, 12, 64 * 1024);
    let mut text = tail.text;
    if text.trim().is_empty() {
        text = crate::log_tail::read_tail(console_log, 12, 64 * 1024).text;
    }
    text.trim().to_string()
}

/// Shared bookkeeping: clock, "we stopped it ourselves" flag, resource sampler.
pub(crate) struct Runtime {
    pub supervisor: crate::supervisor::Supervisor,
    pub sampler: ResourceSampler,
    pub stopped_by_us: bool,
    pub last_start: Option<std::time::Instant>,
}

impl Runtime {
    pub fn new(label: &str) -> Self {
        Runtime {
            supervisor: crate::supervisor::Supervisor::new(label),
            sampler: ResourceSampler::default(),
            stopped_by_us: false,
            last_start: None,
        }
    }

    pub fn mark_started(&mut self) {
        self.stopped_by_us = false;
        self.last_start = Some(std::time::Instant::now());
        self.sampler.reset();
    }

    pub fn mark_stopped(&mut self) {
        self.stopped_by_us = true;
    }

    pub fn stats(&mut self) -> ServiceStats {
        let sample: ResourceSample = self.sampler.sample(&self.supervisor.members());
        let mut out = ServiceStats {
            valid: sample.valid,
            cpu_percent: sample.cpu_percent,
            rss_bytes: sample.rss_bytes,
            uptime_ms: 0,
        };
        if self.supervisor.is_running() {
            if let Some(started) = self.last_start {
                out.uptime_ms = started.elapsed().as_millis() as u64;
            }
        }
        out
    }

    /// `status` logic shared by all three: a stopped-by-us or never-started child is
    /// `Stopped`; a child that exited on its own is `Crashed`; a live child that does not
    /// answer is `Starting` for `ready_timeout_ms`, then `Crashed`.
    pub fn classify(
        &mut self,
        probe_ok: bool,
        detail: String,
        ready_timeout_ms: u64,
    ) -> ServiceStatus {
        let mut status = ServiceStatus::default();
        if !self.supervisor.is_running() {
            if self.stopped_by_us || self.last_start.is_none() {
                status.state = RunState::Stopped;
                return status;
            }
            status.state = RunState::Crashed;
            status.detail = format!("退出码 {}", self.supervisor.exit_code());
            return status;
        }
        status.pid = self.supervisor.pid();
        if probe_ok {
            status.state = RunState::Running;
            return status;
        }
        status.detail = detail;
        let waited = self
            .last_start
            .map(|started| started.elapsed().as_millis() as u64)
            .unwrap_or(ready_timeout_ms + 1);
        status.state = if waited < ready_timeout_ms {
            RunState::Starting
        } else {
            RunState::Crashed
        };
        status
    }
}
