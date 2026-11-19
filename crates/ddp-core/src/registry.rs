//! `data/services.json` — what is installed, and which instance of each kind is default.
//!
//! Invariants, ported from the Qt registry:
//!   * at most one `default` per kind, and the `default` field is the only authority;
//!   * a per-instance `role` marker disagreeing with it is healed on load;
//!   * removing the default promotes the oldest remaining instance of that kind;
//!   * the first version installed becomes default, so installing a second version never
//!     silently steals 3306;
//!   * a self-contradictory file stays readable but frozen for writes.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use serde_json::{Map, Value};

use crate::atomic;
use crate::instance::ServiceInstance;
use crate::kinds;
use crate::log;

const SCHEMA_VERSION: i64 = 1;
const ROLE_DEFAULT: &str = "default";
const ROLE_SECONDARY: &str = "secondary";

#[derive(Clone, Debug)]
pub struct InstanceRegistry {
    path: PathBuf,
    items: Vec<ServiceInstance>,
    defaults: BTreeMap<String, String>,
    loaded: bool,
    healthy: bool,
    problem: String,
    needs_heal: bool,
}

impl InstanceRegistry {
    pub fn new(path: impl Into<PathBuf>) -> Self {
        InstanceRegistry {
            path: path.into(),
            items: Vec::new(),
            defaults: BTreeMap::new(),
            loaded: false,
            healthy: false,
            problem: String::new(),
            needs_heal: false,
        }
    }

    pub fn under(root: &Path) -> Self {
        Self::new(root.join("data").join("services.json"))
    }

    pub fn file_path(&self) -> &Path {
        &self.path
    }
    pub fn is_loaded(&self) -> bool {
        self.loaded
    }
    pub fn is_healthy(&self) -> bool {
        self.healthy
    }
    pub fn problem(&self) -> &str {
        &self.problem
    }

    fn set_problem(&mut self, text: String) {
        self.problem = text;
        self.healthy = false;
        log::error(&format!("services: {}", self.problem));
    }

    // ------------------------------------------------------------------ load / save
    pub fn load(&mut self) -> bool {
        self.loaded = false;
        self.healthy = false;
        self.needs_heal = false;
        self.problem.clear();
        self.items.clear();
        self.defaults.clear();

        if !self.path.exists() {
            // Nothing installed is the normal state on a fresh machine.
            self.loaded = true;
            self.healthy = true;
            log::info("services: no registry yet, starting empty");
            return true;
        }

        let raw = match atomic::read_bytes(&self.path) {
            Ok(raw) => raw,
            Err(error) => {
                self.set_problem(format!("cannot read {}: {error}", self.path.display()));
                return false;
            }
        };

        match self.parse(&raw) {
            ParseResult::Fatal => return false,
            ParseResult::Frozen => {
                self.sync_flags();
                self.loaded = true;
                return true;
            }
            ParseResult::Ok => {}
        }

        self.sync_flags();
        self.loaded = true;
        self.healthy = true;
        log::info(&format!(
            "services: {} instance(s) across {} kind(s)",
            self.items.len(),
            self.kind_order().len()
        ));

        // The declared default is authoritative, so healing a stale per-instance role is
        // not a guess about intent.
        if self.needs_heal && !self.save() {
            log::warn("services: could not rewrite the file to fix role markers");
        }
        true
    }

