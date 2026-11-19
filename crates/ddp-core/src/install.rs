//! Download + unpack a catalog component into `Extensions/<kind><version>/`.
//!
//! Windows 10+ ships bsdtar at `C:/Windows/System32/tar.exe`, which reads both zip and
//! tar.gz. Using its absolute path matters: a Git-for-Windows `tar` earlier on PATH does not
//! understand zip.

use std::path::{Path, PathBuf};

use crate::catalog::Component;
use crate::download;
use crate::log;
use crate::paths::Paths;
use crate::run_once::run_once;

/// Where a component lands: `Extensions/<kind><version>`.
pub fn install_dir(paths: &Paths, component: &Component) -> PathBuf {
    paths
        .extensions_dir()
        .join(format!("{}{}", component.kind, component.version))
}

/// Download (if the archive is not already cached) and unpack. Returns the install directory.
pub fn install_component(
    paths: &Paths,
    component: &Component,
    mut on_progress: impl FnMut(&download::Progress),
) -> Result<PathBuf, String> {
    let downloads = paths.downloads_dir();
    std::fs::create_dir_all(&downloads)
        .map_err(|error| format!("无法创建 {}：{error}", downloads.display()))?;
    let archive = downloads.join(&component.file_name);

    if !archive.exists() {
        download::download(&component.url, &archive, &mut on_progress)?;
    } else {
        log::info(&format!("install: reusing cached {}", archive.display()));
    }

    let dest = install_dir(paths, component);
    extract(&archive, &dest, component.server_binary())?;
    Ok(dest)
}

/// Unpack `archive` into `dest` and flatten it so `dest/<serverBinary>` exists.
pub fn extract(archive: &Path, dest: &Path, server_binary: &str) -> Result<(), String> {
    if !archive.exists() {
        return Err(format!("压缩包不存在：{}", archive.display()));
    }
    if server_binary.is_empty() {
        return Err("不知道该找哪个服务端二进制".into());
    }

    // Unpack beside the destination first, then swap in. A half-unpacked archive must never
    // look installable.
    let temp = temp_dir_for(dest);
    let _ = std::fs::remove_dir_all(&temp);
    std::fs::create_dir_all(&temp)
        .map_err(|error| format!("无法创建临时目录 {}：{error}", temp.display()))?;

    let result = run_once(
        Path::new("C:/Windows/System32/tar.exe"),
        &[
            "-xf".into(),
            archive.display().to_string(),
            "-C".into(),
            temp.display().to_string(),
        ],
        std::time::Duration::from_secs(180),
        None,
    );
    if !result.ran || result.exit_code != 0 {
        let _ = std::fs::remove_dir_all(&temp);
        let detail = if result.error_output.trim().is_empty() {
            result.output
        } else {
            result.error_output
        };
        return Err(format!("解压失败：{}", detail.trim()));
    }

    let Some(mut server_dir) = find_server_dir(&temp, server_binary) else {
        let _ = std::fs::remove_dir_all(&temp);
        return Err(format!("解压后没找到 {server_binary}"));
    };

    // MySQL's layout is `<root>/bin/mysqld.exe` plus `<root>/share/errmsg.sys`. Flattening
    // only `bin/` would lose `share/`, and mysqld refuses to initialise without the error
    // message file. Anything with the binary in `bin/` flattens its parent instead; redis and
    // nginx do not put the binary there, so they are unaffected.
    if server_dir
        .file_name()
        .map(|name| name.to_string_lossy().eq_ignore_ascii_case("bin"))
        .unwrap_or(false)
    {
        if let Some(parent) = server_dir.parent() {
            server_dir = parent.to_path_buf();
        }
    }

    let _ = std::fs::remove_dir_all(dest);
    std::fs::create_dir_all(dest)
        .map_err(|error| format!("无法创建安装目录 {}：{error}", dest.display()))?;
    if let Err(error) = move_contents(&server_dir, dest) {
        let _ = std::fs::remove_dir_all(&temp);
        return Err(error);
    }

    let _ = std::fs::remove_dir_all(&temp);
    log::info(&format!(
        "install: {} -> {}",
        archive.display(),
        dest.display()
    ));
    Ok(())
}

/// Recursively locate `server_binary` and return the directory holding it.
pub fn find_server_dir(root: &Path, server_binary: &str) -> Option<PathBuf> {
    let mut stack = vec![root.to_path_buf()];
    while let Some(dir) = stack.pop() {
        let Ok(entries) = std::fs::read_dir(&dir) else {
            continue;
        };
        for entry in entries.flatten() {
            let path = entry.path();
            if path.is_dir() {
                stack.push(path);
            } else if path
                .file_name()
                .map(|name| name.to_string_lossy().eq_ignore_ascii_case(server_binary))
                .unwrap_or(false)
            {
                return path.parent().map(Path::to_path_buf);
            }
        }
    }
    None
}

fn move_contents(source: &Path, dest: &Path) -> Result<(), String> {
    let entries = std::fs::read_dir(source)
        .map_err(|error| format!("无法读取 {}：{error}", source.display()))?;
    for entry in entries.flatten() {
        let from = entry.path();
        let to = dest.join(entry.file_name());
        if to.exists() {
            if to.is_dir() {
                let _ = std::fs::remove_dir_all(&to);
            } else {
                let _ = std::fs::remove_file(&to);
            }
        }
        if std::fs::rename(&from, &to).is_err() {
            // Rename fails across volumes; fall back to copy + delete.
            copy_recursive(&from, &to)?;
            if from.is_dir() {
                let _ = std::fs::remove_dir_all(&from);
            } else {
                let _ = std::fs::remove_file(&from);
            }
        }
    }
    Ok(())
}

fn copy_recursive(from: &Path, to: &Path) -> Result<(), String> {
    if from.is_dir() {
        std::fs::create_dir_all(to)
            .map_err(|error| format!("无法创建 {}：{error}", to.display()))?;
        for entry in std::fs::read_dir(from)
            .map_err(|error| format!("无法读取 {}：{error}", from.display()))?
            .flatten()
        {
            copy_recursive(&entry.path(), &to.join(entry.file_name()))?;
        }
        Ok(())
    } else {
        std::fs::copy(from, to)
            .map(|_| ())
            .map_err(|error| format!("无法复制 {}：{error}", to.display()))
    }
}

fn temp_dir_for(dest: &Path) -> PathBuf {
    let mut os = dest.as_os_str().to_os_string();
    os.push(".unpacking");
    PathBuf::from(os)
}

/// Remove an installed component. Only ever inside `Extensions`, so a malformed registry
/// entry cannot turn into "delete the user's home directory".
pub fn remove_install_dir(paths: &Paths, install_path: &str) -> Result<(), String> {
    let candidate = paths.resolve(install_path);
    let extensions = paths
        .extensions_dir()
        .canonicalize()
        .unwrap_or_else(|_| paths.extensions_dir());
    let candidate_abs = candidate.canonicalize().unwrap_or_else(|_| candidate.clone());
    if !candidate_abs.starts_with(&extensions) {
        return Err(format!(
            "拒绝删除 {}：不在 Extensions 目录下",
            candidate.display()
        ));
    }
    if candidate_abs == extensions {
        return Err("拒绝删除整个 Extensions 目录".into());
    }
    std::fs::remove_dir_all(&candidate_abs)
        .map_err(|error| format!("无法删除 {}：{error}", candidate_abs.display()))
}
