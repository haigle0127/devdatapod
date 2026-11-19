//! The single owner of the instance model, and the only object that resolves a kind-level
//! request.
//!
//! A driver has no "start MySQL" entry point: a vague request becomes
//! `kind -> registry.default_of(kind) -> port from the allocator -> that instance's driver`
//! here, so the ambiguity never reaches a driver.
//!
//! Concurrency model: one actor thread owns every mutable piece (registry, ports, drivers,
//! job objects). The UI sends commands over a channel and reads an immutable
//! `Arc<Snapshot>` at whatever rate it likes. Nothing in this file knows about a windowing
//! toolkit, which is what makes the v1.1 split into an IPC-served process possible.

use std::collections::BTreeMap;
use std::sync::mpsc::{channel, Receiver, Sender};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::Duration;

use serde_json::Value;

use crate::driver::{self, ServiceDriver};
use crate::instance::{RunState, ServiceInstance};
use crate::kinds;
use crate::log;
use crate::orphan;
use crate::paths::Paths;
use crate::ports::{Allocation, PortAllocator};
use crate::probe;
use crate::registry::InstanceRegistry;
use crate::sites::{self, Site};
use crate::switcher;

#[derive(Clone, Debug, PartialEq)]
pub struct Row {
    pub instance: ServiceInstance,
    pub port: u16,
    pub state: RunState,
    pub pid: u32,
    pub detail: String,
    pub cpu_percent: f64,
    pub rss_bytes: u64,
    pub uptime_ms: u64,
}

#[derive(Clone, Debug)]
pub struct PortView {
    pub key: String,
    pub port: u16,
    pub owner: String,
    pub role: String,
}

#[derive(Clone, Debug, Default)]
pub struct Snapshot {
    pub ready: bool,
    pub problem: String,
    pub rows: Vec<Row>,
    pub ports: Vec<PortView>,
    pub panel: BTreeMap<String, i32>,
    /// Monotonic counter so a UI can tell an unchanged snapshot from a new one.
    pub sequence: u64,
}

impl Snapshot {
    pub fn row(&self, id: &str) -> Option<&Row> {
        self.rows.iter().find(|row| row.instance.id == id)
    }

    pub fn rows_of_kind(&self, kind: &str) -> Vec<&Row> {
        self.rows
            .iter()
            .filter(|row| row.instance.kind == kind)
            .collect()
    }

    pub fn default_of(&self, kind: &str) -> Option<&Row> {
        self.rows
            .iter()
            .find(|row| row.instance.kind == kind && row.instance.is_default)
    }

    pub fn kinds_present(&self) -> Vec<String> {
        let mut out: Vec<String> = Vec::new();
        for row in &self.rows {
            if !out.contains(&row.instance.kind) {
                out.push(row.instance.kind.clone());
            }
        }
        out
    }

    pub fn port_of(&self, id: &str) -> u16 {
        self.row(id).map(|row| row.port).unwrap_or(0)
    }

    pub fn any_running(&self) -> bool {
        self.rows
            .iter()
            .any(|row| matches!(row.state, RunState::Running | RunState::Starting))
    }
}

// ------------------------------------------------------------------------- actor state

pub(crate) struct EngineState {
    pub paths: Paths,
    pub ports: PortAllocator,
    pub registry: InstanceRegistry,
    pub drivers: BTreeMap<String, Box<dyn ServiceDriver>>,
    ready: bool,
    problem: String,
    sequence: u64,
}

impl EngineState {
    pub fn new(paths: Paths) -> Self {
        let mut ports = PortAllocator::under(paths.root());
        ports.load();
        let mut registry = InstanceRegistry::under(paths.root());
        registry.load();
        EngineState {
            paths,
            ports,
            registry,
            drivers: BTreeMap::new(),
            ready: false,
            problem: String::new(),
            sequence: 0,
        }
    }

