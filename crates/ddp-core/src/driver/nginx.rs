//! Nginx: instance-scoped `nginx.conf` with a managed block, `-s quit` for shutdown.
//!
//! Three things make nginx different from redis:
//!   1. the config is a full nested syntax (`http { server { ... } }`), not a flat list of
//!      `key value` lines;
//!   2. stopping is `nginx -s quit` (workers finish their requests), not a signal;
//!   3. on Windows both start and stop need `-p <prefix>` so conf/logs/pid resolve inside
//!      the instance datadir and `Extensions` stays a read-only component.

use std::path::{Path, PathBuf};
use std::time::Duration;

use serde_json::{Map, Value};

use super::{
    get_bool, get_i64, get_str, load_settings, save_settings, startup_failure, to_conf_path,
    Detection, Runtime, ServiceDriver, ServiceStats, ServiceStatus,
};
use crate::instance::ServiceInstance;
use crate::log;
use crate::log_tail::{read_tail, LogTail};
use crate::managed_block::{self, BlockOutcome};
use crate::paths::Paths;
use crate::probe;
use crate::run_once::run_once;
use crate::sites;

const READY_TIMEOUT_MS: u64 = 8_000;
const PROBE_TIMEOUT: Duration = Duration::from_millis(400);
const STOP_GRACE: Duration = Duration::from_secs(4);
const START_CHECK: Duration = Duration::from_millis(1_500);

#[derive(Clone, Debug)]
pub struct NginxSettings {
    pub worker_processes: i64,
    pub worker_connections: i64,
    /// Defaults to the shared `WWW` directory at the install root.
    pub root_dir: String,
    pub index_files: String,
    pub access_log: bool,
    pub gzip: bool,
}

impl Default for NginxSettings {
    fn default() -> Self {
        NginxSettings {
            worker_processes: 1,
            worker_connections: 1024,
            root_dir: "WWW".into(),
            index_files: "index.html index.htm".into(),
            access_log: true,
            gzip: true,
        }
    }
}

impl NginxSettings {
    fn load(data_dir: &Path) -> Self {
        let mut out = NginxSettings::default();
        let map = load_settings(&data_dir.join("nginx.settings.json"));
        if map.is_empty() {
            return out;
        }
        out.worker_processes = get_i64(&map, "workerProcesses", out.worker_processes);
        out.worker_connections = get_i64(&map, "workerConnections", out.worker_connections);
        out.root_dir = get_str(&map, "rootDir", &out.root_dir);
        out.index_files = get_str(&map, "indexFiles", &out.index_files);
        out.access_log = get_bool(&map, "accessLog", out.access_log);
        out.gzip = get_bool(&map, "gzip", out.gzip);
        out
    }

    pub fn save(&self, data_dir: &Path) -> bool {
        let mut object = Map::new();
        object.insert("workerProcesses".into(), Value::from(self.worker_processes));
        object.insert(
            "workerConnections".into(),
            Value::from(self.worker_connections),
        );
        object.insert("rootDir".into(), Value::from(self.root_dir.clone()));
        object.insert("indexFiles".into(), Value::from(self.index_files.clone()));
        object.insert("accessLog".into(), Value::from(self.access_log));
        object.insert("gzip".into(), Value::from(self.gzip));
        save_settings(&data_dir.join("nginx.settings.json"), object)
    }
}

pub struct NginxDriver {
    paths: Paths,
    instance: ServiceInstance,
    port: u16,
    settings: NginxSettings,
    runtime: Runtime,
}

impl NginxDriver {
    pub fn new(paths: Paths, instance: ServiceInstance, port: u16) -> Self {
        let data_dir = paths.resolve(&instance.data_dir);
        let settings = NginxSettings::load(&data_dir);
        let label = instance.id.clone();
        NginxDriver {
            paths,
            instance,
            port,
            settings,
            runtime: Runtime::new(&label),
        }
    }

    pub fn settings(&self) -> &NginxSettings {
        &self.settings
    }

    pub fn settings_mut(&mut self) -> &mut NginxSettings {
        &mut self.settings
    }

    pub fn save_settings(&self) -> bool {
        self.settings.save(&self.data_root())
    }

    pub fn conf_path(&self) -> PathBuf {
        self.data_root().join("nginx.conf")
    }

