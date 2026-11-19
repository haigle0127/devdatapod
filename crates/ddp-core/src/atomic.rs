//! Write-then-replace for every json the panel owns.

use std::fs::{self, File};
use std::io::Write;
use std::path::Path;

use crate::log;

pub fn read_bytes(path: &Path) -> Result<Vec<u8>, String> {
    fs::read(path).map_err(|error| format!("cannot read {}: {error}", path.display()))
}

pub fn read_text(path: &Path) -> Result<String, String> {
    let raw = read_bytes(path)?;
    Ok(String::from_utf8_lossy(&raw).into_owned())
}

/// A crash mid-write must not leave a half-written file that the next launch reads as
/// "two instances claim 3306".
pub fn write_atomic(path: &Path, data: &[u8]) -> bool {
    if let Some(parent) = path.parent() {
        let _ = fs::create_dir_all(parent);
    }
    let mut temp_os = path.as_os_str().to_os_string();
    temp_os.push(".tmp");
    let temp = std::path::PathBuf::from(temp_os);
    {
        let mut file = match File::create(&temp) {
            Ok(file) => file,
            Err(error) => {
                log::error(&format!("cannot write {}: {error}", temp.display()));
                return false;
            }
        };
        if let Err(error) = file.write_all(data) {
            log::error(&format!("short write to {}: {error}", temp.display()));
            let _ = fs::remove_file(&temp);
            return false;
        }
        if let Err(error) = file.flush() {
            log::error(&format!("cannot flush {}: {error}", temp.display()));
            let _ = fs::remove_file(&temp);
            return false;
        }
    }

    // Windows rename refuses to replace an existing file. The gap is sub-millisecond and a
    // crash there still leaves a complete .tmp rather than a truncated live file.
    if path.exists() && fs::remove_file(path).is_err() {
        log::error(&format!("cannot replace {}", path.display()));
        let _ = fs::remove_file(&temp);
        return false;
    }
    if let Err(error) = fs::rename(&temp, path) {
        log::error(&format!(
            "cannot move {} over {}: {error}",
            temp.display(),
            path.display()
        ));
        return false;
    }
    true
}
