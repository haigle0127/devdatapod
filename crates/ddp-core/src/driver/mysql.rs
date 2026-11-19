//! MySQL: per-instance `my.ini` with a managed block, `--initialize-insecure` on first run,
//! `mysqladmin shutdown` for a clean InnoDB flush.
//!
//! Three MySQL-specific things:
//!   1. an empty datadir must be initialised before `mysqld` will start at all — and the
//!      directories are per instance, because a MySQL 8 datadir cannot be opened by 5.7;
//!   2. graceful stop is `mysqladmin shutdown`, not redis's inline `SHUTDOWN` verb;
//!   3. 5.7 and 8.x/9.x disagree about authentication plugins, so the renderer is
//!      version-gated instead of writing options that the running binary rejects.

use std::path::{Path, PathBuf};
use std::time::Duration;

use serde_json::{Map, Value};

use super::{
    get_i64, get_str, load_settings, save_settings, to_conf_path, Detection, Runtime,
    ServiceDriver, ServiceStats, ServiceStatus,
};
use crate::instance::ServiceInstance;
use crate::log;
use crate::log_tail::{read_tail, LogTail};
use crate::managed_block::{self, BlockOutcome};
use crate::paths::Paths;
use crate::probe;
use crate::run_once::run_once;

const READY_TIMEOUT_MS: u64 = 15_000;
const PROBE_TIMEOUT: Duration = Duration::from_millis(500);
const STOP_GRACE: Duration = Duration::from_secs(8);

#[derive(Clone, Debug)]
pub struct MysqlSettings {
    pub bind: String,
    pub max_connections: i64,
    pub charset: String,
    pub storage_engine: String,
    pub innodb_buffer_pool_mb: i64,
    pub max_allowed_packet_mb: i64,
    /// Windows defaults to 1 (stores lower-case names); changing it after initialisation
    /// requires rebuilding the whole datadir.
    pub lower_case_table_names: i64,
    /// Local development defaults to root/root; the driver sets it into the system tables
    /// right after initialisation.
    pub root_password: String,
}

impl Default for MysqlSettings {
    fn default() -> Self {
        MysqlSettings {
            bind: "127.0.0.1".into(),
            max_connections: 100,
            charset: "utf8mb4".into(),
            storage_engine: "InnoDB".into(),
            innodb_buffer_pool_mb: 64,
            max_allowed_packet_mb: 16,
            lower_case_table_names: 1,
            root_password: "root".into(),
        }
    }
}

impl MysqlSettings {
    fn load(data_dir: &Path) -> Self {
        let mut out = MysqlSettings::default();
        let map = load_settings(&data_dir.join("mysql.settings.json"));
        if map.is_empty() {
            return out;
        }
        out.bind = get_str(&map, "bind", &out.bind);
        out.max_connections = get_i64(&map, "maxConnections", out.max_connections);
        out.charset = get_str(&map, "charset", &out.charset);
        out.storage_engine = get_str(&map, "storageEngine", &out.storage_engine);
        out.innodb_buffer_pool_mb = get_i64(&map, "innodbBufferPoolMb", out.innodb_buffer_pool_mb);
        out.max_allowed_packet_mb =
            get_i64(&map, "maxAllowedPacketMb", out.max_allowed_packet_mb);
        out.lower_case_table_names =
            get_i64(&map, "lowerCaseTableNames", out.lower_case_table_names);
        out.root_password = get_str(&map, "rootPassword", &out.root_password);
        out
    }

    pub fn save(&self, data_dir: &Path) -> bool {
        let mut object = Map::new();
        object.insert("bind".into(), Value::from(self.bind.clone()));
        object.insert("maxConnections".into(), Value::from(self.max_connections));
        object.insert("charset".into(), Value::from(self.charset.clone()));
        object.insert("storageEngine".into(), Value::from(self.storage_engine.clone()));
        object.insert(
            "innodbBufferPoolMb".into(),
            Value::from(self.innodb_buffer_pool_mb),
        );
        object.insert(
            "maxAllowedPacketMb".into(),
            Value::from(self.max_allowed_packet_mb),
        );
        object.insert(
            "lowerCaseTableNames".into(),
            Value::from(self.lower_case_table_names),
        );
        object.insert("rootPassword".into(), Value::from(self.root_password.clone()));
        save_settings(&data_dir.join("mysql.settings.json"), object)
    }
}

pub struct MysqlDriver {
    paths: Paths,
    instance: ServiceInstance,
    port: u16,
    settings: MysqlSettings,
    runtime: Runtime,
    just_initialized: bool,
}

