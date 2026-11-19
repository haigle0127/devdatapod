//! Redis: `redis.conf` with a managed block, an inline `SHUTDOWN SAVE` for a clean exit.
//!
//! The 3.0.504 branch is why this driver is not just a template: it has no HELLO, no ACL,
//! and no recent `maxmemory-policy` values, so the renderer leaves fields out entirely
//! instead of writing them and hoping the server accepts them.

use std::io::{Read, Write};
use std::net::{IpAddr, SocketAddr, TcpStream};
use std::path::{Path, PathBuf};
use std::time::Duration;

use serde_json::{Map, Value};

use super::{
    get_bool, get_i64, get_str, load_settings, save_settings, to_conf_path, Detection, Runtime,
    ServiceDriver, ServiceStats, ServiceStatus,
};
use crate::instance::ServiceInstance;
use crate::log;
use crate::log_tail::{read_tail, LogTail};
use crate::managed_block::{self, BlockOutcome};
use crate::paths::Paths;
use crate::probe;
use crate::run_once::run_once;

const READY_TIMEOUT_MS: u64 = 10_000;
const PROBE_TIMEOUT: Duration = Duration::from_millis(250);
const STOP_GRACE: Duration = Duration::from_secs(3);

#[derive(Clone, Debug)]
pub struct RedisSettings {
    pub bind: String,
    pub timeout: i64,
    pub keep_alive: i64,
    pub max_clients: i64,
    pub databases: i64,
    pub password: String,
    pub max_memory_mb: i64,
    pub max_memory_policy: String,
    /// `SHUTDOWN SAVE` first, `NOSAVE` only if persisting fails.
    pub persist_on_shutdown: bool,
}

impl Default for RedisSettings {
    fn default() -> Self {
        RedisSettings {
            bind: "127.0.0.1".into(),
            timeout: 0,
            keep_alive: 300,
            max_clients: 10_000,
            databases: 16,
            password: String::new(),
            max_memory_mb: 256,
            max_memory_policy: "noeviction".into(),
            persist_on_shutdown: true,
        }
    }
}

impl RedisSettings {
    fn load(data_dir: &Path) -> Self {
        let mut out = RedisSettings::default();
        let map = load_settings(&data_dir.join("redis.settings.json"));
        if map.is_empty() {
            return out;
        }
        out.bind = get_str(&map, "bind", &out.bind);
        out.timeout = get_i64(&map, "timeout", out.timeout);
        out.keep_alive = get_i64(&map, "tcpKeepAlive", out.keep_alive);
        out.max_clients = get_i64(&map, "maxClients", out.max_clients);
        out.databases = get_i64(&map, "databases", out.databases);
        out.password = get_str(&map, "password", &out.password);
        out.max_memory_mb = get_i64(&map, "maxMemoryMb", out.max_memory_mb);
        out.max_memory_policy = get_str(&map, "maxMemoryPolicy", &out.max_memory_policy);
        out.persist_on_shutdown = get_bool(&map, "persistOnShutdown", out.persist_on_shutdown);
        out
    }

    pub fn save(&self, data_dir: &Path) -> bool {
        let mut object = Map::new();
        object.insert("bind".into(), Value::from(self.bind.clone()));
        object.insert("timeout".into(), Value::from(self.timeout));
        object.insert("tcpKeepAlive".into(), Value::from(self.keep_alive));
        object.insert("maxClients".into(), Value::from(self.max_clients));
        object.insert("databases".into(), Value::from(self.databases));
        object.insert("password".into(), Value::from(self.password.clone()));
        object.insert("maxMemoryMb".into(), Value::from(self.max_memory_mb));
        object.insert(
            "maxMemoryPolicy".into(),
            Value::from(self.max_memory_policy.clone()),
        );
        object.insert(
            "persistOnShutdown".into(),
            Value::from(self.persist_on_shutdown),
        );
        save_settings(&data_dir.join("redis.settings.json"), object)
    }
}

pub struct RedisDriver {
    paths: Paths,
    instance: ServiceInstance,
    port: u16,
    settings: RedisSettings,
    runtime: Runtime,
}