    pub fn log_path(&self) -> PathBuf {
        self.data_root().join("logs").join("error.log")
    }

    pub fn access_log_path(&self) -> PathBuf {
        self.data_root().join("logs").join("access.log")
    }

    fn console_log(&self) -> PathBuf {
        self.data_root().join("logs").join("console.log")
    }

    pub fn absolute_root_dir(&self) -> PathBuf {
        let root = self.settings.root_dir.trim();
        if root.is_empty() {
            return self.paths.www_dir();
        }
        let path = Path::new(root);
        if path.is_absolute() {
            path.to_path_buf()
        } else {
            self.paths.resolve(root)
        }
    }

    fn find_executable(&self) -> Option<PathBuf> {
        let root = self.install_root();
        let name = if cfg!(windows) { "nginx.exe" } else { "nginx" };
        let candidates = [root.join(name), root.join("bin").join(name)];
        candidates.into_iter().find(|path| path.exists())
    }

    fn render_block(&self) -> String {
        let data_dir = to_conf_path(&self.data_root());
        let conf_root = to_conf_path(&self.install_root());
        let mut lines: Vec<String> = Vec::new();
        lines.push(format!("worker_processes {};", self.settings.worker_processes));
        // Logs and pid live in the instance datadir, so `Extensions` stays pristine.
        lines.push(format!(
            "error_log \"{data_dir}/logs/error.log\" warn;"
        ));
        lines.push(format!("pid \"{data_dir}/logs/nginx.pid\";"));
        lines.push("events {".into());
        lines.push(format!(
            "    worker_connections {};",
            self.settings.worker_connections
        ));
        lines.push("}".into());
        lines.push("http {".into());
        lines.push(format!("    include \"{conf_root}/conf/mime.types\";"));
        lines.push("    default_type application/octet-stream;".into());
        lines.push("    sendfile on;".into());
        lines.push("    keepalive_timeout 65;".into());
        lines.push("    server_tokens off;".into());
        if self.settings.gzip {
            lines.push("    gzip on;".into());
            lines.push("    gzip_min_length 1k;".into());
            lines.push(
                "    gzip_types text/css application/javascript application/json text/plain text/xml;"
                    .into(),
            );
        }
        if self.settings.access_log {
            lines.push(format!(
                "    access_log \"{}\";",
                to_conf_path(&self.access_log_path())
            ));
        } else {
            lines.push("    access_log off;".into());
        }
        // Nginx's temp directories default to sitting next to nginx.exe, which would write
        // runtime state into the component directory.
        for (key, sub) in [
            ("client_body_temp_path", "client_body"),
            ("proxy_temp_path", "proxy"),
            ("fastcgi_temp_path", "fastcgi"),
            ("uwsgi_temp_path", "uwsgi"),
            ("scgi_temp_path", "scgi"),
        ] {
            lines.push(format!("    {key} \"{data_dir}/temp/{sub}\";"));
        }

        lines.push("    server {".into());
        // Rule 1: loopback only; the port comes from PortAllocator, never a literal.
        lines.push(format!("        listen 127.0.0.1:{};", self.port));
        lines.push("        server_name localhost;".into());
        lines.push(format!(
            "        root \"{}\";",
            to_conf_path(&self.absolute_root_dir())
        ));
        lines.push(format!("        index {};", self.settings.index_files));
        lines.push("        location / {".into());
        lines.push("            try_files $uri $uri/ =404;".into());
        lines.push("        }".into());
        lines.push("    }".into());

        self.append_site_servers(&mut lines);
        lines.push("}".into());
        lines.join("\n") + "\n"
    }

