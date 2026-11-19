//! The only place that turns a service key into a TCP port. Every number comes out of
//! `data/ports.json` — nothing in the code carries a port literal (AGENTS.md rule 2).
//!
//! Two kinds of keys live in the same map:
//!   `"mysql.8.0.12"`  instance ports, role = default (canonical) or secondary (slot)
//!   `"panel.api"`     ports devdatapod itself owns, fixed, never migrated
//!
//! Once a slot port is handed out it never moves. Restart must not reshuffle it or every
//! rendered vhost pointing at it goes stale (docs/PORT-PLAN.md rule 1).

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use chrono::{DateTime, SecondsFormat, Utc};
use serde_json::{Map, Value};

use crate::atomic;
use crate::log;

const SCHEMA_VERSION: i64 = 2;
const SEED: &str = include_str!("../../../config/ports.default.json");
const ROLE_DEFAULT: &str = "default";
const ROLE_SECONDARY: &str = "secondary";

#[derive(Clone, Debug)]
pub struct Allocation {
    pub key: String,
    pub port: u16,
    pub owner: String,
    pub role: String,
    pub bound_at: Option<DateTime<Utc>>,
}

#[derive(Clone, Debug)]
pub struct Slot {
    pub name: String,
    pub first: u16,
    pub last: u16,
    pub stride: u16,
}

#[derive(Clone, Debug)]
pub struct PortAllocator {
    path: PathBuf,
    bind: String,
    band_first: u16,
    band_last: u16,
    canonical: BTreeMap<String, u16>,
    slots: BTreeMap<String, Slot>,
    ranges: BTreeMap<String, Slot>,
    panel: BTreeMap<String, i32>,
    items: Vec<Allocation>,
    loaded: bool,
    healthy: bool,
    problem: String,
}

enum ParseOutcome {
    Ok,
    Frozen,
    Fatal,
}

impl PortAllocator {
    pub fn new(path: impl Into<PathBuf>) -> Self {
        PortAllocator {
            path: path.into(),
            bind: "127.0.0.1".into(),
            band_first: 0,
            band_last: 0,
            canonical: BTreeMap::new(),
            slots: BTreeMap::new(),
            ranges: BTreeMap::new(),
            panel: BTreeMap::new(),
            items: Vec::new(),
            loaded: false,
            healthy: false,
            problem: String::new(),
        }
    }