    // ------------------------------------------------------------------ reconcile
    /// Reconcile `services.json` against `ports.json`. Every instance gets an allocation —
    /// the kind's canonical port when it is the default, a slot port otherwise — and a role
    /// that disagrees between the two files is repaired with one `swap_roles()` write.
    ///
    /// Refuses when no counterpart exists to swap with: inventing a port there is how two
    /// services end up fighting for 3306.
    pub fn synchronize(&mut self) -> Result<(), String> {
        self.ready = false;
        self.problem.clear();

        if !self.ports.is_loaded() || !self.registry.is_loaded() {
            return self.fail("端口或实例存储加载失败".into());
        }
        // Either file may be readable-but-frozen. Both cases are reported rather than
        // healed: the fix belongs to whoever edited the file, not to a guess made here.
        if !self.registry.is_healthy() {
            return self.fail(format!("services.json: {}", self.registry.problem()));
        }
        if !self.ports.is_healthy() {
            return self.fail(format!("ports.json: {}", self.ports.problem()));
        }

        let known: Vec<String> = self
            .registry
            .all()
            .iter()
            .filter(|instance| !instance.port_key.is_empty())
            .map(|instance| instance.port_key.clone())
            .collect();

        // 1. Leftovers from an uninstall. A released port that nobody frees turns into
        //    "the band is full" after a few install/remove cycles.
        let stale: Vec<String> = self
            .ports
            .allocations()
            .iter()
            .filter(|item| item.owner != "panel" && !known.contains(&item.key))
            .map(|item| item.key.clone())
            .collect();
        for key in stale {
            if !self.ports.release(&key) {
                return self.fail(format!("无法释放 {key} 占用的端口"));
            }
            log::info(&format!("engine: released {key}, its instance is gone"));
        }

        // 2. Role drift, before any port is claimed: healing afterwards would find the
        //    canonical port already held by the instance that should have lost it.
        let kinds_present = self.registry.kinds_present();
        for kind in kinds_present {
            if let Err(error) = self.heal_role_drift(&kind) {
                return self.fail(error);
            }
        }

        // 3. Fill whatever is still missing.
        let all = self.registry.all().to_vec();
        for instance in &all {
            if let Err(error) = self.ensure_allocation(instance) {
                return self.fail(error);
            }
        }

        self.ready = true;
        let allocations = self.ports.allocations().len();
        log::info(&format!(
            "engine: {} instance(s) reconciled against {allocations} allocation(s)",
            all.len()
        ));
        Ok(())
    }

    fn fail(&mut self, text: String) -> Result<(), String> {
        self.problem = text.clone();
        log::error(&format!("engine: {text}"));
        Err(text)
    }

    fn heal_role_drift(&mut self, kind: &str) -> Result<(), String> {
        let Some(want_id) = self.registry.default_id(kind).map(str::to_string) else {
            return Ok(());
        };
        let Some(want) = self.registry.find(&want_id).cloned() else {
            return Ok(());
        };

        let mut current = String::new();
        for item in self.ports.allocations_of(kind) {
            if item.role != "default" {
                continue;
            }
            if !current.is_empty() {
                return Err(format!(
                    "{kind}: ports.json 里 {current} 和 {} 都是 default",
                    item.key
                ));
            }
            current = item.key;
        }
        // No default anywhere is not drift — step 3 hands out the canonical port.
        if current.is_empty() || current == want.port_key {
            return Ok(());
        }

        // The two files name different defaults. That is only repairable when the wanted
        // instance currently holds a slot port: then one atomic swap makes both agree.
        if want.port_key.is_empty() || self.ports.role_of(&want.port_key) != "secondary" {
            return Err(format!(
                "{kind}: ports.json 里没有可以给 {} 交换的槽位分配，请手工修复 ports.json",
                want.port_key
            ));
        }
        if !self.ports.swap_roles(&current, &want.port_key) {
            return Err(format!(
                "{kind}: 无法把默认端口从 {current} 移到 {}",
                want.port_key
            ));
        }
        for candidate in self.registry.of_kind(kind) {
            if candidate.port_key == current || candidate.port_key == want.port_key {
                let _ = self.invalidate(&candidate.id);
            }
        }
        log::warn(&format!(
            "engine: {kind} default healed from {current} to {}",
            want.port_key
        ));
        Ok(())
    }