impl RedisDriver {
    pub fn new(paths: Paths, instance: ServiceInstance, port: u16) -> Self {
        let data_dir = paths.resolve(&instance.data_dir);
        let settings = RedisSettings::load(&data_dir);
        let label = instance.id.clone();
        RedisDriver {
            paths,
            instance,
            port,
            settings,
            runtime: Runtime::new(&label),
        }
    }

    pub fn settings(&self) -> &RedisSettings {
        &self.settings
    }

    pub fn settings_mut(&mut self) -> &mut RedisSettings {
        &mut self.settings
    }

    pub fn save_settings(&self) -> bool {
        self.settings.save(&self.data_root())
    }

    pub fn conf_path(&self) -> PathBuf {
        self.data_root().join("redis.conf")
    }

    pub fn log_path(&self) -> PathBuf {
        self.data_root().join("redis.log")
    }

    fn find_executable(&self) -> Option<PathBuf> {
        let root = self.install_root();
        let name = if cfg!(windows) {
            "redis-server.exe"
        } else {
            "redis-server"
        };
        let candidates = [root.join(name), root.join("bin").join(name)];
        candidates.into_iter().find(|path| path.exists())
    }

    fn version_part(&self, index: usize) -> i64 {
        self.instance
            .version
            .split('.')
            .nth(index)
            .map(|part| {
                let digits: String = part.chars().take_while(|ch| ch.is_ascii_digit()).collect();
                digits.parse().unwrap_or(0)
            })
            .unwrap_or(0)
    }

    fn version_at_least(&self, major: i64, minor: i64) -> bool {
        let own = self.version_part(0);
        if own == 0 {
            // Unknown version: assume the newest template.
            return true;
        }
        if own != major {
            return own > major;
        }
        self.version_part(1) >= minor
    }

    fn render_block(&self) -> String {
        let data_dir = to_conf_path(&self.data_root());
        let mut lines = vec![
            "# rendered by devdatapod from the instance settings".to_string(),
            format!("bind {}", self.settings.bind),
            format!("port {}", self.port),
            format!("timeout {}", self.settings.timeout),
        ];
        if self.version_at_least(3, 2) {
            // tcp-keepalive arrived in 3.2.1; 3.0 rejects the unknown key.
            lines.push(format!("tcp-keepalive {}", self.settings.keep_alive));
        }
        lines.push(format!("maxclients {}", self.settings.max_clients));
        // Never daemonize: the panel must stay the parent, or the job object holds nothing
        // and an orphan redis survives a crash (rule 5).
        lines.push("daemonize no".into());
        if self.version_at_least(3, 2) {
            lines.push("supervised no".into());
        }
        lines.push("loglevel notice".into());
        lines.push("logfile \"\"".into());
        lines.push(format!("databases {}", self.settings.databases));
        if self.version_at_least(5, 0) {
            lines.push("always-show-logo no".into());
        }
        // Paths are quoted: the install directory may contain spaces and redis splits
        // unquoted values on whitespace, turning a path into a FATAL CONFIG FILE ERROR.
        lines.push(format!("dir \"{data_dir}\""));
        if self.version_at_least(3, 2) {
            // 3.0.504 has no protected-mode at all; writing it refuses to start.
            lines.push(format!(
                "protected-mode {}",
                if self.settings.password.is_empty() {
                    "no"
                } else {
                    "yes"
                }
            ));
        }
        if self.settings.max_memory_mb > 0 {
            lines.push(format!("maxmemory {}mb", self.settings.max_memory_mb));
            if self.version_at_least(4, 0) && !self.settings.max_memory_policy.is_empty() {
                lines.push(format!(
                    "maxmemory-policy {}",
                    self.settings.max_memory_policy
                ));
            }
        }
        if !self.settings.password.is_empty() {
            lines.push(format!("requirepass \"{}\"", self.settings.password));
        }
        lines.join("\n") + "\n"
    }

