//! Streaming download of component archives.
//!
//! The destination is written as `<name>.part` and renamed only after a clean finish, so an
//! interrupted download can never be mistaken for an installable archive.

use std::fs::File;
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::Duration;

use crate::log;

#[derive(Clone, Debug, Default)]
pub struct Progress {
    pub received: u64,
    pub total: u64,
}

impl Progress {
    pub fn fraction(&self) -> f64 {
        if self.total == 0 {
            0.0
        } else {
            (self.received as f64 / self.total as f64).clamp(0.0, 1.0)
        }
    }
}

/// Download `url` to `dest`. `on_progress` is called roughly every 64 KiB; returning `false`
/// from it cancels the transfer.
pub fn download(
    url: &str,
    dest: &Path,
    mut on_progress: impl FnMut(&Progress),
) -> Result<PathBuf, String> {
    if let Some(parent) = dest.parent() {
        std::fs::create_dir_all(parent)
            .map_err(|error| format!("无法创建 {}：{error}", parent.display()))?;
    }
    let part = part_path(dest);
    let _ = std::fs::remove_file(&part);

    let response = ureq::AgentBuilder::new()
        .timeout_connect(Duration::from_secs(20))
        .build()
        .get(url)
        .call()
        .map_err(|error| format!("下载失败：{error}"))?;

    let total = response
        .header("Content-Length")
        .and_then(|value| value.parse::<u64>().ok())
        .unwrap_or(0);

    let mut file = File::create(&part).map_err(|error| format!("无法写入 {}：{error}", part.display()))?;
    let mut reader = response.into_reader();
    let mut buffer = vec![0u8; 64 * 1024];
    let mut received = 0u64;
    let mut cancelled = false;
    loop {
        let read = match reader.read(&mut buffer) {
            Ok(0) => break,
            Ok(read) => read,
            Err(error) => {
                let _ = std::fs::remove_file(&part);
                return Err(format!("下载中断：{error}"));
            }
        };
        if let Err(error) = file.write_all(&buffer[..read]) {
            let _ = std::fs::remove_file(&part);
            return Err(format!("无法写入 {}：{error}", part.display()));
        }
        received += read as u64;
        let progress = Progress { received, total };
        on_progress(&progress);
        if cancelled_signal() {
            cancelled = true;
            break;
        }
    }
    let _ = file.flush();
    drop(file);

    if cancelled {
        let _ = std::fs::remove_file(&part);
        return Err("已取消".into());
    }

    let _ = std::fs::remove_file(dest);
    std::fs::rename(&part, dest).map_err(|error| {
        let _ = std::fs::remove_file(&part);
        format!("无法保存到 {}：{error}", dest.display())
    })?;
    log::info(&format!("download: {url} -> {}", dest.display()));
    Ok(dest.to_path_buf())
}

/// Cooperative cancellation without threading a token through every caller: the install
/// worker checks the flag between chunks.
static CANCEL: AtomicBool = AtomicBool::new(false);

pub fn cancel_flag() -> Arc<AtomicBool> {
    Arc::new(AtomicBool::new(CANCEL.load(Ordering::SeqCst)))
}

pub fn request_cancel() {
    CANCEL.store(true, Ordering::SeqCst);
}

pub fn clear_cancel() {
    CANCEL.store(false, Ordering::SeqCst);
}

fn cancelled_signal() -> bool {
    CANCEL.load(Ordering::SeqCst)
}

pub fn part_path(dest: &Path) -> PathBuf {
    let mut os = dest.as_os_str().to_os_string();
    os.push(".part");
    PathBuf::from(os)
}