    fn ensure_allocation(&mut self, instance: &ServiceInstance) -> Result<(), String> {
        if instance.port_key.is_empty() {
            return Ok(());
        }
        let Some(meta) = kinds::find(&instance.kind) else {
            return Err(format!("{}: 未知类型 \"{}\"", instance.id, instance.kind));
        };

        if instance.is_default && !meta.canonical.is_empty() {
            let Some(canonical) = self.ports.canonical_port(meta.canonical) else {
                return Err(format!(
                    "{}: ports.json 里没有 \"{}\" 的规范端口",
                    instance.id, meta.canonical
                ));
            };
            let already_ours = self.ports.port_of(&instance.port_key) == canonical
                && self.ports.role_of(&instance.port_key) == "default";
            if !already_ours
                && canonical > 0
                && !probe::is_port_free(canonical, self.ports.bind_address())
            {
                // Rule 3: name the holder. "Stop that one first" is the only honest answer.
                let holder = probe::identify_owner(canonical);
                let who = if holder.occupied {
                    if holder.process_name.is_empty() {
                        format!("pid {}", holder.pid)
                    } else {
                        format!("{} (pid {})", holder.process_name, holder.pid)
                    }
                } else {
                    "unknown".to_string()
                };
                return Err(format!(
                    "{} 需要规范端口 {canonical}，但 {who} 已经在那里监听",
                    instance.id
                ));
            }
            if !self
                .ports
                .assign_canonical(meta.canonical, &instance.port_key, &instance.kind)
            {
                return Err(format!(
                    "{}: 无法占用规范端口 {canonical}",
                    instance.id
                ));
            }
            return Ok(());
        }

        if meta.slot.is_empty() {
            return Ok(()); // python: a kind with no listening socket
        }
        let allocated = self.ports.allocate_slot(meta.slot, &instance.port_key, &instance.kind);
        if allocated <= 0 {
            return Err(format!(
                "{}: {} 槽位段里没有空闲端口",
                instance.id, meta.slot
            ));
        }
        Ok(())
    }

    // ------------------------------------------------------------------ drivers
    fn ensure_driver(&mut self, id: &str) -> Result<(), String> {
        if self.drivers.contains_key(id) {
            return Ok(());
        }
        let Some(instance) = self.registry.find(id).cloned() else {
            return Err(format!(
                "no instance \"{id}\" in {}",
                self.registry.file_path().display()
            ));
        };
        let port = self.ports.port_of(&instance.port_key);
        if instance.has_port() && port == 0 {
            return Err(format!(
                "{} 没有端口分配，请先同步 ports.json",
                instance.id
            ));
        }
        let created = driver::create(&self.paths, &instance, port)?;
        self.drivers.insert(id.to_string(), created);
        Ok(())
    }

    pub fn driver(&mut self, id: &str) -> Result<&mut dyn ServiceDriver, String> {
        self.ensure_driver(id)?;
        Ok(self.drivers.get_mut(id).expect("just inserted").as_mut())
    }

    /// Drop the cached driver so the next call rebuilds it from the current registry entry
    /// and allocation. Returns `false` for a live instance: destroying a driver destroys its
    /// supervisor, and closing the job handle is exactly what kills the service.
    pub fn invalidate(&mut self, id: &str) -> bool {
        if !self.drivers.contains_key(id) {
            return true;
        }
        let state = self
            .drivers
            .get_mut(id)
            .map(|driver| driver.status().state)
            .unwrap_or(RunState::Stopped);
        if state != RunState::Stopped && state != RunState::Idle {
            self.problem = format!("{id} 必须先停止才能重建驱动");
            log::error(&format!("engine: {}", self.problem));
            return false;
        }
        self.drivers.remove(id);
        true
    }