    /// Inline RESP command(s). Any reply at all proves the event loop is serving; `-NOAUTH`
    /// still counts as up, which is what "ready" means for a password-protected instance.
    fn probe(&self, verbs: &[String]) -> (bool, String) {
        let ip: IpAddr = self
            .settings
            .bind
            .parse()
            .unwrap_or(IpAddr::from([127, 0, 0, 1]));
        let Ok(mut stream) =
            TcpStream::connect_timeout(&SocketAddr::new(ip, self.port), PROBE_TIMEOUT)
        else {
            return (
                false,
                format!("no listener on {}:{}", self.settings.bind, self.port),
            );
        };
        let _ = stream.set_read_timeout(Some(PROBE_TIMEOUT));
        let _ = stream.set_write_timeout(Some(PROBE_TIMEOUT));

        let mut request = String::new();
        if !self.settings.password.is_empty() {
            request.push_str(&format!("AUTH {}\r\n", self.settings.password));
        }
        for verb in verbs {
            request.push_str(verb);
            request.push_str("\r\n");
        }
        if stream.write_all(request.as_bytes()).is_err() {
            return (
                false,
                format!("write to {}:{} timed out", self.settings.bind, self.port),
            );
        }
        let mut buffer = [0u8; 256];
        match stream.read(&mut buffer) {
            Ok(0) => (
                false,
                format!(
                    "{}:{} accepted the connection but closed it",
                    self.settings.bind, self.port
                ),
            ),
            Ok(read) => (
                true,
                String::from_utf8_lossy(&buffer[..read]).trim().to_string(),
            ),
            Err(_) => (
                false,
                format!(
                    "{}:{} accepted the connection but stayed silent",
                    self.settings.bind, self.port
                ),
            ),
        }
    }
}

impl ServiceDriver for RedisDriver {
    fn instance(&self) -> &ServiceInstance {
        &self.instance
    }

    fn port(&self) -> u16 {
        self.port
    }

    fn install_root(&self) -> PathBuf {
        self.paths.resolve(&self.instance.install_path)
    }

    fn data_root(&self) -> PathBuf {
        self.paths.resolve(&self.instance.data_dir)
    }

    fn detect(&mut self) -> Detection {
        let mut out = Detection::default();
        let Some(executable) = self.find_executable() else {
            out.message = format!(
                "redis-server not under {}",
                if self.instance.install_path.is_empty() {
                    "<no install path>"
                } else {
                    &self.instance.install_path
                }
            );
            return out;
        };
        out.executable = executable.clone();
        let result = run_once(
            &executable,
            &["--version".into()],
            Duration::from_secs(10),
            None,
        );
        out.installed = result.ran;
        out.version = crate::run_once::extract_version(&result.output);
        if out.version.is_empty() {
            out.version = self.instance.version.clone();
        }
        out.message = if result.ran {
            result.output.trim().to_string()
        } else {
            format!("{} did not run", executable.display())
        };
        out
    }

    fn prepare_config(&mut self) -> Result<(), String> {
        let data_dir = self.data_root();
        if self.instance.data_dir.is_empty() {
            return Err(format!("{} has no data directory", self.instance.id));
        }
        std::fs::create_dir_all(&data_dir)
            .map_err(|error| format!("cannot create {}：{error}", data_dir.display()))?;
        match managed_block::apply(&self.conf_path(), &self.render_block()) {
            Ok(BlockOutcome::Written) => Ok(()),
            Ok(BlockOutcome::Preserved) => {
                log::warn(&format!(
                    "redis/{}: {} has no devdatapod block, using it as is",
                    self.instance.id,
                    self.conf_path().display()
                ));
                Ok(())
            }
            Ok(BlockOutcome::Failed) => {
                Err(format!("cannot write {}", self.conf_path().display()))
            }
            Err(error) => Err(error),
        }
    }

    fn start(&mut self) -> Result<(), String> {
        // Checked first so an uninstalled version cannot leave a rendered redis.conf behind.
        let Some(program) = self.find_executable() else {
            return Err(format!("{} is not installed", self.instance.id));
        };
        // Always re-render: `port` comes from the allocator and a default switch changes it.
        self.prepare_config()?;
        if !self.runtime.supervisor.is_valid() {
            return Err(self.runtime.supervisor.problem().to_string());
        }

        self.runtime.supervisor.set_working_dir(self.data_root());
        self.runtime.supervisor.set_log_file(self.log_path());
        self.runtime.mark_started();
        // Relative on purpose: the working directory IS the datadir, and the msys-built
        // redis-server mangles an absolute `D:/...` path into a doubled Unix-style one.
        self.runtime
            .supervisor
            .start(&program, &["redis.conf".to_string()])
            .map_err(|error| error.to_string())
    }