    fn parse(&mut self, raw: &[u8]) -> ParseResult {
        let value: Value = match serde_json::from_slice(raw) {
            Ok(value) => value,
            Err(error) => {
                self.set_problem(format!("{} is not valid JSON: {error}", self.path.display()));
                return ParseResult::Fatal;
            }
        };
        let Some(root) = value.as_object() else {
            self.set_problem(format!("{} is not a json object", self.path.display()));
            return ParseResult::Fatal;
        };
        if root.get("schema").and_then(Value::as_i64) != Some(SCHEMA_VERSION) {
            self.set_problem(format!(
                "schema {} is not supported (expected {SCHEMA_VERSION})",
                root.get("schema").and_then(Value::as_i64).unwrap_or(0)
            ));
            return ParseResult::Fatal;
        }
        let Some(table) = root.get("kinds").and_then(Value::as_object) else {
            return ParseResult::Ok;
        };

        for (kind, block) in table {
            if kinds::find(kind).is_none() {
                // Written by a newer build: readable, but nothing will allocate a port.
                log::warn(&format!("services: kind \"{kind}\" has no driver table entry"));
            }
            let Some(block) = block.as_object() else {
                self.set_problem(format!("kind \"{kind}\" is not an object"));
                return ParseResult::Frozen;
            };
            let mut marked_default = 0usize;
            let mut marked_id = String::new();

            if let Some(array) = block.get("instances").and_then(Value::as_array) {
                for entry in array {
                    let entry = entry.as_object().cloned().unwrap_or_default();
                    let mut instance = ServiceInstance::new(kind.clone(), text(&entry, "version"));
                    let id = text(&entry, "id");
                    instance.id = if id.is_empty() {
                        ServiceInstance::make_id(kind, &instance.version)
                    } else {
                        id
                    };
                    instance.port_key = text(&entry, "portKey");
                    instance.install_path = text(&entry, "installPath");
                    instance.data_dir = text(&entry, "dataDir");
                    let role = text(&entry, "role");

                    if instance.version.is_empty() {
                        self.set_problem(format!("{kind} has an instance without a version"));
                        return ParseResult::Frozen;
                    }
                    if self.index_of(&instance.id).is_some() {
                        self.set_problem(format!("instance id \"{}\" appears twice", instance.id));
                        return ParseResult::Frozen;
                    }
                    if role == ROLE_DEFAULT {
                        marked_default += 1;
                        if marked_id.is_empty() {
                            marked_id = instance.id.clone();
                        }
                    }
                    self.items.push(instance);
                }
            }

            if marked_default > 1 {
                self.set_problem(format!(
                    "{kind} has {marked_default} instances marked default"
                ));
                return ParseResult::Frozen;
            }

            let declared = text(block, "default");
            if !declared.is_empty() {
                match self.index_of(&declared) {
                    Some(index) if self.items[index].kind == *kind => {
                        self.defaults.insert(kind.clone(), declared.clone());
                        if !marked_id.is_empty() && marked_id != declared {
                            self.needs_heal = true;
                        }
                    }
                    _ => {
                        self.set_problem(format!(
                            "{kind} points at default \"{declared}\" which is not installed"
                        ));
                        return ParseResult::Frozen;
                    }
                }
            } else if !marked_id.is_empty() {
                self.defaults.insert(kind.clone(), marked_id);
                self.needs_heal = true;
            }
        }
        ParseResult::Ok
    }

    fn to_json(&self) -> Vec<u8> {
        let mut kinds_table = Map::new();
        for kind in self.kind_order() {
            let mut array = Vec::new();
            for instance in &self.items {
                if instance.kind != kind {
                    continue;
                }
                let mut entry = Map::new();
                entry.insert("id".into(), Value::from(instance.id.clone()));
                entry.insert("version".into(), Value::from(instance.version.clone()));
                entry.insert(
                    "role".into(),
                    Value::from(if instance.is_default {
                        ROLE_DEFAULT
                    } else {
                        ROLE_SECONDARY
                    }),
                );
                if !instance.port_key.is_empty() {
                    entry.insert("portKey".into(), Value::from(instance.port_key.clone()));
                }
                entry.insert(
                    "installPath".into(),
                    Value::from(instance.install_path.clone()),
                );
                entry.insert("dataDir".into(), Value::from(instance.data_dir.clone()));
                array.push(Value::Object(entry));
            }
            let mut block = Map::new();
            block.insert(
                "default".into(),
                Value::from(self.defaults.get(&kind).cloned().unwrap_or_default()),
            );
            block.insert("instances".into(), Value::Array(array));
            kinds_table.insert(kind, Value::Object(block));
        }

        let mut root = Map::new();
        root.insert("schema".into(), Value::from(SCHEMA_VERSION));
        root.insert("kinds".into(), Value::Object(kinds_table));
        let mut bytes = serde_json::to_vec_pretty(&Value::Object(root)).unwrap_or_default();
        bytes.push(b'\n');
        bytes
    }