    // ------------------------------------------------------------------ actions
    pub fn start(&mut self, id: &str) -> Result<(), String> {
        self.ensure_driver(id)?;
        let driver = self.drivers.get_mut(id).expect("ensured");
        driver
            .start()
            .map_err(|error| {
                log::error(&format!("engine: start {id} failed: {error}"));
                error
            })?;
        log::info(&format!(
            "engine: {id} starting on port {}",
            self.drivers.get(id).map(|driver| driver.port()).unwrap_or(0)
        ));
        Ok(())
    }

    pub fn stop(&mut self, id: &str) -> Result<(), String> {
        let Some(driver) = self.drivers.get_mut(id) else {
            return Ok(()); // never started: already stopped
        };
        driver.stop().map_err(|error| {
            log::error(&format!("engine: stop {id} failed: {error}"));
            error
        })
    }

    pub fn restart(&mut self, id: &str) -> Result<(), String> {
        self.stop(id)?;
        self.start(id)
    }

    pub fn reload(&mut self, id: &str) -> Result<(), String> {
        let Some(driver) = self.drivers.get_mut(id) else {
            return Err(format!("{id} 没有运行，无法热重载"));
        };
        driver.reload().map_err(|error| {
            log::warn(&format!("engine: reload {id} failed: {error}"));
            error
        })?;
        log::info(&format!("engine: {id} reloaded its configuration"));
        Ok(())
    }

    pub fn is_running(&mut self, id: &str) -> bool {
        self.drivers
            .get_mut(id)
            .map(|driver| driver.status().state == RunState::Running)
            .unwrap_or(false)
    }

    pub fn start_kind(&mut self, kind: &str) -> Result<(), String> {
        let target = self
            .registry
            .default_of(kind)
            .map(|instance| instance.id.clone())
            .ok_or_else(|| format!("{} 还没有安装默认实例", kinds::label(kind)))?;
        self.start(&target)
    }

    pub fn stop_kind(&mut self, kind: &str) -> Result<(), String> {
        let target = self
            .registry
            .default_of(kind)
            .map(|instance| instance.id.clone())
            .ok_or_else(|| format!("{} 没有默认实例", kinds::label(kind)))?;
        self.stop(&target)
    }

    pub fn stop_all(&mut self) {
        // Reverse install order: a php-fpm stopped after the nginx pointing at it produces
        // fewer error lines than the other way round.
        let ids: Vec<String> = self.registry.all().iter().rev().map(|item| item.id.clone()).collect();
        for id in ids {
            if !self.drivers.contains_key(&id) {
                continue;
            }
            if let Err(error) = self.stop(&id) {
                log::error(&format!("engine: {id} survived shutdown: {error}"));
            }
        }
    }

    // ------------------------------------------------------------------ registry
    pub fn register(&mut self, mut instance: ServiceInstance) -> Result<(), String> {
        if instance.install_path.is_empty() {
            return Err("没有安装路径，无法注册".into());
        }
        if instance.version.is_empty() {
            return Err("没有版本号，无法注册".into());
        }
        if instance.id.is_empty() {
            instance.id = ServiceInstance::make_id(&instance.kind, &instance.version);
        }
        if !self.registry.add(instance.clone()) {
            return Err(format!("无法写入 {}", self.registry.file_path().display()));
        }
        self.synchronize()?;
        // Verify the binary really runs and reports the version the directory name claims.
        let detection = self.driver(&instance.id)?.detect();
        if !detection.installed {
            return Err(format!(
                "{} 注册成功但无法运行：{}",
                instance.id, detection.message
            ));
        }
        if !detection.version.is_empty() && detection.version != instance.version {
            return Err(format!(
                "{} 的目录名是 {}，但二进制报 {}",
                instance.id, instance.version, detection.version
            ));
        }
        Ok(())
    }

    pub fn unregister(&mut self, id: &str) -> Result<(), String> {
        let Some(instance) = self.registry.find(id).cloned() else {
            return Ok(());
        };
        // Stop first: removing the driver of a live service would kill it through the job
        // handle and look like a crash.
        self.stop(id)?;
        self.drivers.remove(id);
        if !self.registry.remove(id) {
            return Err(format!("无法写入 {}", self.registry.file_path().display()));
        }
        if !instance.port_key.is_empty() {
            self.ports.release(&instance.port_key);
        }
        self.synchronize()?;
        crate::install::remove_install_dir(&self.paths, &instance.install_path)?;
        Ok(())
    }

