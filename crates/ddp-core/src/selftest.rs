//! The invariants that must not regress, runnable without a GUI.
//!
//! `ddp-cli --selftest` prints this report; `scripts/build.ps1` runs it after a build. The
//! Qt build shipped the same suite, and these are the checks that caught real bugs:
//! canonical ports being stolen, a slot port moving on restart, a frozen file still being
//! written, and two instances both claiming to be default.

use std::path::{Path, PathBuf};

use crate::engine::EngineState;
use crate::instance::ServiceInstance;
use crate::managed_block;
use crate::paths::Paths;
use crate::ports::PortAllocator;
use crate::probe;
use crate::registry::InstanceRegistry;
use crate::sites::{self, Site};

pub struct Report {
    pub passed: u32,
    pub failed: u32,
    pub lines: Vec<String>,
}

impl Report {
    pub fn ok(&mut self, what: impl Into<String>) {
        self.passed += 1;
        self.lines.push(format!("  [ok] {}", what.into()));
    }

    pub fn check(&mut self, condition: bool, what: impl Into<String>) {
        let what = what.into();
        if condition {
            self.ok(what);
        } else {
            self.failed += 1;
            self.lines.push(format!("  [FAIL] {what}"));
        }
    }

    pub fn section(&mut self, name: &str) {
        self.lines.push(format!("\n== {name} =="));
    }

    pub fn success(&self) -> bool {
        self.failed == 0
    }
}

/// `31789` is inside the 30000-32000 band but outside every slot band, so a test canonical
/// port can never collide with a test slot port. A test may point a canonical anywhere it
/// likes; production code may not hardcode one (AGENTS.md rule 2).
const TEST_CANONICAL: u16 = 31789;

const BAND_FIXTURE: &str = r#"{
  "schema": 2,
  "band": [30000, 32000],
  "bindAddress": "127.0.0.1",
  "canonical": { "redis": 31789 },
  "slots": { "redis": [30600, 30619] },
  "stride": { "redis": 1 },
  "panel": { "api": 30010 },
  "allocations": {}
}"#;

pub fn run() -> Report {
    let mut report = Report {
        passed: 0,
        failed: 0,
        lines: Vec::new(),
    };
    let root = temp_root();
    let _ = std::fs::remove_dir_all(&root);
    let _ = std::fs::create_dir_all(&root);
    let paths = Paths::at(&root);
    let paths = paths;

    seed_and_band(&mut report, &paths);
    allocation_and_swap(&mut report, &paths);
    frozen_ports(&mut report, &paths);
    registry_defaults(&mut report, &paths);
    frozen_registry(&mut report, &paths);
    engine_reconcile(&mut report, &paths);
    managed_block_checks(&mut report, &paths);
    site_checks(&mut report, &paths);
    probe_checks(&mut report);
    log_tail_checks(&mut report, &paths);

    let _ = std::fs::remove_dir_all(&root);
    report
}

fn temp_root() -> PathBuf {
    std::env::temp_dir().join(format!(
        "ddp-selftest-{}-{}",
        std::process::id(),
        chrono::Local::now().format("%H%M%S%3f")
    ))
}

fn write(path: &Path, text: &str) {
    if let Some(parent) = path.parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    let _ = std::fs::write(path, text);
}

// ---------------------------------------------------------------- ports: seed