    pub fn save(&mut self) -> bool {
        if !self.loaded {
            log::error("services: refusing to save before a successful load()");
            return false;
        }
        if !self.healthy {
            log::error(&format!("services: refusing to write, {}", self.problem));
            return false;
        }
        self.sync_flags();
        self.needs_heal = false;
        atomic::write_atomic(&self.path, &self.to_json())
    }

    // ------------------------------------------------------------------ derived
    fn sync_flags(&mut self) {
        for instance in self.items.iter_mut() {
            instance.is_default = self.defaults.get(&instance.kind) == Some(&instance.id);
        }
    }

    fn index_of(&self, id: &str) -> Option<usize> {
        self.items.iter().position(|item| item.id == id)
    }

    /// First-appearance order, i.e. install order. Sorting alphabetically would make every
    /// diff of services.json jump around.
    fn kind_order(&self) -> Vec<String> {
        let mut out: Vec<String> = Vec::new();
        for instance in &self.items {
            if !out.contains(&instance.kind) {
                out.push(instance.kind.clone());
            }
        }
        out
    }

    // ------------------------------------------------------------------ queries
    pub fn all(&self) -> &[ServiceInstance] {
        &self.items
    }

    pub fn of_kind(&self, kind: &str) -> Vec<ServiceInstance> {
        self.items
            .iter()
            .filter(|item| item.kind == kind)
            .cloned()
            .collect()
    }

    pub fn find(&self, id: &str) -> Option<&ServiceInstance> {
        self.index_of(id).map(|index| &self.items[index])
    }

    pub fn default_of(&self, kind: &str) -> Option<&ServiceInstance> {
        let id = self.defaults.get(kind)?;
        self.find(id)
    }

    pub fn default_id(&self, kind: &str) -> Option<&str> {
        self.defaults.get(kind).map(String::as_str)
    }

    pub fn is_default(&self, id: &str) -> bool {
        self.find(id).map(|item| item.is_default).unwrap_or(false)
    }

    pub fn count_of_kind(&self, kind: &str) -> usize {
        self.items.iter().filter(|item| item.kind == kind).count()
    }

    pub fn kinds_present(&self) -> Vec<String> {
        self.kind_order()
    }

    // ------------------------------------------------------------------ mutations
    pub fn add(&mut self, mut instance: ServiceInstance) -> bool {
        if !self.healthy {
            return false;
        }
        let Some(meta) = kinds::find(&instance.kind) else {
            log::error(&format!(
                "services: refusing to register a {}, no driver for it",
                instance.kind
            ));
            return false;
        };
        if instance.version.is_empty() {
            log::error(&format!(
                "services: cannot register a {} without a version",
                instance.kind
            ));
            return false;
        }
        if instance.id.is_empty() {
            instance.id = ServiceInstance::make_id(&instance.kind, &instance.version);
        }
        if self.index_of(&instance.id).is_some() {
            log::warn(&format!("services: {} is already registered", instance.id));
            return false;
        }
        if instance.data_dir.is_empty() {
            instance.data_dir = ServiceInstance::make_data_dir(&instance.kind, &instance.version);
        }
        if instance.port_key.is_empty() && !meta.slot.is_empty() {
            instance.port_key = ServiceInstance::make_port_key(&instance.kind, &instance.version);
        }

        let first_of_kind = self.count_of_kind(&instance.kind) == 0;
        let snapshot = self.items.clone();
        let previous_default = self.defaults.get(&instance.kind).cloned();
        let id = instance.id.clone();
        let kind = instance.kind.clone();

        self.items.push(instance);
        if first_of_kind {
            // The first version installed is what sites talk to. Later versions have to be
            // promoted explicitly.
            self.defaults.insert(kind.clone(), id.clone());
            log::info(&format!("services: {id} is the default of {kind}"));
        }
        self.sync_flags();
        if !self.save() {
            self.items = snapshot;
            match previous_default {
                Some(previous) => self.defaults.insert(kind, previous),
                None => self.defaults.remove(&kind),
            };
            self.sync_flags();
            return false;
        }
        true
    }