    pub fn save_settings(&mut self, id: &str, value: &Value) -> Result<(), String> {
        self.driver(id)?.apply_settings(value)?;
        Ok(())
    }

    /// Apply a config-dialog submission: settings first, then the port move, then the
    /// optional restart. The order matters — a port move needs the service stopped, and a
    /// restart must read the *new* settings.
    pub fn save_config(
        &mut self,
        id: &str,
        settings: &Value,
        new_port: Option<u16>,
        restart: bool,
    ) -> Result<(), String> {
        let Some(instance) = self.registry.find(id).cloned() else {
            return Err(format!("没有实例 {id}"));
        };
        let current_port = self.ports.port_of(&instance.port_key);
        let port_changed = new_port.map(|port| port != current_port).unwrap_or(false);

        if port_changed {
            // A live service would keep the old socket. Stop before touching the plan.
            self.stop(id)?;
            let wanted = new_port.unwrap_or(current_port);
            if !self.ports.set_port(&instance.port_key, wanted) {
                return Err(format!(
                    "端口 {wanted} 不在规划内，或者已经被别的服务占用"
                ));
            }
            let _ = self.invalidate(id);
        }

        // A driver was built from the old allocation; `save_settings` would otherwise write
        // into the old data dir only when the kind is unchanged, which it is — but the
        // cached driver is fine after a settings-only change.
        self.driver(id)?.apply_settings(settings)?;

        if restart {
            self.start(id)?;
        }
        // Nginx vhosts reference service ports; a move must be re-rendered even when the
        // service is stopped (the config is written on next start).
        if port_changed {
            self.rerender_nginx()?;
        }
        log::info(&format!(
            "engine: {id} configuration saved{}",
            if port_changed {
                format!(", port {} -> {}", current_port, new_port.unwrap_or(0))
            } else {
                String::new()
            }
        ));
        Ok(())
    }

    pub fn save_site(&mut self, site: Site, previous_domain: Option<&str>) -> Result<(), String> {
        let (mut items, problem) = sites::load(self.paths.root());
        if let Some(problem) = problem {
            return Err(problem);
        }
        sites::validate(&site, previous_domain, &items)?;
        sites::prepare_root(&site)?;

        match previous_domain.map(sites::normalize) {
            Some(previous) if sites::find(&items, &previous).is_some() => {
                let index = items
                    .iter()
                    .position(|item| sites::normalize(&item.domain) == previous)
                    .expect("found above");
                items[index] = site;
            }
            _ => items.push(site),
        }
        sites::save(self.paths.root(), &items)?;
        self.rerender_nginx()
    }

    pub fn remove_site(&mut self, domain: &str) -> Result<(), String> {
        let (mut items, problem) = sites::load(self.paths.root());
        if let Some(problem) = problem {
            return Err(problem);
        }
        let key = sites::normalize(domain);
        items.retain(|item| sites::normalize(&item.domain) != key);
        sites::save(self.paths.root(), &items)?;
        // Removing a site only removes its vhost; the files on disk stay.
        self.rerender_nginx()
    }

    /// Re-render every nginx instance and hot-reload the ones that are running. This is the
    /// cascade for a port move or a site edit: nothing else references a service port.
    pub fn rerender_nginx(&mut self) -> Result<(), String> {
        let ids: Vec<String> = self
            .registry
            .all()
            .iter()
            .filter(|instance| instance.kind == "nginx")
            .map(|instance| instance.id.clone())
            .collect();
        let mut first_error: Option<String> = None;
        for id in ids {
            self.ensure_driver(&id)?;
            let running = self
                .drivers
                .get_mut(&id)
                .map(|driver| driver.status().state == RunState::Running)
                .unwrap_or(false);
            let result = if running {
                self.drivers.get_mut(&id).expect("ensured").reload()
            } else {
                self.drivers.get_mut(&id).expect("ensured").prepare_config()
            };
            if let Err(error) = result {
                if first_error.is_none() {
                    first_error = Some(error);
                }
            }
        }
        match first_error {
            Some(error) => Err(error),
            None => Ok(()),
        }
    }

