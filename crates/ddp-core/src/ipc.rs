//! One panel per install directory.
//!
//! The lock file decides ownership; a loopback TCP listener on `panel.ipc` (from
//! `data/ports.json`, never a literal) lets a second launch ask the running panel to raise
//! its window instead of opening a second copy.
//!
//! Deliberately not a named pipe or a Qt local server: the v1.1 plan puts the engine behind
//! an IPC boundary anyway, and a loopback socket is the same primitive there. It also gives
//! a reliable liveness test — if `connect` fails, the lock file is stale.

use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::net::{SocketAddr, TcpListener, TcpStream};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{channel, Receiver};
use std::sync::Arc;
use std::time::Duration;

use crate::log;
use crate::paths::Paths;

const PING_TIMEOUT: Duration = Duration::from_millis(300);
const ACCEPT_POLL: Duration = Duration::from_millis(100);

pub struct Guard {
    /// 0 when the activation channel was unavailable; the lock still makes us the owner.
    port: u16,
    activate: Receiver<()>,
    lock_path: PathBuf,
    _lock: Option<File>,
    stop: Arc<AtomicBool>,
    listener: Option<TcpListener>,
}

impl Guard {
    pub fn port(&self) -> u16 {
        self.port
    }

    pub fn has_channel(&self) -> bool {
        self.listener.is_some()
    }

    /// True when another launch asked us to come to the front. Draining is non-blocking, so
    /// it can be polled from a UI tick.
    pub fn take_activation(&self) -> bool {
        let mut asked = false;
        while self.activate.try_recv().is_ok() {
            asked = true;
        }
        asked
    }
}

impl Drop for Guard {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        // Give the accept thread a moment to notice and release the port before the lock
        // file disappears, so a relaunch does not ping a half-closed socket.
        std::thread::sleep(ACCEPT_POLL);
        self.listener = None;
        let _ = fs::remove_file(&self.lock_path);
        if let Ok(file) = OpenOptions::new().write(true).open(&self.lock_path) {
            let _ = file.set_len(0);
        }
    }
}

/// `Err(())` means another instance owns this install directory and has been asked to show
/// itself; the caller should exit quietly.
pub fn acquire(paths: &Paths, ipc_port: u16) -> Result<Guard, ()> {
    let lock_path = paths.data_dir().join("devdatapod.lock");

    // The socket is the authority. A failed bind plus a successful connect is the normal
    // "already running" path; a failed bind plus a failed connect means something foreign
    // holds the port, so the lock file gets the final say.
    let mut listener: Option<TcpListener> = None;
    let mut port = 0u16;
    if ipc_port > 0 {
        match TcpListener::bind(("127.0.0.1", ipc_port)) {
            Ok(bound) => {
                let _ = bound.set_nonblocking(true);
                listener = Some(bound);
                port = ipc_port;
            }
            Err(_) => {
                if ping(ipc_port) {
                    return Err(());
                }
                log::warn(&format!(
                    "ipc: 127.0.0.1:{ipc_port} is held by another process; window activation unavailable"
                ));
            }
        }
    }

    if let Some(owner) = read_lock_owner(&lock_path) {
        if owner != std::process::id() && process_alive(owner) {
            // No channel to ping, but the owner is genuinely running.
            return Err(());
        }
    }

    let lock = claim_lock(&lock_path);

    let (tx, activate) = channel::<()>();
    let stop = Arc::new(AtomicBool::new(false));
    if let Some(bound) = listener.as_ref().and_then(|listener| listener.try_clone().ok()) {
        let stop_flag = stop.clone();
        std::thread::Builder::new()
            .name("ddp-ipc".into())
            .spawn(move || {
                while !stop_flag.load(Ordering::Relaxed) {
                    match bound.accept() {
                        Ok((mut stream, _)) => {
                            let _ = stream.set_read_timeout(Some(PING_TIMEOUT));
                            let mut buffer = [0u8; 32];
                            let _ = stream.read(&mut buffer);
                            let _ = tx.send(());
                        }
                        Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => {
                            std::thread::sleep(ACCEPT_POLL);
                        }
                        Err(_) => break,
                    }
                }
            })
            .ok();
    }

    Ok(Guard {
        port,
        activate,
        lock_path,
        _lock: lock,
        stop,
        listener,
    })
}

/// Ask the running panel to show itself. `true` means someone answered.
pub fn request_activation(port: u16) -> bool {
    port > 0 && ping(port)
}

fn ping(port: u16) -> bool {
    let address = SocketAddr::from(([127, 0, 0, 1], port));
    let Ok(mut stream) = TcpStream::connect_timeout(&address, PING_TIMEOUT) else {
        return false;
    };
    let _ = stream.set_write_timeout(Some(PING_TIMEOUT));
    stream.write_all(b"activate").is_ok()
}

fn claim_lock(lock_path: &Path) -> Option<File> {
    if let Some(parent) = lock_path.parent() {
        let _ = fs::create_dir_all(parent);
    }
    let file = OpenOptions::new()
        .create(true)
        .write(true)
        .truncate(true)
        .open(lock_path)
        .ok();
    if let Some(handle) = file.as_ref() {
        let mut handle = handle;
        let _ = handle.write_all(std::process::id().to_string().as_bytes());
        let _ = handle.flush();
    }
    file
}

fn read_lock_owner(lock_path: &Path) -> Option<u32> {
    let text = fs::read_to_string(lock_path).ok()?;
    text.trim().parse().ok()
}

#[cfg(windows)]
fn process_alive(pid: u32) -> bool {
    use windows_sys::Win32::Foundation::CloseHandle;
    use windows_sys::Win32::System::Threading::{
        GetExitCodeProcess, OpenProcess, PROCESS_QUERY_LIMITED_INFORMATION,
    };

    // windows-sys 0.61 does not export the STILL_ACTIVE constant.
    const STILL_ACTIVE: u32 = 259;
    if pid == 0 {
        return false;
    }
    let handle = unsafe { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid) };
    if handle.is_null() {
        return false;
    }
    let mut code: u32 = 0;
    let ok = unsafe { GetExitCodeProcess(handle, &mut code) };
    unsafe { CloseHandle(handle) };
    ok != 0 && code == STILL_ACTIVE
}

#[cfg(not(windows))]
fn process_alive(pid: u32) -> bool {
    if pid == 0 {
        return false;
    }
    Path::new(&format!("/proc/{pid}")).exists()
}

/// Paths used by the panel's own Adminer/api servers, resolved from ports.json for the UI.
pub fn panel_port(paths: &Paths, name: &str) -> i32 {
    let mut ports = crate::ports::PortAllocator::under(paths.root());
    if !ports.load() {
        return -1;
    }
    ports.panel_port(name)
}