fn seed_and_band(report: &mut Report, paths: &Paths) {
    report.section("ports.json: seed and band table");
    let file = paths.data_dir().join("ports.json");
    let _ = std::fs::remove_file(&file);

    let mut ports = PortAllocator::under(paths.root());
    report.check(ports.load(), "load() seeds a missing ports.json");
    report.check(file.exists(), "the seeded file exists on disk");
    report.check(ports.is_loaded() && ports.is_healthy(), "the shipped seed is self-consistent");
    report.check(
        ports.band_first() == 30000 && ports.band_last() == 32000,
        "the band is 30000-32000",
    );
    report.check(
        ports.bind_address() == "127.0.0.1",
        "every allocation binds loopback only",
    );
    report.check(ports.canonical_port("mysql") == Some(3306), "canonical mysql is 3306");
    report.check(ports.canonical_port("redis") == Some(6379), "canonical redis is 6379");
    report.check(ports.canonical_port("http") == Some(80), "canonical http is 80");
    report.check(ports.canonical_port("nginx").is_none(), "nginx has no canonical band of its own");
    report.check(ports.canonical_port("php").is_none(), "php has no canonical port");
    report.check(
        ports.slot_range("redis") == Some((30600, 30619)),
        "the redis slot band is 30600-30619",
    );
    report.check(ports.slot_range("nope").is_none(), "an unknown slot band is absent");
    report.check(ports.slot_stride("phpFpm") == 10, "phpFpm stride is 10");
    report.check(ports.slot_stride("redis") == 1, "redis stride is 1");
    report.check(ports.panel_port("api") == 30010, "panel.api is 30010");
    report.check(ports.panel_port("nope") == -1, "an unknown panel port is -1");
}

// ---------------------------------------------------------------- ports: mutations

fn allocation_and_swap(report: &mut Report, paths: &Paths) {
    report.section("ports.json: allocation, swap, rollback");
    let file = paths.data_dir().join("ports.json");
    write(&file, BAND_FIXTURE);

    let mut ports = PortAllocator::under(paths.root());
    report.check(ports.load() && ports.is_healthy(), "fixture loads healthy");

    let old = "redis.3.0.504".to_string();
    let fresh = "redis.7.2.5".to_string();
    let slot_a = ports.allocate_slot("redis", &old, "redis");
    report.check(slot_a == 30600, "the first slot allocation is the band start");
    report.check(
        ports.allocate_slot("redis", &old, "redis") == slot_a,
        "re-allocating the same key is idempotent, not a reshuffle",
    );
    let slot_b = ports.allocate_slot("redis", &fresh, "redis");
    report.check(
        slot_b > slot_a && slot_b <= 30619,
        "a second instance gets a free port",
    );

    report.check(
        ports.assign_canonical("redis", &fresh, "redis"),
        "the default claims the canonical port",
    );
    report.check(
        ports.port_of(&fresh) == TEST_CANONICAL && ports.role_of(&fresh) == "default",
        "the canonical holder is marked default",
    );
    report.check(
        !ports.assign_canonical("redis", &old, "redis"),
        "a second default is refused while one already holds the canonical port",
    );
    report.check(
        ports.retarget(&fresh, "redis") == 0,
        "a default never moves into a slot band",
    );
    let moved = ports.retarget(&old, "redis");
    report.check(
        (moved as i32) > 30599 && moved <= 30619 && (moved as i32) != slot_a,
        "a secondary moves inside its own band after a conflict",
    );
    report.check(
        !ports.rebind(&old, 45555, "secondary", "redis"),
        "rebind refuses a port outside the plan",
    );
    report.check(
        ports.rebind(&old, slot_a as u16, "secondary", "redis"),
        "rebind restores a known allocation",
    );

    report.check(
        ports.swap_roles(&fresh, &old),
        "swapRoles permutes two allocations of one kind",
    );
    report.check(
        ports.port_of(&old) == TEST_CANONICAL && ports.role_of(&old) == "default",
        "after the swap the old instance is the default",
    );
    report.check(
        ports.port_of(&fresh) == slot_a as u16 && ports.role_of(&fresh) == "secondary",
        "and the new one is secondary on the old slot port",
    );
    report.check(
        ports.swap_roles(&fresh, &old),
        "swapRoles is its own inverse (rollback)",
    );
    report.check(
        ports.port_of(&fresh) == TEST_CANONICAL && ports.port_of(&old) == slot_a as u16,
        "the inverse swap restores both ports",
    );
    report.check(
        !ports.swap_roles(&fresh, "redis.missing"),
        "swapping with an unknown key is refused",
    );

    // Drift check: a saved file must be byte-identical after a load/save round trip.
    report.check(ports.release(&old), "release drops an allocation");
    report.check(!ports.contains(&old), "the released key is gone");
    report.check(ports.release(&old), "releasing twice is not an error");
    let before = std::fs::read_to_string(&file).unwrap_or_default();
    let mut reloaded = PortAllocator::under(paths.root());
    report.check(reloaded.load() && reloaded.is_healthy(), "the file reloads");
    report.check(
        reloaded.port_of(&fresh) == TEST_CANONICAL,
        "reload keeps the canonical holder",
    );
    report.check(reloaded.save(), "saving a reloaded file is allowed");
    let after = std::fs::read_to_string(&file).unwrap_or_default();
    report.check(before == after, "load -> save is byte-stable (no silent rewrite)");
}