    pub fn promote_default(&mut self, id: &str) -> Result<(), String> {
        switcher::promote_default(self, id)
    }

    // ------------------------------------------------------------------ observation
    pub fn rows(&mut self) -> Vec<Row> {
        let instances = self.registry.all().to_vec();
        let mut out = Vec::with_capacity(instances.len());
        for instance in instances {
            let port = self.ports.port_of(&instance.port_key);
            let mut row = Row {
                instance: instance.clone(),
                port,
                state: RunState::Idle,
                pid: 0,
                detail: String::new(),
                cpu_percent: 0.0,
                rss_bytes: 0,
                uptime_ms: 0,
            };
            match self.ensure_driver(&instance.id) {
                Ok(()) => {
                    let driver = self.drivers.get_mut(&instance.id).expect("ensured");
                    let status = driver.status();
                    let stats = driver.stats();
                    row.state = status.state;
                    row.pid = status.pid;
                    row.detail = status.detail;
                    row.cpu_percent = stats.cpu_percent;
                    row.rss_bytes = stats.rss_bytes;
                    row.uptime_ms = stats.uptime_ms;
                }
                Err(error) => {
                    // No driver means we cannot observe it — not that it is stopped. Idle is
                    // "installed but nothing to run yet", and the detail carries the reason.
                    row.state = RunState::Idle;
                    row.detail = error;
                }
            }
            out.push(row);
        }
        out
    }

    pub fn is_ready(&self) -> bool {
        self.ready
    }

    pub fn snapshot(&mut self) -> Snapshot {
        self.sequence += 1;
        let ports = self
            .ports
            .allocations()
            .iter()
            .map(|item: &Allocation| PortView {
                key: item.key.clone(),
                port: item.port,
                owner: item.owner.clone(),
                role: item.role.clone(),
            })
            .collect();
        let panel: BTreeMap<String, i32> = ["ipc", "api", "adminer", "download"]
            .iter()
            .map(|name| (name.to_string(), self.ports.panel_port(name)))
            .collect();
        let rows = self.rows();
        Snapshot {
            ready: self.ready,
            problem: self.problem.clone(),
            rows,
            ports,
            panel,
            sequence: self.sequence,
        }
    }

    pub fn shutdown(&mut self) {
        self.stop_all();
    }
}

// ------------------------------------------------------------------------- handle

enum Command {
    Start {
        id: String,
        reply: Sender<Result<(), String>>,
    },
    Stop {
        id: String,
        reply: Sender<Result<(), String>>,
    },
    Restart {
        id: String,
        reply: Sender<Result<(), String>>,
    },
    Reload {
        id: String,
        reply: Sender<Result<(), String>>,
    },
    StartKind {
        kind: String,
        reply: Sender<Result<(), String>>,
    },
    StopKind {
        kind: String,
        reply: Sender<Result<(), String>>,
    },
    SwitchDefault {
        id: String,
        reply: Sender<Result<(), String>>,
    },
    Register {
        instance: ServiceInstance,
        reply: Sender<Result<(), String>>,
    },
    Unregister {
        id: String,
        reply: Sender<Result<(), String>>,
    },
    SaveSettings {
        id: String,
        settings: Value,
        reply: Sender<Result<(), String>>,
    },
    Settings {
        id: String,
        reply: Sender<Result<Value, String>>,
    },
    SaveConfig {
        id: String,
        settings: Value,
        new_port: Option<u16>,
        restart: bool,
        reply: Sender<Result<(), String>>,
    },
    SaveSite {
        site: Site,
        previous: Option<String>,
        reply: Sender<Result<(), String>>,
    },
    RemoveSite {
        domain: String,
        reply: Sender<Result<(), String>>,
    },
    Synchronize {
        reply: Sender<Result<(), String>>,
    },
    Shutdown,
}

