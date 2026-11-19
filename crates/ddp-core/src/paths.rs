use std::path::{Path, PathBuf};

/// Green, portable install layout: `<root>/bin`, `<root>/data`, `<root>/logs`,
/// `<root>/Extensions`, `<root>/WWW`.
///
/// `DEVDATAPOD_ROOT` overrides the discovered root for development runs and tests.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Paths {
    root: PathBuf,
}

impl Paths {
    pub fn at(root: impl Into<PathBuf>) -> Self {
        Paths {
            root: root.into(),
        }
    }

    pub fn discover() -> Self {
        if let Ok(override_root) = std::env::var("DEVDATAPOD_ROOT") {
            if !override_root.is_empty() && Path::new(&override_root).exists() {
                return Paths::at(override_root);
            }
        }
        let exe = std::env::current_exe().unwrap_or_else(|_| PathBuf::from("."));
        let mut dir = exe.parent().unwrap_or(Path::new(".")).to_path_buf();
        if dir
            .file_name()
            .map(|name| name.to_string_lossy().eq_ignore_ascii_case("bin"))
            .unwrap_or(false)
        {
            if let Some(parent) = dir.parent() {
                dir = parent.to_path_buf();
            }
        }
        Paths::at(dir)
    }

    pub fn root(&self) -> &Path {
        &self.root
    }

    pub fn bin_dir(&self) -> PathBuf {
        self.root.join("bin")
    }
    pub fn data_dir(&self) -> PathBuf {
        self.root.join("data")
    }
    pub fn logs_dir(&self) -> PathBuf {
        self.root.join("logs")
    }
    pub fn extensions_dir(&self) -> PathBuf {
        self.root.join("Extensions")
    }
    pub fn www_dir(&self) -> PathBuf {
        self.root.join("WWW")
    }
    pub fn downloads_dir(&self) -> PathBuf {
        self.data_dir().join("downloads")
    }

    /// Resolve a path stored relative to the install root. Absolute paths are returned as
    /// given, so a user-entered absolute site root keeps working.
    pub fn resolve(&self, stored: &str) -> PathBuf {
        let path = Path::new(stored);
        if path.is_absolute() {
            path.to_path_buf()
        } else {
            self.root.join(path)
        }
    }

    pub fn ensure_writable_dirs(&self) -> bool {
        let data = std::fs::create_dir_all(self.data_dir()).is_ok();
        let logs = std::fs::create_dir_all(self.logs_dir()).is_ok();
        data && logs
    }
}
