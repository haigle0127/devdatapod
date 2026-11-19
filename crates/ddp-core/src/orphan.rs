//! Startup sweep for processes left behind by a panel that died before its job object could
//! be closed (AGENTS.md rule 5).
//!
//! The job object covers the normal crash path on Windows. This covers the rest: a machine
//! that was reset while services ran, a supervisor that could not create a job, or a second
//! install pointing at the same directory. Matching is strictly by install-root path prefix,
//! never by image name, so `D:\devdatapod2` is not mistaken for `D:\devdatapod`.

use std::path::{Path, PathBuf};

use crate::log;

#[derive(Clone, Debug)]
pub struct Orphan {
    pub pid: u32,
    pub executable: PathBuf,
    pub name: String,
}

/// Both sides of the comparison go through the same normalization: forward slashes, the
/// `\\?\` prefix `QueryFullProcessImageNameW` sometimes returns, folded case on Windows.
fn normalize(path: &Path) -> String {
    let mut text = path.to_string_lossy().replace('\\', "/");
    if let Some(stripped) = text.strip_prefix("//?/") {
        text = stripped.to_string();
    }
    #[cfg(windows)]
    {
        text = text.to_lowercase();
    }
    text
}

fn normalized_root(root: &Path) -> String {
    let mut text = normalize(root);
    if !text.ends_with('/') {
        text.push('/');
    }
    text
}

fn under_root(normalized_root: &str, candidate: &Path) -> bool {
    let text = normalize(candidate);
    // `D:/devdatapod/` does not prefix `D:/devdatapod2/`, which is exactly why the trailing
    // separator is kept.
    text.len() > normalized_root.len() && text.starts_with(normalized_root)
}

/// Every live process whose image lives under `root`, excluding the panel itself.
pub fn scan_orphans(root: &Path) -> Vec<Orphan> {
    if root.as_os_str().is_empty() {
        return Vec::new();
    }
    let prefix = normalized_root(root);
    let own = std::process::id();

    #[cfg(windows)]
    {
        use windows_sys::Win32::Foundation::{CloseHandle, INVALID_HANDLE_VALUE};
        use windows_sys::Win32::System::Diagnostics::ToolHelp::{
            CreateToolhelp32Snapshot, Process32FirstW, Process32NextW, PROCESSENTRY32W,
            TH32CS_SNAPPROCESS,
        };
        use windows_sys::Win32::System::Threading::{
            OpenProcess, QueryFullProcessImageNameW, PROCESS_QUERY_LIMITED_INFORMATION,
        };

        let mut out = Vec::new();
        let snapshot = unsafe { CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0) };
        if snapshot == INVALID_HANDLE_VALUE {
            log::error("sweep: CreateToolhelp32Snapshot failed");
            return out;
        }
        let mut entry: PROCESSENTRY32W = unsafe { std::mem::zeroed() };
        entry.dwSize = std::mem::size_of::<PROCESSENTRY32W>() as u32;
        if unsafe { Process32FirstW(snapshot, &mut entry) } != 0 {
            loop {
                let pid = entry.th32ProcessID;
                if pid != own {
                    // QUERY_LIMITED_INFORMATION works across bitness and without
                    // SeDebugPrivilege, which is what a green app started from Explorer has.
                    let process = unsafe { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid) };
                    if !process.is_null() {
                        let mut buffer = [0u16; 4096];
                        let mut chars = buffer.len() as u32;
                        let ok = unsafe {
                            QueryFullProcessImageNameW(process, 0, buffer.as_mut_ptr(), &mut chars)
                        };
                        unsafe { CloseHandle(process) };
                        if ok != 0 {
                            let path = PathBuf::from(String::from_utf16_lossy(
                                &buffer[..chars as usize],
                            ));
                            if under_root(&prefix, &path) {
                                out.push(Orphan {
                                    pid,
                                    name: path
                                        .file_name()
                                        .map(|name| name.to_string_lossy().into_owned())
                                        .unwrap_or_default(),
                                    executable: path,
                                });
                            }
                        }
                    }
                }
                if unsafe { Process32NextW(snapshot, &mut entry) } == 0 {
                    break;
                }
            }
        }
        unsafe { CloseHandle(snapshot) };
        out
    }

    #[cfg(not(windows))]
    {
        // No portable enumerator; macOS would use proc_listallpids. Until that lands, the
        // process group is the only protection, which is why the plan lists it as a
        // follow-up.
        let _ = prefix;
        Vec::new()
    }
}

/// Terminate the given processes and return the ones that are confirmed gone.
pub fn kill_orphans(orphans: &[Orphan]) -> Vec<u32> {
    let mut gone = Vec::new();
    for orphan in orphans {
        #[cfg(windows)]
        {
            use windows_sys::Win32::Foundation::CloseHandle;
            use windows_sys::Win32::System::Threading::{
                OpenProcess, TerminateProcess, WaitForSingleObject, PROCESS_TERMINATE,
            };
            // SYNCHRONIZE is not exported as a constant by windows-sys; it is the standard
            // right that lets WaitForSingleObject see the exit.
            const SYNCHRONIZE: u32 = 0x0010_0000;
            let process = unsafe { OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, 0, orphan.pid) };
            if !process.is_null() {
                let terminated = unsafe { TerminateProcess(process, 1) } != 0;
                if terminated {
                    // Without the wait the caller's port check still sees the listener for
                    // a few milliseconds and reports a conflict that is already resolved.
                    unsafe { WaitForSingleObject(process, 3000) };
                }
                unsafe { CloseHandle(process) };
                if terminated {
                    gone.push(orphan.pid);
                    continue;
                }
            }
        }
        #[cfg(not(windows))]
        {
            let _ = orphan;
        }
        log::error(&format!(
            "sweep: could not terminate pid {} ({})",
            orphan.pid, orphan.name
        ));
    }
    gone
}

/// Scan, kill, and report. Returns how many processes were confirmed dead.
pub fn sweep_install_root(root: &Path) -> (usize, Vec<String>) {
    let orphans = scan_orphans(root);
    if orphans.is_empty() {
        return (0, Vec::new());
    }
    let gone = kill_orphans(&orphans);
    let mut report = Vec::new();
    for orphan in &orphans {
        let dead = gone.contains(&orphan.pid);
        let line = format!(
            "{} pid {} {}",
            if dead { "killed" } else { "SURVIVED" },
            orphan.pid,
            orphan.executable.display()
        );
        if dead {
            log::warn(&format!("sweep: {line}"));
        } else {
            log::error(&format!("sweep: {line}"));
        }
        report.push(line);
    }
    (gone.len(), report)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sibling_install_is_not_under_root() {
        let root = normalized_root(Path::new("D:/devdatapod"));
        assert!(under_root(&root, Path::new("D:/devdatapod/Extensions/nginx/nginx.exe")));
        assert!(!under_root(&root, Path::new("D:/devdatapod2/Extensions/nginx/nginx.exe")));
        assert!(!under_root(&root, Path::new("D:/other/nginx.exe")));
    }
}