    pub fn remove(&mut self, id: &str) -> bool {
        if !self.healthy {
            return false;
        }
        let Some(index) = self.index_of(id) else {
            return true;
        };
        let kind = self.items[index].kind.clone();
        let snapshot = self.items.clone();
        let previous_default = self.defaults.get(&kind).cloned();

        self.items.remove(index);
        let was_default = previous_default.as_deref() == Some(id);
        if !was_default {
            self.sync_flags();
            return self.save();
        }

        // A kind with versions left must still name a default, or nothing would hold the
        // canonical port and every site connection would fail with no explanation.
        let remaining = self.of_kind(&kind);
        match remaining.first() {
            Some(first) => {
                self.defaults.insert(kind.clone(), first.id.clone());
                log::warn(&format!(
                    "services: {id} removed, default of {kind} moved to {}",
                    first.id
                ));
            }
            None => {
                self.defaults.remove(&kind);
                log::warn(&format!("services: {id} removed, {kind} has no instances left"));
            }
        }
        if !self.save() {
            self.items = snapshot;
            if let Some(previous) = previous_default {
                self.defaults.insert(kind, previous);
            }
            self.sync_flags();
            return false;
        }
        true
    }

    pub fn promote_default(&mut self, id: &str) -> bool {
        if !self.healthy {
            return false;
        }
        let Some(index) = self.index_of(id) else {
            log::error(&format!("services: cannot promote unknown instance {id}"));
            return false;
        };
        let kind = self.items[index].kind.clone();
        let previous_default = self.defaults.get(&kind).cloned();
        if previous_default.as_deref() == Some(id) {
            return true;
        }

        self.defaults.insert(kind.clone(), id.to_string());
        self.sync_flags();
        if !self.save() {
            // Inserting "" would leave the kind naming an empty default, which the next
            // load() reads as "declared default is not installed" and freezes the file.
            match previous_default.clone() {
                Some(previous) => self.defaults.insert(kind, previous),
                None => self.defaults.remove(&kind),
            };
            self.sync_flags();
            return false;
        }
        log::info(&format!(
            "services: default of {kind} is now {id} (was {})",
            previous_default.unwrap_or_else(|| "nobody".into())
        ));
        true
    }

    /// Absolute install directory of an instance.
    pub fn install_dir(&self, root: &Path, id: &str) -> Option<PathBuf> {
        let instance = self.find(id)?;
        let path = Path::new(&instance.install_path);
        Some(if path.is_absolute() {
            path.to_path_buf()
        } else {
            root.join(path)
        })
    }

    /// Absolute data directory of an instance.
    pub fn data_dir(&self, root: &Path, id: &str) -> Option<PathBuf> {
        let instance = self.find(id)?;
        let path = Path::new(&instance.data_dir);
        Some(if path.is_absolute() {
            path.to_path_buf()
        } else {
            root.join(path)
        })
    }
}

enum ParseResult {
    Ok,
    Frozen,
    Fatal,
}

fn text(map: &Map<String, Value>, key: &str) -> String {
    map.get(key)
        .and_then(Value::as_str)
        .unwrap_or("")
        .to_string()
}