fn frozen_ports(report: &mut Report, paths: &Paths) {
    report.section("ports.json: readable but frozen");
    let file = paths.data_dir().join("ports.json");
    write(
        &file,
        r#"{
  "schema": 2,
  "band": [30000, 32000],
  "bindAddress": "127.0.0.1",
  "canonical": { "redis": 6379 },
  "slots": { "redis": [30600, 30619] },
  "panel": {},
  "allocations": {
    "a": { "port": 30600, "owner": "redis", "role": "secondary" },
    "b": { "port": 30600, "owner": "redis", "role": "secondary" }
  }
}"#,
    );
    let mut ports = PortAllocator::under(paths.root());
    report.check(ports.load(), "a duplicated port still loads (reads must keep working)");
    report.check(
        ports.is_loaded() && !ports.is_healthy(),
        "but it is marked frozen",
    );
    report.check(
        ports.problem().contains("30600"),
        "the problem names the duplicated port",
    );
    report.check(
        ports.allocate_slot("redis", "redis.7.2.5", "redis") == -1,
        "allocation refuses while frozen",
    );
    report.check(
        !ports.assign_canonical("redis", "redis.7.2.5", "redis"),
        "canonical assignment refuses while frozen",
    );
    report.check(!ports.release("a"), "release refuses while frozen");
    report.check(!ports.save(), "save refuses while frozen");
}

// ---------------------------------------------------------------- registry

fn redis_instance(version: &str, role_default: bool) -> ServiceInstance {
    ServiceInstance {
        id: ServiceInstance::make_id("redis", version),
        kind: "redis".into(),
        version: version.into(),
        is_default: role_default,
        port_key: ServiceInstance::make_port_key("redis", version),
        install_path: format!("Extensions/redis{version}"),
        data_dir: ServiceInstance::make_data_dir("redis", version),
    }
}

fn registry_defaults(report: &mut Report, paths: &Paths) {
    report.section("services.json: one default per kind");
    let file = paths.data_dir().join("services.json");
    let _ = std::fs::remove_file(&file);

    let mut registry = InstanceRegistry::under(paths.root());
    report.check(
        registry.load() && registry.is_healthy(),
        "a missing services.json is normal, not an error",
    );
    report.check(registry.all().is_empty(), "nothing is installed yet");

    let first = redis_instance("7.2.5", false);
    let second = redis_instance("3.0.504", false);
    report.check(registry.add(first.clone()), "add() registers the first version");
    let stored = registry
        .find("redis-7.2.5")
        .cloned()
        .unwrap_or_else(|| redis_instance("0.0.0", false));
    report.check(stored.is_default, "first of a kind becomes the default");
    report.check(
        stored.port_key == "redis.7.2.5",
        "the first version gets a port key",
    );
    report.check(
        stored.data_dir == "data/instances/redis/7.2.5",
        "the data directory is per instance",
    );

    report.check(registry.add(second.clone()), "a second version coexists");
    report.check(registry.count_of_kind("redis") == 2, "both are listed");
    report.check(
        !registry.is_default("redis-3.0.504"),
        "installing a second version does not hijack the default",
    );
    report.check(
        registry.default_id("redis") == Some("redis-7.2.5"),
        "the first version still owns the kind",
    );
    report.check(
        !registry.add(first.clone()),
        "registering the same version twice is refused",
    );
    let mut unknown = redis_instance("1.0.0", false);
    unknown.kind = "apache".into();
    report.check(
        !registry.add(unknown),
        "a kind without a table entry is refused",
    );

    report.check(
        registry.promote_default("redis-3.0.504"),
        "promoteDefault() flips the role",
    );
    report.check(
        registry.is_default("redis-3.0.504") && !registry.is_default("redis-7.2.5"),
        "exactly one instance is default afterwards",
    );

    let mut reloaded = InstanceRegistry::under(paths.root());
    report.check(reloaded.load() && reloaded.is_healthy(), "services.json reloads");
    report.check(reloaded.count_of_kind("redis") == 2, "both instances survived the reload");
    report.check(
        reloaded.default_id("redis") == Some("redis-3.0.504"),
        "the promoted default survived the reload",
    );
    report.check(
        reloaded.remove("redis-3.0.504"),
        "removing the default is allowed",
    );
    report.check(
        reloaded.default_id("redis") == Some("redis-7.2.5"),
        "removing the default promotes the oldest remaining instance",
    );
    report.check(reloaded.remove("redis-7.2.5"), "removing the last instance is allowed");
    report.check(
        reloaded.default_id("redis").is_none(),
        "a kind with no instances names no default",
    );
}