/// The UI-facing half. Cheap to clone the snapshot; every mutating method blocks until the
/// actor has finished, which is what makes the return value mean "done", not "queued".
pub struct Engine {
    tx: Sender<Command>,
    snapshot: Arc<Mutex<Arc<Snapshot>>>,
    join: Mutex<Option<JoinHandle<()>>>,
}

impl Engine {
    pub fn spawn(paths: Paths) -> Self {
        let (tx, rx) = channel::<Command>();
        let snapshot = Arc::new(Mutex::new(Arc::new(Snapshot::default())));
        let sink = snapshot.clone();
        let join = std::thread::Builder::new()
            .name("ddp-engine".into())
            .spawn(move || run_actor(paths, rx, sink))
            .expect("cannot start the engine thread");
        Engine {
            tx,
            snapshot,
            join: Mutex::new(Some(join)),
        }
    }

    pub fn snapshot(&self) -> Arc<Snapshot> {
        self.snapshot
            .lock()
            .map(|guard| guard.clone())
            .unwrap_or_else(|poisoned| poisoned.into_inner().clone())
    }

    fn ask(&self, build: impl FnOnce(Sender<Result<(), String>>) -> Command) -> Result<(), String> {
        let (reply, rx) = channel();
        self.tx
            .send(build(reply))
            .map_err(|_| "引擎线程已经退出".to_string())?;
        rx.recv()
            .map_err(|_| "引擎线程没有回应".to_string())?
    }

    pub fn start(&self, id: &str) -> Result<(), String> {
        self.ask(|reply| Command::Start {
            id: id.to_string(),
            reply,
        })
    }

    pub fn stop(&self, id: &str) -> Result<(), String> {
        self.ask(|reply| Command::Stop {
            id: id.to_string(),
            reply,
        })
    }

    pub fn restart(&self, id: &str) -> Result<(), String> {
        self.ask(|reply| Command::Restart {
            id: id.to_string(),
            reply,
        })
    }

    pub fn reload(&self, id: &str) -> Result<(), String> {
        self.ask(|reply| Command::Reload {
            id: id.to_string(),
            reply,
        })
    }

    pub fn start_kind(&self, kind: &str) -> Result<(), String> {
        self.ask(|reply| Command::StartKind {
            kind: kind.to_string(),
            reply,
        })
    }

    pub fn stop_kind(&self, kind: &str) -> Result<(), String> {
        self.ask(|reply| Command::StopKind {
            kind: kind.to_string(),
            reply,
        })
    }

    pub fn switch_default(&self, id: &str) -> Result<(), String> {
        self.ask(|reply| Command::SwitchDefault {
            id: id.to_string(),
            reply,
        })
    }

    pub fn register(&self, instance: ServiceInstance) -> Result<(), String> {
        self.ask(|reply| Command::Register { instance, reply })
    }

    pub fn unregister(&self, id: &str) -> Result<(), String> {
        self.ask(|reply| Command::Unregister {
            id: id.to_string(),
            reply,
        })
    }

    pub fn save_settings(&self, id: &str, settings: Value) -> Result<(), String> {
        self.ask(|reply| Command::SaveSettings {
            id: id.to_string(),
            settings,
            reply,
        })
    }

    pub fn save_site(&self, site: Site, previous_domain: Option<String>) -> Result<(), String> {
        self.ask(|reply| Command::SaveSite {
            site,
            previous: previous_domain,
            reply,
        })
    }

    /// The driver's current settings, as the config dialog should show them.
    pub fn settings(&self, id: &str) -> Result<Value, String> {
        let (reply, rx) = channel();
        self.tx
            .send(Command::Settings {
                id: id.to_string(),
                reply,
            })
            .map_err(|_| "引擎线程已经退出".to_string())?;
        rx.recv()
            .map_err(|_| "引擎线程没有回应".to_string())?
    }

