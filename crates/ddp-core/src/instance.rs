//! The persistent half of an installed service version.
//!
//! Runtime state is not here: role is intent (`data/services.json`), state is observed (the
//! driver). "default" does not mean "running" and "running" does not mean "default".

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ServiceInstance {
    /// `"mysql-8.0.12"`, unique across kinds.
    pub id: String,
    /// `"mysql" | "mariadb" | "redis" | ...` — see `kinds`.
    pub kind: String,
    pub version: String,
    pub is_default: bool,
    /// Key in ports.json; empty when the kind owns no port.
    pub port_key: String,
    /// Relative to the install root, e.g. `Extensions/MySQL8.0.12`.
    pub install_path: String,
    /// Relative, one per instance, never shared.
    pub data_dir: String,
}

impl ServiceInstance {
    pub fn new(kind: impl Into<String>, version: impl Into<String>) -> Self {
        ServiceInstance {
            id: String::new(),
            kind: kind.into(),
            version: version.into(),
            is_default: false,
            port_key: String::new(),
            install_path: String::new(),
            data_dir: String::new(),
        }
    }

    pub fn has_port(&self) -> bool {
        !self.port_key.is_empty()
    }

    pub fn make_id(kind: &str, version: &str) -> String {
        format!("{kind}-{version}")
    }

    pub fn make_port_key(kind: &str, version: &str) -> String {
        format!("{kind}.{version}")
    }

    /// `data/instances/<kind>/<version>/` — MySQL 8 initialises a directory 5.7 cannot open,
    /// so sharing one between instances is data loss waiting to happen.
    pub fn make_data_dir(kind: &str, version: &str) -> String {
        format!("data/instances/{kind}/{version}")
    }

    /// `"MySQL 8.0.12"`.
    pub fn display_name(&self) -> String {
        format!("{} {}", crate::kinds::label(&self.kind), self.version)
    }

    /// `"默认"` / `"备用"`.
    pub fn role_label(&self) -> &'static str {
        if self.is_default {
            "默认"
        } else {
            "备用"
        }
    }
}

/// Observed runtime state. Deliberately separate from the persistent role.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RunState {
    Stopped,
    Starting,
    Running,
    Switching,
    /// Died on its own, not because we stopped it.
    Crashed,
    /// Installed but nothing to run yet.
    Idle,
}

impl RunState {
    /// The spelling the UI styles on.
    pub fn key(self) -> &'static str {
        match self {
            RunState::Stopped => "stopped",
            RunState::Starting => "starting",
            RunState::Running => "running",
            RunState::Switching => "switching",
            RunState::Crashed => "crashed",
            RunState::Idle => "idle",
        }
    }
}
