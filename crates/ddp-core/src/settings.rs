//! `data/settings.ini`, next to the executable — never the registry, never %APPDATA%.
//!
//! Only the keys devdatapod itself reads/writes are modelled; anything else found in the
//! file is preserved verbatim on save, which keeps a hand-edited file from being flattened.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use crate::atomic;

pub const AUTO_RESTART_ON_CONFIG: &str = "ui/autoRestartOnConfig";
pub const CLOSE_TO_TRAY: &str = "ui/closeToTray";
pub const WINDOW_BOUNDS: &str = "window/bounds";

#[derive(Clone, Debug, Default)]
pub struct SettingsStore {
    path: PathBuf,
    sections: BTreeMap<String, BTreeMap<String, String>>,
}

impl SettingsStore {
    pub fn new(path: impl Into<PathBuf>) -> Self {
        SettingsStore {
            path: path.into(),
            sections: BTreeMap::new(),
        }
    }

    /// `data/settings.ini` under the given root.
    pub fn under(root: &Path) -> Self {
        Self::new(root.join("data").join("settings.ini"))
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    pub fn load(&mut self) {
        self.sections.clear();
        let Ok(raw) = std::fs::read_to_string(&self.path) else {
            return;
        };
        let mut section = String::new();
        for line in raw.lines() {
            let line = line.trim();
            if line.is_empty() || line.starts_with(';') || line.starts_with('#') {
                continue;
            }
            if line.starts_with('[') && line.ends_with(']') {
                section = line[1..line.len() - 1].trim().to_string();
                self.sections.entry(section.clone()).or_default();
                continue;
            }
            if let Some((key, value)) = line.split_once('=') {
                self.sections
                    .entry(section.clone())
                    .or_default()
                    .insert(key.trim().to_string(), value.trim().to_string());
            }
        }
    }

    pub fn value(&self, key: &str) -> Option<&str> {
        let (section, name) = split_key(key);
        self.sections
            .get(section)
            .and_then(|entries| entries.get(name))
            .map(String::as_str)
    }

    pub fn bool_value(&self, key: &str, fallback: bool) -> bool {
        match self.value(key) {
            Some(value) => matches!(
                value.trim().to_ascii_lowercase().as_str(),
                "true" | "1" | "yes" | "on"
            ),
            None => fallback,
        }
    }

    pub fn string_value(&self, key: &str, fallback: &str) -> String {
        self.value(key).unwrap_or(fallback).to_string()
    }

    pub fn set(&mut self, key: &str, value: &str) {
        let (section, name) = split_key(key);
        self.sections
            .entry(section.to_string())
            .or_default()
            .insert(name.to_string(), value.to_string());
    }

    pub fn save(&self) -> bool {
        let mut out = String::new();
        for (section, entries) in &self.sections {
            out.push('[');
            out.push_str(section);
            out.push_str("]\n");
            for (key, value) in entries {
                out.push_str(key);
                out.push('=');
                out.push_str(value);
                out.push('\n');
            }
        }
        atomic::write_atomic(&self.path, out.as_bytes())
    }

    /// Load, mutate and save in one call — the pattern every settings write uses.
    pub fn update(&self, key: &str, value: &str) -> bool {
        let mut store = Self::new(self.path.clone());
        store.load();
        store.set(key, value);
        store.save()
    }
}

fn split_key(key: &str) -> (&str, &str) {
    match key.split_once('/') {
        Some((section, name)) => (section, name),
        None => ("General", key),
    }
}