    pub fn remove_site(&self, domain: &str) -> Result<(), String> {
        self.ask(|reply| Command::RemoveSite {
            domain: domain.to_string(),
            reply,
        })
    }

    pub fn synchronize(&self) -> Result<(), String> {
        self.ask(|reply| Command::Synchronize { reply })
    }

    /// Save per-kind settings and, when `new_port` differs, move the allocation — then
    /// restart and re-render if asked. One call so a form cannot half-apply.
    pub fn save_config(
        &self,
        id: &str,
        settings: Value,
        new_port: Option<u16>,
        restart: bool,
    ) -> Result<(), String> {
        self.ask(|reply| Command::SaveConfig {
            id: id.to_string(),
            settings,
            new_port,
            restart,
            reply,
        })
    }

    /// Stop every service we started, then stop the actor. Called on quit.
    pub fn shutdown(&self) {
        let _ = self.tx.send(Command::Shutdown);
        if let Ok(mut guard) = self.join.lock() {
            if let Some(join) = guard.take() {
                let _ = join.join();
            }
        }
    }
}

impl Drop for Engine {
    fn drop(&mut self) {
        self.shutdown();
    }
}

fn run_actor(paths: Paths, rx: Receiver<Command>, sink: Arc<Mutex<Arc<Snapshot>>>) {
    let mut state = EngineState::new(paths);
    if let Err(error) = state.synchronize() {
        log::error(&format!("engine: initial reconcile failed: {error}"));
    }
    orphan::sweep_install_root(state.paths.root());
    publish(&sink, &mut state);

    loop {
        match rx.recv_timeout(Duration::from_secs(1)) {
            Ok(Command::Shutdown) => break,
            Ok(command) => {
                handle(&mut state, command);
                publish(&sink, &mut state);
            }
            Err(std::sync::mpsc::RecvTimeoutError::Timeout) => {
                publish(&sink, &mut state);
            }
            Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => break,
        }
    }
    state.shutdown();
}

fn publish(sink: &Arc<Mutex<Arc<Snapshot>>>, state: &mut EngineState) {
    let snapshot = Arc::new(state.snapshot());
    if let Ok(mut guard) = sink.lock() {
        *guard = snapshot;
    }
}

fn handle(state: &mut EngineState, command: Command) {
    match command {
        Command::Start { id, reply } => {
            let _ = reply.send(state.start(&id));
        }
        Command::Stop { id, reply } => {
            let _ = reply.send(state.stop(&id));
        }
        Command::Restart { id, reply } => {
            let _ = reply.send(state.restart(&id));
        }
        Command::Reload { id, reply } => {
            let _ = reply.send(state.reload(&id));
        }
        Command::StartKind { kind, reply } => {
            let _ = reply.send(state.start_kind(&kind));
        }
        Command::StopKind { kind, reply } => {
            let _ = reply.send(state.stop_kind(&kind));
        }
        Command::SwitchDefault { id, reply } => {
            let _ = reply.send(state.promote_default(&id));
        }
        Command::Register { instance, reply } => {
            let _ = reply.send(state.register(instance));
        }
        Command::Unregister { id, reply } => {
            let _ = reply.send(state.unregister(&id));
        }
        Command::SaveSettings {
            id,
            settings,
            reply,
        } => {
            let _ = reply.send(state.save_settings(&id, &settings));
        }
        Command::Settings { id, reply } => {
            let result = state.driver(&id).map(|driver| driver.settings_json());
            let _ = reply.send(result);
        }
        Command::SaveConfig {
            id,
            settings,
            new_port,
            restart,
            reply,
        } => {
            let _ = reply.send(state.save_config(&id, &settings, new_port, restart));
        }
        Command::SaveSite {
            site,
            previous,
            reply,
        } => {
            let _ = reply.send(state.save_site(site, previous.as_deref()));
        }
        Command::RemoveSite { domain, reply } => {
            let _ = reply.send(state.remove_site(&domain));
        }
        Command::Synchronize { reply } => {
            let _ = reply.send(state.synchronize());
        }
        Command::Shutdown => {}
    }
}