fn frozen_registry(report: &mut Report, paths: &Paths) {
    report.section("services.json: readable but frozen");
    let file = paths.data_dir().join("services.json");
    write(
        &file,
        r#"{
  "schema": 1,
  "kinds": {
    "redis": {
      "default": "redis-9.9.9",
      "instances": [
        { "id": "redis-7.2.5", "version": "7.2.5", "role": "secondary",
          "portKey": "redis.7.2.5", "installPath": "Extensions/redis7.2.5",
          "dataDir": "data/instances/redis/7.2.5" }
      ]
    }
  }
}"#,
    );
    let mut registry = InstanceRegistry::under(paths.root());
    report.check(
        registry.load() && registry.is_loaded() && !registry.is_healthy(),
        "a default pointing at a missing instance freezes the file",
    );
    report.check(
        !registry.add(redis_instance("3.0.504", false)),
        "writes refuse until a human fixes the file",
    );
    report.check(
        !registry.promote_default("redis-7.2.5"),
        "promotion refuses while frozen",
    );
    report.check(
        registry.problem().contains("redis-9.9.9"),
        "the problem names the missing default",
    );
    report.check(
        registry.default_of("redis").is_none(),
        "a frozen file reports no usable default",
    );

    // A stale per-instance role marker is healable: the `default` field is authoritative.
    write(
        &file,
        r#"{
  "schema": 1,
  "kinds": {
    "redis": {
      "default": "redis-7.2.5",
      "instances": [
        { "id": "redis-7.2.5", "version": "7.2.5", "role": "secondary",
          "portKey": "redis.7.2.5", "installPath": "Extensions/redis7.2.5",
          "dataDir": "data/instances/redis/7.2.5" },
        { "id": "redis-3.0.504", "version": "3.0.504", "role": "default",
          "portKey": "redis.3.0.504", "installPath": "Extensions/redis3.0.504",
          "dataDir": "data/instances/redis/3.0.504" }
      ]
    }
  }
}"#,
    );
    let mut healed = InstanceRegistry::under(paths.root());
    report.check(healed.load() && healed.is_healthy(), "a stale role marker is healable");
    report.check(healed.is_default("redis-7.2.5"), "the declared default wins");
    report.check(
        !healed.is_default("redis-3.0.504"),
        "the stale marker is rewritten on load",
    );
}

// ---------------------------------------------------------------- engine reconcile