    pub fn under(root: &Path) -> Self {
        Self::new(root.join("data").join("ports.json"))
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
    pub fn bind_address(&self) -> &str {
        &self.bind
    }
    pub fn band_first(&self) -> u16 {
        self.band_first
    }
    pub fn band_last(&self) -> u16 {
        self.band_last
    }

    fn set_problem(&mut self, text: String) {
        self.problem = text;
        self.healthy = false;
        log::error(&format!("ports: {}", self.problem));
    }

    // ------------------------------------------------------------------ load / save
    pub fn load(&mut self) -> bool {
        self.loaded = false;
        self.healthy = false;
        self.problem.clear();
        self.items.clear();
        self.canonical.clear();
        self.slots.clear();
        self.ranges.clear();
        self.panel.clear();

        let raw = if !self.path.exists() {
            let bytes = SEED.as_bytes().to_vec();
            if !atomic::write_atomic(&self.path, &bytes) {
                return false;
            }
            log::info(&format!("ports: seeded {}", self.path.display()));
            bytes
        } else {
            match atomic::read_bytes(&self.path) {
                Ok(bytes) => bytes,
                Err(error) => {
                    self.set_problem(format!("cannot read {}: {error}", self.path.display()));
                    return false;
                }
            }
        };

        // "Readable but frozen" is a successful load: reads keep working, writes refuse.
        self.healthy = true;
        match self.parse(&raw) {
            ParseOutcome::Fatal => return false,
            ParseOutcome::Ok | ParseOutcome::Frozen => {}
        }
        self.loaded = true;
        if !self.healthy {
            log::warn(&format!(
                "ports: {} is readable but frozen: {}",
                self.path.display(),
                self.problem
            ));
        } else {
            log::info(&format!(
                "ports: {} allocations, band {}-{}",
                self.items.len(),
                self.band_first,
                self.band_last
            ));
        }
        true
    }

    fn parse(&mut self, raw: &[u8]) -> ParseOutcome {
        let value: Value = match serde_json::from_slice(raw) {
            Ok(value) => value,
            Err(error) => {
                self.set_problem(format!("{} is not valid JSON: {error}", self.path.display()));
                return ParseOutcome::Fatal;
            }
        };
        let Some(root) = value.as_object() else {
            self.set_problem(format!("{} is not a json object", self.path.display()));
            return ParseOutcome::Fatal;
        };
        if root.get("schema").and_then(Value::as_i64) != Some(SCHEMA_VERSION) {
            self.set_problem(format!(
                "schema {} is not supported (expected {})",
                root.get("schema").and_then(Value::as_i64).unwrap_or(0),
                SCHEMA_VERSION
            ));
            return ParseOutcome::Fatal;
        }
        let Some((first, last)) = read_pair(root.get("band")) else {
            self.set_problem("missing or malformed \"band\"".into());
            return ParseOutcome::Fatal;
        };
        self.band_first = first;
        self.band_last = last;
        self.bind = root
            .get("bindAddress")
            .and_then(Value::as_str)
            .unwrap_or("127.0.0.1")
            .to_string();

        if let Some(canonical) = root.get("canonical").and_then(Value::as_object) {
            for (key, value) in canonical {
                if let Some(port) = value.as_i64() {
                    if (1..=65535).contains(&port) {
                        self.canonical.insert(key.clone(), port as u16);
                    }
                }
            }
        }
        let strides = root.get("stride").and_then(Value::as_object);
        if let Some(table) = root.get("slots").and_then(Value::as_object) {
            for (name, value) in table {
                let Some((first, last)) = read_pair(Some(value)) else {
                    self.set_problem(format!("slot \"{name}\" is not [first, last]"));
                    return ParseOutcome::Fatal;
                };
                let stride = strides
                    .and_then(|map| map.get(name))
                    .and_then(Value::as_u64)
                    .unwrap_or(1)
                    .max(1) as u16;
                self.slots.insert(
                    name.clone(),
                    Slot {
                        name: name.clone(),
                        first,
                        last,
                        stride,
                    },
                );
            }
        }
        if let Some(table) = root.get("ranges").and_then(Value::as_object) {
            for (name, value) in table {
                let Some((first, last)) = read_pair(Some(value)) else {
                    self.set_problem(format!("range \"{name}\" is not [first, last]"));
                    return ParseOutcome::Fatal;
                };
                self.ranges.insert(
                    name.clone(),
                    Slot {
                        name: name.clone(),
                        first,
                        last,
                        stride: 1,
                    },
                );
            }
        }
        if let Some(table) = root.get("panel").and_then(Value::as_object) {
            for (name, value) in table {
                if let Some(port) = value.as_i64() {
                    self.panel.insert(name.clone(), port as i32);
                }
            }
        }

        if let Some(table) = root.get("allocations").and_then(Value::as_object) {
            for (key, value) in table {
                let entry = value.as_object().cloned().unwrap_or_default();
                let port = entry.get("port").and_then(Value::as_i64).unwrap_or(0);
                let owner = entry
                    .get("owner")
                    .and_then(Value::as_str)
                    .unwrap_or("")
                    .to_string();
                if port <= 0 || owner.is_empty() {
                    self.set_problem(format!("allocation \"{key}\" is incomplete"));
                    return ParseOutcome::Fatal;
                }
                let bound_at = entry
                    .get("boundAt")
                    .and_then(Value::as_str)
                    .and_then(|text| DateTime::parse_from_rfc3339(text).ok())
                    .map(|time| time.with_timezone(&Utc));
                self.items.push(Allocation {
                    key: key.clone(),
                    port: port as u16,
                    owner,
                    role: entry
                        .get("role")
                        .and_then(Value::as_str)
                        .unwrap_or("")
                        .to_string(),
                    bound_at,
                });
            }
        }
        self.items.sort_by(|a, b| a.key.cmp(&b.key));

        // Duplicate ports are the failure mode that turns into "two services fighting for
        // 3306", so the file is readable but frozen until a human resolves it.
        for i in 0..self.items.len() {
            for j in (i + 1)..self.items.len() {
                if self.items[i].port == self.items[j].port {
                    self.set_problem(format!(
                        "{} and {} both claim port {}",
                        self.items[i].key, self.items[j].key, self.items[i].port
                    ));
                    return ParseOutcome::Frozen;
                }
            }
        }
        ParseOutcome::Ok
    }

    fn to_json(&self) -> Vec<u8> {
        let mut root = Map::new();
        root.insert("schema".into(), Value::from(SCHEMA_VERSION));
        root.insert(
            "band".into(),
            Value::Array(vec![
                Value::from(self.band_first),
                Value::from(self.band_last),
            ]),
        );
        root.insert("bindAddress".into(), Value::from(self.bind.clone()));

        let mut canonical = Map::new();
        for (key, port) in &self.canonical {
            canonical.insert(key.clone(), Value::from(*port));
        }
        root.insert("canonical".into(), Value::Object(canonical));

        let mut slots = Map::new();
        let mut strides = Map::new();
        for (name, slot) in &self.slots {
            slots.insert(
                name.clone(),
                Value::Array(vec![Value::from(slot.first), Value::from(slot.last)]),
            );
            if slot.stride != 1 {
                strides.insert(name.clone(), Value::from(slot.stride));
            }
        }
        root.insert("slots".into(), Value::Object(slots));
        if !strides.is_empty() {
            root.insert("stride".into(), Value::Object(strides));
        }
        if !self.ranges.is_empty() {
            let mut ranges = Map::new();
            for (name, range) in &self.ranges {
                ranges.insert(
                    name.clone(),
                    Value::Array(vec![Value::from(range.first), Value::from(range.last)]),
                );
            }
            root.insert("ranges".into(), Value::Object(ranges));
        }
        let mut panel = Map::new();
        for (name, port) in &self.panel {
            panel.insert(name.clone(), Value::from(*port));
        }
        root.insert("panel".into(), Value::Object(panel));

        let mut allocations = Map::new();
        for item in &self.items {
            let mut entry = Map::new();
            entry.insert("port".into(), Value::from(item.port));
            entry.insert("owner".into(), Value::from(item.owner.clone()));
            if !item.role.is_empty() {
                entry.insert("role".into(), Value::from(item.role.clone()));
            }
            if let Some(bound_at) = item.bound_at {
                entry.insert(
                    "boundAt".into(),
                    Value::from(bound_at.to_rfc3339_opts(SecondsFormat::Millis, true)),
                );
            }
            allocations.insert(item.key.clone(), Value::Object(entry));
        }
        root.insert("allocations".into(), Value::Object(allocations));

        let mut bytes = serde_json::to_vec_pretty(&Value::Object(root)).unwrap_or_default();
        bytes.push(b'\n');
        bytes
    }

    pub fn save(&mut self) -> bool {
        if !self.loaded {
            log::error("ports: refusing to save before a successful load()");
            return false;
        }
        if !self.healthy {
            log::error(&format!("ports: refusing to write, {}", self.problem));
            return false;
        }
        atomic::write_atomic(&self.path, &self.to_json())
    }

    // ------------------------------------------------------------------ band table
    pub fn canonical_port(&self, band: &str) -> Option<u16> {
        self.canonical.get(band).copied()
    }

    pub fn slot_range(&self, slot: &str) -> Option<(u16, u16)> {
        self.slots.get(slot).map(|slot| (slot.first, slot.last))
    }

    pub fn slot_stride(&self, slot: &str) -> u16 {
        self.slots.get(slot).map(|slot| slot.stride).unwrap_or(1)
    }

    pub fn reserved_range(&self, name: &str) -> Option<(u16, u16)> {
        self.ranges.get(name).map(|slot| (slot.first, slot.last))
    }

    pub fn panel_port(&self, name: &str) -> i32 {
        self.panel.get(name).copied().unwrap_or(-1)
    }

    // ------------------------------------------------------------------ read state
    fn index_of(&self, key: &str) -> Option<usize> {
        self.items.iter().position(|item| item.key == key)
    }

    pub fn contains(&self, key: &str) -> bool {
        self.index_of(key).is_some()
    }

    pub fn port_of(&self, key: &str) -> u16 {
        self.index_of(key).map(|index| self.items[index].port).unwrap_or(0)
    }

    pub fn role_of(&self, key: &str) -> &str {
        self.index_of(key)
            .map(|index| self.items[index].role.as_str())
            .unwrap_or("")
    }

    pub fn allocations(&self) -> &[Allocation] {
        &self.items
    }

    pub fn allocations_of(&self, owner: &str) -> Vec<Allocation> {
        self.items
            .iter()
            .filter(|item| item.owner == owner)
            .cloned()
            .collect()
    }

    fn has_port(&self, port: u16, except: Option<&str>) -> bool {
        self.items
            .iter()
            .any(|item| Some(item.key.as_str()) != except && item.port == port)
    }

    fn free_port_in(&self, slot: &Slot, skip: u16) -> u16 {
        let mut port = slot.first;
        while port <= slot.last {
            if port != skip && !self.has_port(port, None) {
                return port;
            }
            let next = port as u32 + u32::from(slot.stride.max(1));
            if next > u16::MAX as u32 {
                break;
            }
            port = next as u16;
        }
        0
    }

    // ------------------------------------------------------------------ mutations
    pub fn allocate_slot(&mut self, slot: &str, key: &str, owner: &str) -> i32 {
        if !self.healthy {
            return -1;
        }
        let existing = self.port_of(key);
        if existing > 0 {
            return i32::from(existing); // fixed at first install, never re-drawn
        }
        let Some(band) = self.slots.get(slot).cloned() else {
            log::error(&format!(
                "ports: no slot band \"{slot}\" in {}",
                self.path.display()
            ));
            return -1;
        };
        let port = self.free_port_in(&band, 0);
        if port == 0 {
            log::error(&format!(
                "ports: band {}-{} is full",
                band.first, band.last
            ));
            return -1;
        }

        let snapshot = self.items.clone();
        self.items.push(Allocation {
            key: key.to_string(),
            port,
            owner: owner.to_string(),
            role: ROLE_SECONDARY.into(),
            bound_at: Some(Utc::now()),
        });
        self.items.sort_by(|a, b| a.key.cmp(&b.key));
        if !self.save() {
            self.items = snapshot;
            return -1;
        }
        log::info(&format!("ports: {key} -> {port} ({slot})"));
        i32::from(port)
    }

    pub fn assign_canonical(&mut self, band: &str, key: &str, owner: &str) -> bool {
        if !self.healthy {
            return false;
        }
        let Some(port) = self.canonical.get(band).copied() else {
            log::error(&format!("ports: band \"{band}\" has no canonical port"));
            return false;
        };
        if let Some(index) = self.index_of(key) {
            if self.items[index].port == port && self.items[index].role == ROLE_DEFAULT {
                return true;
            }
        }
        if self.has_port(port, None) {
            log::error(&format!(
                "ports: canonical {port} for {band} is already held"
            ));
            return false;
        }
        if self
            .items
            .iter()
            .any(|item| item.key != key && item.owner == owner && item.role == ROLE_DEFAULT)
        {
            log::error(&format!(
                "ports: an instance already owns the default of {band}"
            ));
            return false;
        }

        let snapshot = self.items.clone();
        let fresh = Allocation {
            key: key.to_string(),
            port,
            owner: owner.to_string(),
            role: ROLE_DEFAULT.into(),
            bound_at: Some(Utc::now()),
        };
        if let Some(index) = self.index_of(key) {
            self.items[index] = fresh;
        } else {
            self.items.push(fresh);
        }
        self.items.sort_by(|a, b| a.key.cmp(&b.key));
        if !self.save() {
            self.items = snapshot;
            return false;
        }
        true
    }

    pub fn release(&mut self, key: &str) -> bool {
        if !self.healthy {
            return false;
        }
        let Some(index) = self.index_of(key) else {
            return true;
        };
        let snapshot = self.items.clone();
        self.items.remove(index);
        if !self.save() {
            self.items = snapshot;
            return false;
        }
        true
    }

    /// Move an existing *secondary* allocation inside its own band after a conflict.
    /// A default never moves — the canonical port is either held by us or reported.
    pub fn retarget(&mut self, key: &str, slot: &str) -> u16 {
        if !self.healthy {
            return 0;
        }
        let Some(index) = self.index_of(key) else {
            return 0;
        };
        if self.items[index].role == ROLE_DEFAULT {
            log::error(&format!(
                "ports: {key} is the default of {}, it does not move",
                self.items[index].owner
            ));
            return 0;
        }
        let Some(band) = self.slots.get(slot).cloned() else {
            log::error(&format!("ports: {key} has no band \"{slot}\" to move inside"));
            return 0;
        };
        let old_port = self.items[index].port;
        let port = self.free_port_in(&band, old_port);
        if port == 0 {
            return 0;
        }

        let snapshot = self.items.clone();
        self.items[index].port = port;
        self.items[index].bound_at = Some(Utc::now());
        if !self.save() {
            self.items = snapshot;
            return 0;
        }
        log::warn(&format!(
            "ports: {key} moved {old_port} -> {port}, references must be re-rendered"
        ));
        port
    }

    /// Role *and* port swap between one default and one secondary of the same kind, in a
    /// single write. Both ports are free by then, so no temporary port is needed.
    pub fn swap_roles(&mut self, default_key: &str, other_key: &str) -> bool {
        if !self.healthy {
            return false;
        }
        let (Some(a), Some(b)) = (self.index_of(default_key), self.index_of(other_key)) else {
            log::error(&format!(
                "ports: swap needs two known keys, got {default_key} and {other_key}"
            ));
            return false;
        };
        if a == b {
            return false;
        }
        if self.items[a].owner != self.items[b].owner {
            log::error(&format!(
                "ports: {default_key} and {other_key} are different kinds"
            ));
            return false;
        }
        if self.items[a].role == self.items[b].role {
            log::error(&format!(
                "ports: swap wants exactly one default, both are \"{}\"",
                self.items[a].role
            ));
            return false;
        }
        let port_a = self.items[a].port;
        let port_b = self.items[b].port;
        if self.has_port(port_a, Some(default_key)) || self.has_port(port_b, Some(other_key)) {
            log::error(&format!(
                "ports: a third allocation holds {port_a} or {port_b}, refusing to swap"
            ));
            return false;
        }

        let snapshot = self.items.clone();
        let mut first = self.items[a].clone();
        let mut second = self.items[b].clone();
        std::mem::swap(&mut first.port, &mut second.port);
        std::mem::swap(&mut first.role, &mut second.role);
        first.bound_at = Some(Utc::now());
        second.bound_at = Some(Utc::now());
        self.items[a] = first;
        self.items[b] = second;
        if !self.save() {
            self.items = snapshot;
            return false;
        }
        log::info(&format!(
            "ports: default of {} is now {other_key} (swapped ports {port_a} and {port_b})",
            self.items[a].owner
        ));
        true
    }

    pub fn set_port(&mut self, key: &str, port: u16) -> bool {
        if !self.healthy {
            return false;
        }
        let Some(index) = self.index_of(key) else {
            log::error(&format!("ports: cannot set port on unknown key {key}"));
            return false;
        };
        if port == 0 {
            log::error("ports: 0 is not a valid port");
            return false;
        }
        if self.has_port(port, Some(key)) {
            log::error(&format!("ports: {port} already belongs to another key"));
            return false;
        }

        let mut in_plan = false;
        let mut is_canonical = false;
        for value in self.canonical.values() {
            if *value == port {
                in_plan = true;
                is_canonical = true;
                break;
            }
        }
        if !in_plan {
            for slot in self.slots.values() {
                if port >= slot.first && port <= slot.last {
                    in_plan = true;
                    break;
                }
            }
        }
        if !in_plan {
            log::error(&format!(
                "ports: {port} is not a planned port, refusing to set it on {key}"
            ));
            return false;
        }

        let snapshot = self.items.clone();
        self.items[index].port = port;
        self.items[index].bound_at = Some(Utc::now());
        if !is_canonical && self.items[index].role == ROLE_DEFAULT {
            self.items[index].role = ROLE_SECONDARY.into();
        }
        self.items.sort_by(|a, b| a.key.cmp(&b.key));
        if !self.save() {
            self.items = snapshot;
            return false;
        }
        log::info(&format!(
            "ports: {key} manually set to {port} (role={})",
            self.role_of(key)
        ));
        true
    }

    /// Put a known allocation back exactly where it was. Only a rollback has this need.
    pub fn rebind(&mut self, key: &str, port: u16, role: &str, owner: &str) -> bool {
        if !self.healthy {
            return false;
        }
        if port == 0 || owner.is_empty() {
            log::error(&format!(
                "ports: refusing to rebind {key} to an incomplete allocation"
            ));
            return false;
        }
        if self.has_port(port, Some(key)) {
            log::error(&format!(
                "ports: another key still holds {port}, cannot rebind {key}"
            ));
            return false;
        }
        let mut in_plan = false;
        for value in self.canonical.values() {
            if *value == port {
                in_plan = true;
                break;
            }
        }
        if !in_plan {
            for slot in self.slots.values() {
                if port >= slot.first && port <= slot.last {
                    in_plan = true;
                    break;
                }
            }
        }
        if !in_plan {
            log::error(&format!(
                "ports: {port} is not a planned port, refusing to rebind {key} to it"
            ));
            return false;
        }

        let index = self.index_of(key);
        let bound_at = index
            .and_then(|index| self.items[index].bound_at)
            .or_else(|| Some(Utc::now()));
        let snapshot = self.items.clone();
        let fresh = Allocation {
            key: key.to_string(),
            port,
            owner: owner.to_string(),
            role: role.to_string(),
            bound_at,
        };
        match index {
            Some(index) => self.items[index] = fresh,
            None => self.items.push(fresh),
        }
        self.items.sort_by(|a, b| a.key.cmp(&b.key));
        if !self.save() {
            self.items = snapshot;
            return false;
        }
        log::info(&format!("ports: {key} rebound to {port} ({role})"));
        true
    }
}

fn read_pair(value: Option<&Value>) -> Option<(u16, u16)> {
    let array = value?.as_array()?;
    if array.len() != 2 {
        return None;
    }
    let first = array[0].as_i64()?;
    let last = array[1].as_i64()?;
    if first <= 0 || last < first || last > i64::from(u16::MAX) {
        return None;
    }
    Some((first as u16, last as u16))
}