    fn stop(&mut self) -> Result<(), String> {
        self.runtime.mark_stopped();
        if !self.runtime.supervisor.is_running() {
            return Ok(());
        }

        // The service's own shutdown verb first, so RDB/AOF survive. `probe` prepends AUTH.
        let verb = format!(
            "SHUTDOWN {}",
            if self.settings.persist_on_shutdown {
                "SAVE"
            } else {
                "NOSAVE"
            }
        );
        let (_, detail) = self.probe(&[verb]);
        if self.settings.persist_on_shutdown && detail.contains("ERR") {
            // Redis refuses SAVE-with-pending-writes in a few states; the escape hatch is
            // NOSAVE, not a kill signal, so the process still exits cleanly.
            self.probe(&["SHUTDOWN NOSAVE".to_string()]);
        }
        if self.runtime.supervisor.wait_finished(STOP_GRACE) {
            return Ok(());
        }
        self.runtime.supervisor.graceful();
        if self.runtime.supervisor.wait_finished(Duration::from_secs(2)) {
            return Ok(());
        }
        if self.runtime.supervisor.kill_forced() {
            log::warn(&format!(
                "redis/{}: had to be killed, SHUTDOWN did not take ({detail})",
                self.instance.id
            ));
            return Ok(());
        }
        Err(format!(
            "{} is still running after SHUTDOWN, SIGTERM and SIGKILL",
            self.instance.id
        ))
    }

    fn status(&mut self) -> ServiceStatus {
        let (ok, detail) = self.probe(&["PING".to_string()]);
        // Keep the shared PING helper exercised for the no-password case; the inline probe
        // above is what handles AUTH.
        let ok = ok
            || (self.settings.password.is_empty()
                && probe::redis_ping(self.port, &self.settings.bind, PROBE_TIMEOUT));
        self.runtime.classify(ok, detail, READY_TIMEOUT_MS)
    }

    fn logs(&self, lines: usize) -> LogTail {
        read_tail(&self.log_path(), lines, 256 * 1024)
    }

    fn stats(&mut self) -> ServiceStats {
        self.runtime.stats()
    }

    fn settings_json(&self) -> Value {
        let mut object = Map::new();
        object.insert("bind".into(), Value::from(self.settings.bind.clone()));
        object.insert("timeout".into(), Value::from(self.settings.timeout));
        object.insert("tcpKeepAlive".into(), Value::from(self.settings.keep_alive));
        object.insert("maxClients".into(), Value::from(self.settings.max_clients));
        object.insert("databases".into(), Value::from(self.settings.databases));
        object.insert("password".into(), Value::from(self.settings.password.clone()));
        object.insert(
            "maxMemoryMb".into(),
            Value::from(self.settings.max_memory_mb),
        );
        object.insert(
            "maxMemoryPolicy".into(),
            Value::from(self.settings.max_memory_policy.clone()),
        );
        object.insert(
            "persistOnShutdown".into(),
            Value::from(self.settings.persist_on_shutdown),
        );
        Value::Object(object)
    }

    fn apply_settings(&mut self, value: &Value) -> Result<(), String> {
        let map = value.as_object().cloned().unwrap_or_default();
        if !map.is_empty() {
            self.settings.bind = get_str(&map, "bind", &self.settings.bind);
            self.settings.timeout = get_i64(&map, "timeout", self.settings.timeout).clamp(0, 86400);
            self.settings.keep_alive =
                get_i64(&map, "tcpKeepAlive", self.settings.keep_alive).clamp(0, 86400);
            self.settings.max_clients =
                get_i64(&map, "maxClients", self.settings.max_clients).clamp(1, 1_000_000);
            self.settings.databases =
                get_i64(&map, "databases", self.settings.databases).clamp(1, 1024);
            self.settings.password = get_str(&map, "password", &self.settings.password);
            self.settings.max_memory_mb =
                get_i64(&map, "maxMemoryMb", self.settings.max_memory_mb).clamp(0, 1_048_576);
            self.settings.max_memory_policy =
                get_str(&map, "maxMemoryPolicy", &self.settings.max_memory_policy);
            self.settings.persist_on_shutdown =
                get_bool(&map, "persistOnShutdown", self.settings.persist_on_shutdown);
        }
        if !self.save_settings() {
            return Err(format!("cannot write {}", self.data_root().display()));
        }
        self.prepare_config()
    }
}