fn engine_reconcile(report: &mut Report, paths: &Paths) {
    report.section("Engine::synchronize() reconciles the two files");
    let ports_file = paths.data_dir().join("ports.json");
    let services_file = paths.data_dir().join("services.json");
    write(&ports_file, BAND_FIXTURE);
    write(
        &services_file,
        r#"{
  "schema": 1,
  "kinds": {
    "redis": {
      "default": "redis-7.2.5",
      "instances": [
        { "id": "redis-7.2.5", "version": "7.2.5", "role": "default",
          "portKey": "redis.7.2.5", "installPath": "Extensions/redis7.2.5",
          "dataDir": "data/instances/redis/7.2.5" },
        { "id": "redis-3.0.504", "version": "3.0.504", "role": "secondary",
          "portKey": "redis.3.0.504", "installPath": "Extensions/redis3.0.504",
          "dataDir": "data/instances/redis/3.0.504" }
      ]
    }
  }
}"#,
    );

    let mut state = EngineState::new(paths.clone());
    report.check(
        state.synchronize().is_ok(),
        "synchronize() reconciles services.json against ports.json",
    );
    report.check(state.is_ready(), "the engine reports itself usable");
    report.check(
        state.ports.role_of("redis.7.2.5") == "default",
        "the default holds the canonical role",
    );
    report.check(
        state.ports.port_of("redis.7.2.5") == TEST_CANONICAL,
        "the default sits on the canonical port",
    );
    report.check(
        state.ports.port_of("redis.3.0.504") == 30600,
        "the secondary sits on the first slot port",
    );

    let mut second = EngineState::new(paths.clone());
    report.check(second.synchronize().is_ok(), "synchronize() is idempotent");
    report.check(
        second.ports.port_of("redis.7.2.5") == TEST_CANONICAL
            && second.ports.port_of("redis.3.0.504") == 30600,
        "a second reconcile does not reshuffle ports",
    );
    report.check(
        second.registry.default_of("redis").is_some(),
        "the kind still resolves to a default instance",
    );

    // A frozen ports.json must stop the engine rather than let it run half-working.
    write(
        &ports_file,
        r#"{
  "schema": 2,
  "band": [30000, 32000],
  "bindAddress": "127.0.0.1",
  "canonical": { "redis": 31789 },
  "slots": { "redis": [30600, 30619] },
  "panel": {},
  "allocations": {
    "a": { "port": 30600, "owner": "redis", "role": "secondary" },
    "b": { "port": 30600, "owner": "redis", "role": "secondary" }
  }
}"#,
    );
    let mut broken = EngineState::new(paths.clone());
    let result = broken.synchronize();
    report.check(result.is_err(), "a frozen ports.json stops the engine");
    report.check(!broken.is_ready(), "the engine stays unusable rather than half-working");
    report.check(
        result.unwrap_err().contains("ports.json"),
        "the reason names ports.json",
    );
}

// ---------------------------------------------------------------- managed block

fn managed_block_checks(report: &mut Report, paths: &Paths) {
    report.section("managed block: never silently overwrite the user");
    let file = paths.data_dir().join("block-test.conf");
    let _ = std::fs::remove_file(&file);

    report.check(
        managed_block::apply(&file, "port 3306\n").is_ok(),
        "a missing file is created with markers",
    );
    let text = std::fs::read_to_string(&file).unwrap_or_default();
    report.check(text.contains(managed_block::BEGIN_MARKER), "the begin marker is present");
    report.check(text.contains("port 3306"), "the rendered body is present");

    // CRLF is preserved and user content outside the block survives.
    write(
        &file,
        &format!(
            "# user header\r\n{} \r\nport 3306\r\n{}\r\n# user tail\r\n",
            managed_block::BEGIN_MARKER,
            managed_block::END_MARKER
        ),
    );
    report.check(
        managed_block::apply(&file, "port 3307\n").is_ok(),
        "an existing block is replaced",
    );
    let text = std::fs::read_to_string(&file).unwrap_or_default();
    report.check(text.contains("port 3307"), "the new value landed");
    report.check(!text.contains("port 3306"), "the old value is gone");
    report.check(text.contains("# user header") && text.contains("# user tail"), "user lines survive");
    report.check(text.contains("\r\n"), "CRLF line endings are preserved");

    write(&file, "# hand written\nport 1234\n");
    report.check(
        managed_block::apply(&file, "port 3307\n").is_ok(),
        "a file without markers is not an error",
    );
    let text = std::fs::read_to_string(&file).unwrap_or_default();
    report.check(
        text == "# hand written\nport 1234\n",
        "a hand-written file is left byte-for-byte alone",
    );
}

