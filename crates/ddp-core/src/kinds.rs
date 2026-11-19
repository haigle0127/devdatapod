//! Static metadata about the service kinds devdatapod manages. Names only — every port
//! number still comes from `data/ports.json` (AGENTS.md rule 2).
//!
//! `mariadb` and `mysql` are two kinds, deliberately not interchangeable: their data
//! directories and handshake packets differ.

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Meta {
    /// The spelling used in services.json / ports.json.
    pub key: &'static str,
    /// What the UI shows.
    pub label: &'static str,
    /// Band name in ports.json "slots", empty = no listening port.
    pub slot: &'static str,
    /// Band name in ports.json "canonical", empty = none.
    pub canonical: &'static str,
}

const METAS: &[Meta] = &[
    Meta { key: "mysql",   label: "MySQL",   slot: "mysql",    canonical: "mysql" },
    Meta { key: "mariadb", label: "MariaDB", slot: "mariadb",  canonical: "mariadb" },
    Meta { key: "redis",   label: "Redis",   slot: "redis",    canonical: "redis" },
    Meta { key: "nginx",   label: "Nginx",   slot: "nginx",    canonical: "http" },
    Meta { key: "php",     label: "PHP",     slot: "phpFpm",   canonical: "" },
    Meta { key: "python",  label: "Python",  slot: "",         canonical: "" },
];

pub fn find(key: &str) -> Option<&'static Meta> {
    METAS.iter().find(|meta| meta.key == key)
}

pub fn all() -> &'static [Meta] {
    METAS
}

pub fn label(key: &str) -> String {
    find(key).map(|meta| meta.label.to_string()).unwrap_or_else(|| key.to_string())
}

pub fn owns_port(key: &str) -> bool {
    find(key).map(|meta| !meta.slot.is_empty()).unwrap_or(false)
}
