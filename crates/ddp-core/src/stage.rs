//! Console staging: register component directories that already exist on disk, then prove
//! each one really runs.
//!
//! `ddp-cli --stage Extensions/redis3.0.504` is the pre-installer entry point. Only three
//! fields of a `ServiceInstance` are not derived — kind, version, and location — and all
//! three come out of the directory name (`Extensions/redis3.0.504`, `Extensions/MySQL8.0.12`),
//! which is the layout the manifest-driven installer will produce later.
//!
//! `ddp-cli --run <id>` starts one instance, waits for its protocol probe to say Running,
//! then stops it again. That is the acceptance test for a driver: "process exists" is not
//! "service is up", and only `status()` knows the difference.

use std::path::{Path, PathBuf};

use crate::driver::Detection;
use crate::engine::EngineState;
use crate::instance::{RunState, ServiceInstance};
use crate::kinds;
use crate::paths::Paths;
use crate::run_once::extract_version;

pub struct Candidate {
    pub kind: String,
    pub version: String,
    /// Relative to the install root, forward slashes.
    pub install_path: String,
}

/// The argument may be relative to the install root or absolute inside it. Escaping the root
/// is refused rather than rewritten: `installPath` is stored relative (AGENTS.md rule 7), and
/// a leading `..` only shows up later as a service that fails on another machine.
pub fn resolve_argument(paths: &Paths, argument: &str) -> Result<Candidate, String> {
    let cleaned = argument.trim().replace('\\', "/");
    let cleaned = cleaned.trim_end_matches('/');
    if cleaned.is_empty() {
        return Err("empty path".into());
    }
    let root = paths.root();
    let given = Path::new(cleaned);
    let relative: PathBuf = if given.is_absolute() {
        match given.strip_prefix(root) {
            Ok(rest) => rest.to_path_buf(),
            Err(_) => {
                return Err(format!(
                    "\"{argument}\" is outside the install root {}",
                    root.display()
                ))
            }
        }
    } else {
        given.to_path_buf()
    };
    let parts: Vec<String> = relative
        .components()
        .filter_map(|component| match component {
            std::path::Component::Normal(text) => Some(text.to_string_lossy().to_string()),
            std::path::Component::ParentDir => Some("..".to_string()),
            _ => None,
        })
        .collect();
    if parts.is_empty() {
        return Err(format!("\"{argument}\" is the install root itself"));
    }
    if parts[0] == ".." {
        return Err(format!(
            "\"{argument}\" is outside the install root {}",
            root.display()
        ));
    }
    let install_path = parts.join("/");
    let absolute = paths.resolve(&install_path);
    if !absolute.is_dir() {
        return Err(format!("no such directory: {}", absolute.display()));
    }

    // Longest kind key wins, so a `mariadb10.6` directory can never read as mysql.
    let name = parts.last().expect("non-empty");
    let lower = name.to_lowercase();
    let mut best: Option<&kinds::Meta> = None;
    for meta in kinds::all() {
        if !lower.starts_with(&meta.key.to_lowercase()) {
            continue;
        }
        if best.map(|current| meta.key.len() > current.key.len()).unwrap_or(true) {
            best = Some(meta);
        }
    }
    let Some(meta) = best else {
        let known: Vec<&str> = kinds::all().iter().map(|meta| meta.key).collect();
        return Err(format!(
            "\"{name}\" does not start with a known kind ({})",
            known.join(", ")
        ));
    };
    let version = extract_version(&name[meta.key.len()..]);
    if version.is_empty() {
        return Err(format!("\"{name}\" carries no version after the kind name"));
    }
    Ok(Candidate {
        kind: meta.key.to_string(),
        version,
        install_path,
    })
}