    /// Every row on the 网站 page becomes one vhost. The site port is the user's choice and
    /// is not subject to the panel's 30000-32000 rule — it belongs to their website.
    fn append_site_servers(&self, lines: &mut Vec<String>) {
        let (items, problem) = sites::load(self.paths.root());
        if let Some(problem) = problem {
            log::warn(&format!("nginx/{}: 读取站点失败：{problem}", self.instance.id));
        }
        for site in items {
            if site.port == 0 {
                continue;
            }
            // A site that collides with the web service port is a config error. Skipping it
            // keeps `nginx -t` passing instead of taking the whole panel down.
            if site.port == self.port {
                log::warn(&format!(
                    "nginx/{}: 站点 {} 想用 {}，和 Web 服务端口冲突，已跳过",
                    self.instance.id, site.domain, site.port
                ));
                continue;
            }
            let mut names = vec![site.domain.clone()];
            for alias in &site.aliases {
                if !names.contains(alias) {
                    names.push(alias.clone());
                }
            }
            lines.push(format!("    # 站点 {}", site.domain));
            lines.push("    server {".into());
            lines.push(format!("        listen 127.0.0.1:{};", site.port));
            lines.push(format!("        server_name {};", names.join(" ")));
            lines.push(format!(
                "        root \"{}\";",
                to_conf_path(Path::new(&site.root_dir))
            ));
            lines.push(format!("        index {};", self.settings.index_files));
            lines.push("        location / {".into());
            lines.push("            try_files $uri $uri/ =404;".into());
            lines.push("        }".into());
            lines.push("    }".into());
        }
    }

    fn ensure_welcome_page(&self) {
        let root = self.absolute_root_dir();
        if std::fs::create_dir_all(&root).is_err() {
            log::warn(&format!(
                "nginx/{}: 无法创建站点目录 {}",
                self.instance.id,
                root.display()
            ));
            return;
        }
        // Any existing index wins; the user's page is never overwritten.
        for index in self.settings.index_files.split_whitespace() {
            if root.join(index).exists() {
                return;
            }
        }
        let label = crate::sites::normalize(&self.settings.root_dir);
        let page = format!(
            "<!doctype html>\n<html lang=\"zh-CN\">\n<head>\n  <meta charset=\"utf-8\">\n  \
             <title>devdatapod · Nginx</title>\n  <style>\n    body {{ margin: 0; min-height: \
             100vh; display: flex; align-items: center; justify-content: center; background: \
             #F6F5FB; color: #26203F; font-family: 'MiSans', 'Microsoft YaHei', sans-serif; }}\n  \
             .card {{ padding: 40px 48px; background: #FFFFFF; border-radius: 16px; box-shadow: \
             0 12px 32px rgba(38, 32, 63, 0.08); text-align: center; }}\n    h1 {{ margin: 0 0 \
             10px; font-size: 22px; font-weight: 600; }}\n    p {{ margin: 6px 0 0; font-size: \
             13px; color: #6F6A85; }}\n    code {{ color: #6C5CE7; }}\n  </style>\n</head>\n\
             <body>\n  <div class=\"card\">\n    <h1>Nginx 已经跑起来了</h1>\n    <p>这是 \
             devdatapod 放的欢迎页，把站点文件放进 <code>{label}</code> 即可。</p>\n    <p>把这里\
             换成你自己的 <code>index.html</code>，刷新就是你的站点了。</p>\n  </div>\n</body>\n\
             </html>\n"
        );
        let file = root.join("index.html");
        if !crate::atomic::write_atomic(&file, page.as_bytes()) {
            log::warn(&format!(
                "nginx/{}: 无法写入欢迎页 {}",
                self.instance.id,
                file.display()
            ));
        }
    }

    fn probe_http(&self) -> (bool, String) {
        let address = "127.0.0.1";
        if probe::http_reachable(self.port, address, "localhost", PROBE_TIMEOUT) {
            (true, String::new())
        } else if !probe::is_listening(self.port, address) {
            (
                false,
                format!("127.0.0.1:{} 没有监听", self.port),
            )
        } else {
            (
                false,
                format!("127.0.0.1:{} 接受连接但没有响应", self.port),
            )
        }
    }

    fn prefix_dir(&self) -> String {
        let mut text = to_conf_path(&self.data_root());
        if !text.ends_with('/') {
            text.push('/');
        }
        text
    }
}