impl MysqlDriver {
    pub fn new(paths: Paths, instance: ServiceInstance, port: u16) -> Self {
        let data_dir = paths.resolve(&instance.data_dir);
        let settings = MysqlSettings::load(&data_dir);
        let label = instance.id.clone();
        MysqlDriver {
            paths,
            instance,
            port,
            settings,
            runtime: Runtime::new(&label),
            just_initialized: false,
        }
    }

    pub fn settings(&self) -> &MysqlSettings {
        &self.settings
    }

    pub fn settings_mut(&mut self) -> &mut MysqlSettings {
        &mut self.settings
    }

    pub fn save_settings(&self) -> bool {
        self.settings.save(&self.data_root())
    }

    pub fn ini_path(&self) -> PathBuf {
        self.data_root().join("my.ini")
    }

    pub fn log_path(&self) -> PathBuf {
        self.data_root().join("mysql-error.log")
    }

    fn find_executable(&self, name: &str) -> Option<PathBuf> {
        let root = self.install_root();
        let file = if cfg!(windows) {
            format!("{name}.exe")
        } else {
            name.to_string()
        };
        let candidates = [root.join("bin").join(&file), root.join(&file)];
        candidates.into_iter().find(|path| path.exists())
    }

    /// 8.0 and above (including 9.x) default to `caching_sha2_password`; older clients need
    /// the native plugin where the server still ships it.
    fn is_version8(&self) -> bool {
        self.instance.version.starts_with('8') || self.instance.version.starts_with('9')
    }

    fn render_block(&self) -> String {
        let data_dir = to_conf_path(&self.data_root());
        let base_dir = to_conf_path(&self.install_root());
        let mut lines = vec![
            "[mysqld]".to_string(),
            "# 由 devdatapod 按实例设置渲染".into(),
            format!("bind-address={}", self.settings.bind),
            format!("port={}", self.port),
            format!("basedir={base_dir}"),
            format!("datadir={data_dir}"),
            format!("character-set-server={}", self.settings.charset),
            format!("default-storage-engine={}", self.settings.storage_engine),
            format!("max_connections={}", self.settings.max_connections),
            format!("max_allowed_packet={}M", self.settings.max_allowed_packet_mb),
            format!(
                "lower_case_table_names={}",
                self.settings.lower_case_table_names
            ),
            format!(
                "innodb_buffer_pool_size={}M",
                self.settings.innodb_buffer_pool_mb
            ),
            format!("log-error={}", to_conf_path(&self.log_path())),
        ];
        // 5.7 already defaults to mysql_native_password. 8.0.x still understands
        // `default_authentication_plugin`; 8.4 needs the plugin loaded explicitly; 9.x
        // removed it entirely, so nothing is written there.
        if self.is_version8() {
            let version = &self.instance.version;
            if version.starts_with("8.0") {
                lines.push("default_authentication_plugin=mysql_native_password".into());
            } else if version.starts_with('8') {
                lines.push("mysql_native_password=ON".into());
            }
        }
        lines.join("\n") + "\n"
    }

    fn datadir_initialized(&self) -> bool {
        // The presence of the `mysql` system schema is the marker of a finished initialise.
        self.data_root().join("mysql").is_dir()
    }

    fn initialize_datadir(&mut self) -> Result<(), String> {
        let Some(program) = self.find_executable("mysqld") else {
            return Err(format!("{} 未安装", self.instance.id));
        };
        let data_dir = self.data_root();
        log::info(&format!(
            "mysql/{}: 初始化数据目录 {}",
            self.instance.id,
            data_dir.display()
        ));
        // `--initialize` creates only the leaf datadir; on a fresh machine
        // `data/instances/<kind>/` does not exist yet.
        std::fs::create_dir_all(&data_dir)
            .map_err(|error| format!("无法创建数据目录 {}：{error}", data_dir.display()))?;

        // `--initialize` requires a completely empty datadir, and our my.ini lives inside it,
        // so the initialise call passes everything on the command line instead.
        let base_dir = to_conf_path(&self.install_root());
        let arguments = vec![
            format!("--basedir={base_dir}"),
            format!("--datadir={}", to_conf_path(&data_dir)),
            format!(
                "--lower_case_table_names={}",
                self.settings.lower_case_table_names
            ),
            "--initialize-insecure".into(),
            "--console".into(),
        ];
        let result = run_once(&program, &arguments, Duration::from_secs(120), None);
        if !result.ran || result.exit_code != 0 {
            let why = if result.error_output.trim().is_empty() {
                result.output.trim().to_string()
            } else {
                result.error_output.trim().to_string()
            };
            return Err(format!("初始化失败：{why}"));
        }
        if !self.datadir_initialized() {
            return Err(format!(
                "初始化后 mysql 系统库仍不存在：{}",
                result.output.trim()
            ));
        }
        self.just_initialized = true;
        Ok(())
    }