/// `--stage`: register, reconcile, then detect each requested directory.
pub fn run_stage(paths: &Paths, arguments: &[String]) -> i32 {
    println!("devdatapod stage, install root {}", paths.root().display());
    if arguments.is_empty() {
        println!(
            "usage: ddp-cli --stage <dir>...\n  \
             dir is inside the install root and named <kind><version>, e.g.\n  \
             --stage Extensions/redis3.0.504 Extensions/redis7.2.5"
        );
        return 1;
    }

    let mut state = EngineState::new(paths.clone());
    if !state.ports.is_loaded() {
        println!("[FAIL] ports.json unusable: {}", state.ports.problem());
        return 1;
    }
    if !state.registry.is_loaded() {
        println!(
            "[FAIL] services.json unusable: {}",
            state.registry.problem()
        );
        return 1;
    }
    if !state.registry.is_healthy() {
        println!(
            "[FAIL] services.json contradicts itself, writes are refused: {}",
            state.registry.problem()
        );
        return 1;
    }

    let mut ids: Vec<String> = Vec::new();
    let mut failures = 0;
    for argument in arguments {
        let candidate = match resolve_argument(paths, argument) {
            Ok(candidate) => candidate,
            Err(why) => {
                println!("[FAIL] {argument}: {why}");
                failures += 1;
                continue;
            }
        };
        let id = ServiceInstance::make_id(&candidate.kind, &candidate.version);
        if let Some(known) = state.registry.find(&id) {
            println!(
                "[ok]   {id} is already registered at {}",
                known.install_path
            );
        } else {
            let instance = ServiceInstance {
                id: id.clone(),
                kind: candidate.kind.clone(),
                version: candidate.version.clone(),
                is_default: false,
                port_key: ServiceInstance::make_port_key(&candidate.kind, &candidate.version),
                install_path: candidate.install_path.clone(),
                data_dir: ServiceInstance::make_data_dir(&candidate.kind, &candidate.version),
            };
            // `add` fills the role, makes the first instance of a kind its default, and
            // writes the same file the panel writes.
            if !state.registry.add(instance) {
                println!("[FAIL] {id}: {}", state.registry.problem());
                failures += 1;
                continue;
            }
            println!("[ok]   registered {id} from {}", candidate.install_path);
        }
        ids.push(id);
    }

    // Reconcile before probing: a driver needs the port its role calls for, and this is the
    // same call the panel makes on startup, so the table below is what the panel will show.
    if let Err(error) = state.synchronize() {
        println!("[FAIL] reconcile: {error}");
        failures += 1;
    }

    for id in &ids {
        let detection = match state.driver(id) {
            Ok(driver) => driver.detect(),
            Err(error) => {
                // A kind without a driver still registers fine; its row and port are what
                // was asked for, so this is not a staging failure.
                println!("[skip] {id}: {error}");
                continue;
            }
        };
        if !detection.installed {
            println!("[FAIL] {id}: {}", detection.message);
            failures += 1;
            continue;
        }
        let filed_version = state
            .registry
            .find(id)
            .map(|instance| instance.version.clone())
            .unwrap_or_default();
        if !detection.version.is_empty() && detection.version != filed_version {
            println!(
                "[FAIL] {id}: filed as {filed_version} but the binary answers {} - rename the directory",
                detection.version
            );
            failures += 1;
            continue;
        }
        println!(
            "[ok]   {id} runs: {}",
            detection.executable.display()
        );
    }

    println!();
    println!("  {}", state.registry.file_path().display());
    println!("  {}", state.ports.file_path().display());
    println!();
    println!("  {:<22} {:<10} {:<6} installPath", "id", "role", "port");
    for instance in state.registry.all() {
        let port = state.ports.port_of(&instance.port_key);
        println!(
            "  {:<22} {:<10} {:<6} {}",
            instance.id,
            if instance.is_default {
                "default"
            } else {
                "secondary"
            },
            port,
            instance.install_path
        );
    }
    println!();
    if failures == 0 {
        0
    } else {
        1
    }
}

/// `--run`: start one instance, wait for a real protocol answer, stop it cleanly.
pub fn run_instance(paths: &Paths, id: &str) -> i32 {
    println!("devdatapod run, install root {}", paths.root().display());
    let mut state = EngineState::new(paths.clone());
    if !state.ports.is_loaded() {
        println!("[FAIL] ports.json unusable: {}", state.ports.problem());
        return 1;
    }
    if !state.registry.is_loaded() || !state.registry.is_healthy() {
        println!(
            "[FAIL] services.json unusable: {}",
            state.registry.problem()
        );
        return 1;
    }
    if let Err(error) = state.synchronize() {
        println!("[FAIL] reconcile: {error}");
        return 1;
    }
    if state.registry.find(id).is_none() {
        println!(
            "[FAIL] no instance \"{id}\" in {}",
            state.registry.file_path().display()
        );
        return 1;
    }

    let detection: Detection = match state.driver(id) {
        Ok(driver) => driver.detect(),
        Err(error) => {
            println!("[FAIL] no driver for {id}: {error}");
            return 1;
        }
    };
    println!(
        "  detect   {}  {}  {}",
        if detection.installed { "installed" } else { "missing" },
        detection.executable.display(),
        detection.version
    );
    if !detection.installed {
        println!("[FAIL] {}", detection.message);
        return 1;
    }

    if let Err(error) = state.start(id) {
        println!("[FAIL] start: {error}");
        return 1;
    }

    // `status()` is the only honest "it is up": every driver answers Running only after its
    // protocol probe succeeded, so this loop is the acceptance test, not a sleep.
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(20);
    let mut last_line = String::new();
    let mut final_state = RunState::Starting;
    while std::time::Instant::now() < deadline {
        let status = match state.driver(id) {
            Ok(driver) => driver.status(),
            Err(_) => break,
        };
        final_state = status.state;
        let line = format!(
            "{}{}",
            state_key(final_state),
            if status.detail.is_empty() {
                String::new()
            } else {
                format!("  {}", status.detail)
            }
        );
        if line != last_line {
            println!("  state    {line}");
            last_line = line;
        }
        if matches!(
            final_state,
            RunState::Running | RunState::Crashed | RunState::Stopped
        ) {
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(200));
    }
    if final_state != RunState::Running {
        println!("[FAIL] never reached Running");
        let tail = state
            .driver(id)
            .map(|driver| driver.logs(15))
            .ok();
        if let Some(tail) = tail {
            if !tail.text.trim().is_empty() {
                println!("{}", tail.text);
            }
        }
        let _ = state.stop(id);
        return 1;
    }

    let port = state.ports.port_of(
        &state
            .registry
            .find(id)
            .map(|instance| instance.port_key.clone())
            .unwrap_or_default(),
    );
    let pid = state.driver(id).map(|driver| driver.status().pid).unwrap_or(0);
    println!("  pid      {pid}  port {port}");
    if let Ok(driver) = state.driver(id) {
        let tail = driver.logs(12);
        if !tail.text.trim().is_empty() {
            println!("-- {} ({})", tail.path, tail.encoding);
            println!("{}", tail.text);
        }
    }

    if let Err(error) = state.stop(id) {
        println!("[FAIL] stop: {error}");
        return 1;
    }
    println!("[ok]   {id} reached Running on 127.0.0.1:{port} and stopped cleanly");
    0
}

fn state_key(state: RunState) -> &'static str {
    match state {
        RunState::Idle => "idle",
        RunState::Starting => "starting",
        RunState::Running => "running",
        RunState::Switching => "switching",
        RunState::Stopped => "stopped",
        RunState::Crashed => "crashed",
    }
}