impl ServiceDriver for NginxDriver {
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
                "{} 下找不到 nginx",
                if self.instance.install_path.is_empty() {
                    "<无安装路径>"
                } else {
                    &self.instance.install_path
                }
            );
            return out;
        };
        out.executable = executable.clone();
        // `nginx -v` writes the version to stderr; stdout is empty.
        let result = run_once(&executable, &["-v".into()], Duration::from_secs(10), None);
        let text = format!("{} {}", result.output, result.error_output)
            .trim()
            .to_string();
        out.version = crate::run_once::extract_version(&text);
        out.installed = result.ran && !out.version.is_empty();
        out.message = if out.installed {
            text
        } else if result.ran {
            format!("{} 没有报出版本：{text}", executable.display())
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
        std::fs::create_dir_all(data_dir.join("logs"))
            .map_err(|error| format!("无法创建 {}：{error}", data_dir.display()))?;
        std::fs::create_dir_all(data_dir.join("temp"))
            .map_err(|error| format!("无法创建 {}：{error}", data_dir.display()))?;
        std::fs::create_dir_all(self.absolute_root_dir())
            .map_err(|error| format!("无法创建站点目录：{error}"))?;
        self.ensure_welcome_page();
        for sub in ["client_body", "proxy", "fastcgi", "uwsgi", "scgi"] {
            let _ = std::fs::create_dir_all(data_dir.join("temp").join(sub));
        }

        let body = self.render_block();
        match managed_block::apply(&self.conf_path(), &body) {
            Ok(BlockOutcome::Written) => Ok(()),
            Ok(BlockOutcome::Preserved) => {
                // The file is hand-written: its port may not be ours. Starting is still
                // allowed and `status` reports the mismatch instead of pretending.
                log::warn(&format!(
                    "nginx/{}: {} 没有 devdatapod 托管块，按原样使用",
                    self.instance.id,
                    self.conf_path().display()
                ));
                Ok(())
            }
            Ok(BlockOutcome::Failed) => Err(format!(
                "无法写入 {}",
                self.conf_path().display()
            )),
            Err(error) => Err(error),
        }
    }

    fn start(&mut self) -> Result<(), String> {
        let Some(program) = self.find_executable() else {
            return Err(format!("{} 未安装", self.instance.id));
        };
        // Re-render every start: the port comes from the allocator and a default switch
        // changes it, so an existing nginx.conf may name the old one (rule 2).
        self.prepare_config()?;
        if !self.runtime.supervisor.is_valid() {
            return Err(self.runtime.supervisor.problem().to_string());
        }

        let data_dir = self.data_root();
        self.runtime
            .supervisor
            .set_working_dir(data_dir.clone());
        self.runtime
            .supervisor
            .set_log_file(self.console_log());
        self.runtime.mark_started();

        // `-p` pinned to the instance datadir so nginx does not write pid/temp into
        // `Extensions`.
        let arguments = vec![
            "-p".to_string(),
            self.prefix_dir(),
            "-c".to_string(),
            to_conf_path(&self.conf_path()),
        ];
        if let Err(error) = self.runtime.supervisor.start(&program, &arguments) {
            return Err(error);
        }

        // nginx spawns first and reads its config after; a bad config exits immediately and
        // the reason only lands in error.log. Turn "starts then dies" into a start failure
        // with the actual message instead of a red Crash dot on the home page.
        let deadline = std::time::Instant::now() + START_CHECK;
        while std::time::Instant::now() < deadline {
            if !self.runtime.supervisor.is_running() {
                let why = startup_failure(&self.log_path(), &self.console_log());
                return Err(if why.is_empty() {
                    format!(
                        "nginx 启动后立即退出（退出码 {}）",
                        self.runtime.supervisor.exit_code()
                    )
                } else {
                    why
                });
            }
            if self.probe_http().0 {
                return Ok(());
            }
            std::thread::sleep(Duration::from_millis(100));
        }
        Ok(())
    }

    fn stop(&mut self) -> Result<(), String> {
        self.runtime.mark_stopped();
        let Some(program) = self.find_executable() else {
            return Ok(());
        };

        // Even when the supervisor thinks the child exited (nginx daemonises on Windows),
        // send one `quit` to collect a master that outlived its parent.
        let _ = run_once(
            &program,
            &[
                "-p".into(),
                self.prefix_dir(),
                "-c".into(),
                to_conf_path(&self.conf_path()),
                "-s".into(),
                "quit".into(),
            ],
            Duration::from_secs(5),
            Some(&self.data_root()),
        );
        if self.runtime.supervisor.wait_finished(STOP_GRACE) {
            return Ok(());
        }
        self.runtime.supervisor.graceful();
        if self.runtime.supervisor.wait_finished(Duration::from_secs(2)) {
            return Ok(());
        }
        if self.runtime.supervisor.kill_forced() {
            log::warn(&format!(
                "nginx/{}: nginx -s quit 没停掉，已强杀",
                self.instance.id
            ));
            return Ok(());
        }
        Err(format!(
            "{} 在 nginx -s quit、信号、强杀之后仍在运行",
            self.instance.id
        ))
    }

    fn status(&mut self) -> ServiceStatus {
        let (serving, detail) = self.probe_http();
        if !self.runtime.supervisor.is_running()
            && serving
            && self.runtime.last_start.is_some()
            && !self.runtime.stopped_by_us
        {
            // The port answers but our handle is gone: Windows nginx daemonises its master.
            // Not reported as healthy — stop() still sends `-s quit`.
            return ServiceStatus {
                state: crate::instance::RunState::Running,
                pid: 0,
                detail: format!("master 已脱离父进程，仍在 127.0.0.1:{} 提供服务", self.port),
            };
        }
        self.runtime.classify(serving, detail, READY_TIMEOUT_MS)
    }

    fn logs(&self, lines: usize) -> LogTail {
        read_tail(&self.log_path(), lines, 256 * 1024)
    }

    fn stats(&mut self) -> ServiceStats {
        self.runtime.stats()
    }

    fn settings_json(&self) -> Value {
        let mut object = Map::new();
        object.insert(
            "workerProcesses".into(),
            Value::from(self.settings.worker_processes),
        );
        object.insert(
            "workerConnections".into(),
            Value::from(self.settings.worker_connections),
        );
        object.insert("rootDir".into(), Value::from(self.settings.root_dir.clone()));
        object.insert("indexFiles".into(), Value::from(self.settings.index_files.clone()));
        object.insert("accessLog".into(), Value::from(self.settings.access_log));
        object.insert("gzip".into(), Value::from(self.settings.gzip));
        Value::Object(object)
    }

    fn apply_settings(&mut self, value: &Value) -> Result<(), String> {
        let map = value.as_object().cloned().unwrap_or_default();
        if !map.is_empty() {
            self.settings.worker_processes = get_i64(
                &map,
                "workerProcesses",
                self.settings.worker_processes,
            )
            .clamp(1, 64);
            self.settings.worker_connections = get_i64(
                &map,
                "workerConnections",
                self.settings.worker_connections,
            )
            .clamp(64, 65535);
            self.settings.root_dir = get_str(&map, "rootDir", &self.settings.root_dir);
            self.settings.index_files = get_str(&map, "indexFiles", &self.settings.index_files);
            self.settings.access_log = get_bool(&map, "accessLog", self.settings.access_log);
            self.settings.gzip = get_bool(&map, "gzip", self.settings.gzip);
        }
        if !self.save_settings() {
            return Err(format!("无法写入 {}", self.data_root().display()));
        }
        self.prepare_config()
    }

    fn reload(&mut self) -> Result<(), String> {
        let Some(program) = self.find_executable() else {
            return Err(format!("{} 未安装", self.instance.id));
        };
        if !self.runtime.supervisor.is_running() {
            return Err(format!("{} 没有运行，无法热重载", self.instance.id));
        }
        self.prepare_config()?;

        let base = vec![
            "-p".to_string(),
            self.prefix_dir(),
            "-c".to_string(),
            to_conf_path(&self.conf_path()),
        ];
        // `-t` before `-s reload`: a broken config should give a line number, not a service
        // that is still running while the site silently did not change.
        let mut test_args = base.clone();
        test_args.push("-t".into());
        let test = run_once(
            &program,
            &test_args,
            Duration::from_secs(5),
            Some(&self.data_root()),
        );
        let test_text = format!("{}\n{}", test.error_output, test.output)
            .trim()
            .to_string();
        if !test.ran || test.exit_code != 0 {
            return Err(if test_text.is_empty() {
                "nginx -t 失败".into()
            } else {
                test_text
            });
        }

        let mut reload_args = base;
        reload_args.push("-s".into());
        reload_args.push("reload".into());
        let sent = run_once(
            &program,
            &reload_args,
            Duration::from_secs(5),
            Some(&self.data_root()),
        );
        if !sent.ran || sent.exit_code != 0 {
            let text = format!("{}\n{}", sent.error_output, sent.output)
                .trim()
                .to_string();
            return Err(if text.is_empty() {
                "nginx -s reload 失败".into()
            } else {
                text
            });
        }
        Ok(())
    }
}