    fn probe_handshake(&self) -> (bool, String) {
        if probe::mysql_handshake(self.port, &self.settings.bind, PROBE_TIMEOUT) {
            (true, String::new())
        } else if !probe::is_listening(self.port, &self.settings.bind) {
            (
                false,
                format!("{}:{} 没有监听", self.settings.bind, self.port),
            )
        } else {
            (
                false,
                format!("{}:{} 接受了连接但没发握手包", self.settings.bind, self.port),
            )
        }
    }

    /// Wait for the handshake, then set root's password through the still-empty-password
    /// account. 8.0 creates root with caching_sha2_password, which older clients (Adminer,
    /// older PHP) cannot use, so the plugin is changed at the same time.
    fn ensure_root_password(&mut self) -> Result<(), String> {
        let deadline = std::time::Instant::now() + Duration::from_millis(READY_TIMEOUT_MS);
        let mut detail = String::new();
        let mut up = false;
        while std::time::Instant::now() < deadline {
            let (ok, why) = self.probe_handshake();
            if ok {
                up = true;
                break;
            }
            detail = why;
            std::thread::sleep(Duration::from_millis(200));
        }
        if !up {
            return Err(format!(
                "服务没在 {} 秒内就绪：{detail}",
                READY_TIMEOUT_MS / 1000
            ));
        }

        let Some(mysql) = self.find_executable("mysql") else {
            return Err("找不到 mysql 客户端，无法设置 root 密码".into());
        };
        let plugin = if self.is_version8() && !self.instance.version.starts_with('9') {
            "mysql_native_password"
        } else {
            "caching_sha2_password"
        };
        // Doubling single quotes is the only escaping MySQL needs in a string literal.
        let password = self.settings.root_password.replace('\'', "''");
        let sql = format!(
            "ALTER USER 'root'@'localhost' IDENTIFIED WITH {plugin} BY '{password}'; \
             CREATE USER IF NOT EXISTS 'root'@'127.0.0.1' IDENTIFIED WITH {plugin} BY '{password}'; \
             GRANT ALL PRIVILEGES ON *.* TO 'root'@'127.0.0.1' WITH GRANT OPTION; \
             FLUSH PRIVILEGES;"
        );
        let arguments = vec![
            format!("--port={}", self.port),
            "--user=root".into(),
            "--host=127.0.0.1".into(),
            "-e".into(),
            sql,
        ];
        let result = run_once(&mysql, &arguments, Duration::from_secs(30), None);
        if !result.ran || result.exit_code != 0 {
            let why = if result.error_output.trim().is_empty() {
                result.output.trim().to_string()
            } else {
                result.error_output.trim().to_string()
            };
            return Err(format!("设置 root 密码失败：{why}"));
        }
        log::info(&format!("mysql/{}: root 密码已设置", self.instance.id));
        Ok(())
    }

    fn mysqladmin_shutdown(&self) {
        let Some(admin) = self.find_executable("mysqladmin") else {
            return;
        };
        let _ = run_once(
            &admin,
            &[
                format!("--port={}", self.port),
                "--user=root".into(),
                format!("--password={}", self.settings.root_password),
                "shutdown".into(),
            ],
            Duration::from_secs(12),
            None,
        );
    }
}