// ---------------------------------------------------------------- sites

fn site_checks(report: &mut Report, paths: &Paths) {
    report.section("sites: validation and placeholder page");
    let items: Vec<Site> = Vec::new();
    let mut site = Site {
        domain: "WWW.Example.com".into(),
        port: 80,
        root_dir: paths.www_dir().join("example.com").display().to_string(),
        ..Default::default()
    };
    report.check(sites::looks_like_host(&sites::normalize(&site.domain)), "a hostname is accepted");
    report.check(!sites::looks_like_host("http://example.com"), "a scheme is rejected");
    report.check(!sites::looks_like_host("example.com:8080"), "a port in the domain is rejected");
    report.check(
        sites::validate(&site, None, &items).is_ok(),
        "a fresh site validates",
    );
    report.check(sites::prepare_root(&site).is_ok(), "the root directory is created");
    report.check(
        Path::new(&site.root_dir).join("index.html").exists(),
        "a placeholder page is written",
    );
    std::fs::write(
        Path::new(&site.root_dir).join("index.html"),
        "user content",
    )
    .ok();
    report.check(sites::prepare_root(&site).is_ok(), "preparing again is fine");
    report.check(
        std::fs::read_to_string(Path::new(&site.root_dir).join("index.html"))
            .unwrap_or_default()
            == "user content",
        "an existing index.html is never overwritten",
    );

    site.port = 0;
    report.check(sites::validate(&site, None, &items).is_err(), "port 0 is rejected");
    site.port = 80;
    site.root_dir.clear();
    report.check(sites::validate(&site, None, &items).is_err(), "an empty root is rejected");
}

// ---------------------------------------------------------------- probe / logs

fn probe_checks(report: &mut Report) {
    report.section("PortProbe: free, occupied, and who occupies it");
    let listener = std::net::TcpListener::bind("127.0.0.1:0");
    let Ok(listener) = listener else {
        report.check(false, "fixture could not listen on an ephemeral port");
        return;
    };
    let port = listener.local_addr().map(|addr| addr.port()).unwrap_or(0);
    report.check(port > 0, "the fixture has a port");
    report.check(!probe::is_port_free(port, "127.0.0.1"), "a bound port reports not free");
    let owner = probe::identify_owner(port);
    report.check(owner.occupied, "the occupier is identified, not just \"port busy\"");
    report.check(
        owner.pid == std::process::id(),
        "the holder is this process",
    );
    report.check(!owner.process_name.is_empty(), "the holder is named by process");
    drop(listener);
    report.check(probe::is_port_free(port, "127.0.0.1"), "after close the same port is free again");
}

fn log_tail_checks(report: &mut Report, paths: &Paths) {
    report.section("log tail: encoding and line boundaries");
    let file = paths.logs_dir().join("tail-test.log");
    write(&file, "one\n��\ntwo\n");
    let tail = crate::log_tail::read_tail(&file, 2, 1024);
    report.check(tail.text == "��\ntwo", "only the requested trailing lines are returned");
    report.check(tail.encoding == "UTF-8", "valid UTF-8 is reported as UTF-8");
    report.check(
        crate::log_tail::looks_like_utf8("����".as_bytes()),
        "valid UTF-8 passes the strict check",
    );
    report.check(
        !crate::log_tail::looks_like_utf8(&[0xC0, 0x80]),
        "an overlong encoding is rejected",
    );
    report.check(
        !crate::log_tail::looks_like_utf8(&[0xED, 0xA0, 0x80]),
        "a surrogate half is rejected",
    );
}

#[cfg(test)]
mod tests {
    #[test]
    fn the_suite_passes() {
        let report = super::run();
        if !report.success() {
            for line in &report.lines {
                eprintln!("{line}");
            }
        }
        assert!(report.success(), "{} check(s) failed", report.failed);
    }
}