impl ServiceDriver for MysqlDriver {
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
        let Some(executable) = self.find_executable("mysqld") else {
            out.message = format!(
                "{} 下找不到 mysqld",
                if self.instance.install_path.is_empty() {
                    "<无安装路径>"
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
            Duration::from_secs(15),
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
            format!("{} 无法运行", executable.display())
        };
        out
    }

    fn prepare_config(&mut self) -> Result<(), String> {
        let data_dir = self.data_root();
        if self.instance.data_dir.is_empty() {
            return Err(format!("{} 没有数据目录", self.instance.id));
        }
        std::fs::create_dir_all(&data_dir)
            .map_err(|error| format!("无法创建 {}：{error}", data_dir.display()))?;
        match managed_block::apply(&self.ini_path(), &self.render_block()) {
            Ok(BlockOutcome::Written) => Ok(()),
            Ok(BlockOutcome::Preserved) => {
                log::warn(&format!(
                    "mysql/{}: {} 没有 devdatapod 托管块，按原样使用",
                    self.instance.id,
                    self.ini_path().display()
                ));
                Ok(())
            }
            Ok(BlockOutcome::Failed) => {
                Err(format!("无法写入 {}", self.ini_path().display()))
            }
            Err(error) => Err(error),
        }
    }

    fn start(&mut self) -> Result<(), String> {
        let Some(program) = self.find_executable("mysqld") else {
            return Err(format!("{} 未安装", self.instance.id));
        };
        // Initialise before writing my.ini: `--initialize` wants an empty datadir.
        if !self.datadir_initialized() {
            self.initialize_datadir()?;
        }
        // Re-render every start: the port comes from the allocator.
        self.prepare_config()?;
        if !self.runtime.supervisor.is_valid() {
            return Err(self.runtime.supervisor.problem().to_string());
        }

        self.runtime.supervisor.set_working_dir(self.data_root());
        self.runtime.supervisor.set_log_file(self.log_path());
        self.runtime.mark_started();

        // `--console` keeps mysqld in the foreground. It must never background itself, or
        // the job object holds nothing and a panel crash leaves an orphan mysqld (rule 5).
        let arguments = vec![
            format!("--defaults-file={}", to_conf_path(&self.ini_path())),
            "--console".into(),
        ];
        if let Err(error) = self.runtime.supervisor.start(&program, &arguments) {
            return Err(error);
        }

        if self.just_initialized {
            self.just_initialized = false;
            if let Err(error) = self.ensure_root_password() {
                let _ = self.stop();
                return Err(error);
            }
        }
        Ok(())
    }

    fn stop(&mut self) -> Result<(), String> {
        self.runtime.mark_stopped();
        if !self.runtime.supervisor.is_running() {
            return Ok(());
        }

        // Graceful first so InnoDB flushes its dirty pages. Without the password,
        // mysqladmin silently fails — hence the 8 s wait before escalating.
        self.mysqladmin_shutdown();
        if self.runtime.supervisor.wait_finished(STOP_GRACE) {
            return Ok(());
        }
        self.runtime.supervisor.graceful();
        if self.runtime.supervisor.wait_finished(Duration::from_secs(2)) {
            return Ok(());
        }
        if self.runtime.supervisor.kill_forced() {
            log::warn(&format!(
                "mysql/{}: mysqladmin 没停掉，已强杀",
                self.instance.id
            ));
            return Ok(());
        }
        Err(format!(
            "{} 在 mysqladmin、信号、强杀之后仍在运行",
            self.instance.id
        ))
    }

    fn status(&mut self) -> ServiceStatus {
        let (ok, detail) = self.probe_handshake();
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
        object.insert(
            "maxConnections".into(),
            Value::from(self.settings.max_connections),
        );
        object.insert("charset".into(), Value::from(self.settings.charset.clone()));
        object.insert(
            "storageEngine".into(),
            Value::from(self.settings.storage_engine.clone()),
        );
        object.insert(
            "innodbBufferPoolMb".into(),
            Value::from(self.settings.innodb_buffer_pool_mb),
        );
        object.insert(
            "maxAllowedPacketMb".into(),
            Value::from(self.settings.max_allowed_packet_mb),
        );
        object.insert(
            "lowerCaseTableNames".into(),
            Value::from(self.settings.lower_case_table_names),
        );
        object.insert(
            "rootPassword".into(),
            Value::from(self.settings.root_password.clone()),
        );
        Value::Object(object)
    }

    fn apply_settings(&mut self, value: &Value) -> Result<(), String> {
        let map = value.as_object().cloned().unwrap_or_default();
        if !map.is_empty() {
            self.settings.bind = get_str(&map, "bind", &self.settings.bind);
            self.settings.max_connections =
                get_i64(&map, "maxConnections", self.settings.max_connections).clamp(1, 100_000);
            self.settings.charset = get_str(&map, "charset", &self.settings.charset);
            self.settings.storage_engine =
                get_str(&map, "storageEngine", &self.settings.storage_engine);
            self.settings.innodb_buffer_pool_mb =
                get_i64(&map, "innodbBufferPoolMb", self.settings.innodb_buffer_pool_mb)
                    .clamp(5, 65536);
            self.settings.max_allowed_packet_mb =
                get_i64(&map, "maxAllowedPacketMb", self.settings.max_allowed_packet_mb)
                    .clamp(1, 1024);
            self.settings.lower_case_table_names =
                get_i64(&map, "lowerCaseTableNames", self.settings.lower_case_table_names)
                    .clamp(0, 2);
            self.settings.root_password =
                get_str(&map, "rootPassword", &self.settings.root_password);
        }
        if !self.save_settings() {
            return Err(format!("无法写入 {}", self.data_root().display()));
        }
        self.prepare_config()
    }
}
