//! Port occupancy, listener ownership and "did it really come up" probes.
//!
//! Two questions are deliberately separate:
//!   * `is_port_free` — may I bind here? (used before allocating a fallback slot)
//!   * `wait_for_listen` / handshakes — is the process I started actually serving? A process
//!     that is alive but never listens is the classic "started, then silently broken" state.

use std::io::{Read, Write};
use std::net::{IpAddr, Shutdown, SocketAddr, TcpListener, TcpStream};
use std::time::{Duration, Instant};

#[derive(Clone, Debug, Default)]
pub struct PortOccupier {
    pub port: u16,
    pub occupied: bool,
    pub pid: u32,
    pub address: String,
    pub executable: String,
    pub process_name: String,
}

/// True when a listener can be bound on `address:port` right now.
pub fn is_port_free(port: u16, address: &str) -> bool {
    let ip: IpAddr = address.parse().unwrap_or(IpAddr::from([127, 0, 0, 1]));
    match TcpListener::bind(SocketAddr::new(ip, port)) {
        Ok(listener) => {
            drop(listener);
            true
        }
        Err(_) => false,
    }
}

/// True when something already accepts connections there.
pub fn is_listening(port: u16, address: &str) -> bool {
    let ip: IpAddr = address.parse().unwrap_or(IpAddr::from([127, 0, 0, 1]));
    TcpStream::connect_timeout(&SocketAddr::new(ip, port), Duration::from_millis(200)).is_ok()
}

/// Poll until the port answers or the deadline passes. Returns the time it took.
pub fn wait_for_listen(port: u16, address: &str, timeout: Duration) -> Option<Duration> {
    let started = Instant::now();
    while started.elapsed() < timeout {
        if is_listening(port, address) {
            return Some(started.elapsed());
        }
        std::thread::sleep(Duration::from_millis(120));
    }
    None
}

/// A minimal Redis RESP `PING`, so "listening" is not mistaken for "redis is healthy".
pub fn redis_ping(port: u16, address: &str, timeout: Duration) -> bool {
    let ip: IpAddr = address.parse().unwrap_or(IpAddr::from([127, 0, 0, 1]));
    let Ok(mut stream) = TcpStream::connect_timeout(&SocketAddr::new(ip, port), timeout) else {
        return false;
    };
    let _ = stream.set_read_timeout(Some(timeout));
    let _ = stream.set_write_timeout(Some(timeout));
    if stream.write_all(b"PING\r\n").is_err() {
        return false;
    }
    let mut buffer = [0u8; 64];
    match stream.read(&mut buffer) {
        Ok(read) => read > 0 && buffer[..read].starts_with(b"+PONG"),
        Err(_) => false,
    }
}

/// MySQL's initial handshake packet starts with a 3-byte length and protocol version 10.
/// Reading it proves `mysqld` finished `--initialize` and is accepting clients, which a
/// bare TCP connect does not.
pub fn mysql_handshake(port: u16, address: &str, timeout: Duration) -> bool {
    let ip: IpAddr = address.parse().unwrap_or(IpAddr::from([127, 0, 0, 1]));
    let Ok(mut stream) = TcpStream::connect_timeout(&SocketAddr::new(ip, port), timeout) else {
        return false;
    };
    let _ = stream.set_read_timeout(Some(timeout));
    let mut buffer = [0u8; 16];
    let ok = match stream.read(&mut buffer) {
        Ok(read) if read >= 5 => {
            let length =
                u32::from(buffer[0]) | (u32::from(buffer[1]) << 8) | (u32::from(buffer[2]) << 16);
            length > 0 && buffer[4] == 10
        }
        _ => false,
    };
    let _ = stream.shutdown(Shutdown::Both);
    ok
}

/// `GET /` against a local nginx vhost. Any HTTP status line counts as "serving".
pub fn http_reachable(port: u16, address: &str, host_header: &str, timeout: Duration) -> bool {
    let ip: IpAddr = address.parse().unwrap_or(IpAddr::from([127, 0, 0, 1]));
    let Ok(mut stream) = TcpStream::connect_timeout(&SocketAddr::new(ip, port), timeout) else {
        return false;
    };
    let _ = stream.set_read_timeout(Some(timeout));
    let _ = stream.set_write_timeout(Some(timeout));
    let request = format!(
        "GET / HTTP/1.1\r\nHost: {host_header}\r\nConnection: close\r\nUser-Agent: devdatapod\r\n\r\n"
    );
    if stream.write_all(request.as_bytes()).is_err() {
        return false;
    }
    let mut buffer = [0u8; 32];
    match stream.read(&mut buffer) {
        Ok(read) => read > 0 && buffer[..read].starts_with(b"HTTP/"),
        Err(_) => false,
    }
}

/// Who holds the listening socket, so the UI can say "port 3306 is taken by mysqld.exe
#[cfg(windows)]
pub fn identify_owner(port: u16) -> PortOccupier {
    use windows_sys::Win32::Foundation::{CloseHandle, ERROR_INSUFFICIENT_BUFFER, NO_ERROR};
    use windows_sys::Win32::NetworkManagement::IpHelper::{
        GetExtendedTcpTable, MIB_TCPROW_OWNER_PID, MIB_TCPTABLE_OWNER_PID,
        TCP_TABLE_OWNER_PID_LISTENER,
    };
    use windows_sys::Win32::Networking::WinSock::AF_INET;
    use windows_sys::Win32::System::Threading::{
        OpenProcess, QueryFullProcessImageNameW, PROCESS_QUERY_LIMITED_INFORMATION,
    };

    let mut result = PortOccupier {
        port,
        ..Default::default()
    };
    let mut size: u32 = 0;
    let probe = unsafe {
        GetExtendedTcpTable(
            std::ptr::null_mut(),
            &mut size,
            0,
            AF_INET as u32,
            TCP_TABLE_OWNER_PID_LISTENER,
            0,
        )
    };
    if probe != ERROR_INSUFFICIENT_BUFFER || size == 0 {
        return result;
    }
    let mut table = vec![0u8; size as usize];
    let status = unsafe {
        GetExtendedTcpTable(
            table.as_mut_ptr().cast(),
            &mut size,
            0,
            AF_INET as u32,
            TCP_TABLE_OWNER_PID_LISTENER,
            0,
        )
    };
    if status != NO_ERROR {
        return result;
    }
    let rows = table.as_ptr() as *const MIB_TCPTABLE_OWNER_PID;
    let count = unsafe { (*rows).dwNumEntries } as usize;
    // `table` in windows-sys is declared as a one-element array, so indexing it cannot be
    // used to walk the variable-length table the API actually returned.
    let first = unsafe { std::ptr::addr_of!((*rows).table) } as *const MIB_TCPROW_OWNER_PID;
    for index in 0..count {
        let row: MIB_TCPROW_OWNER_PID = unsafe { *first.add(index) };
        let local_port = u16::from_be(row.dwLocalPort as u16);
        if local_port != port {
            continue;
        }
        result.occupied = true;
        result.pid = row.dwOwningPid;
        let address = u32::from_be(row.dwLocalAddr);
        result.address = IpAddr::from(address.to_be_bytes()).to_string();
        let handle = unsafe { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, row.dwOwningPid) };
        if !handle.is_null() {
            let mut path = [0u16; 1024];
            let mut chars = path.len() as u32;
            let ok = unsafe { QueryFullProcessImageNameW(handle, 0, path.as_mut_ptr(), &mut chars) };
            if ok != 0 {
                let executable = String::from_utf16_lossy(&path[..chars as usize]);
                result.process_name = std::path::Path::new(&executable)
                    .file_name()
                    .map(|name| name.to_string_lossy().into_owned())
                    .unwrap_or_default();
                result.executable = executable;
            }
            unsafe { CloseHandle(handle) };
        }
        break;
    }
    result
}

#[cfg(not(windows))]
pub fn identify_owner(port: u16) -> PortOccupier {
    PortOccupier {
        port,
        occupied: is_listening(port, "127.0.0.1"),
        ..Default::default()
    }
}

/// Listeners owned by *another* devdatapod install. Used by the startup sweep so a crash of
/// panel A is cleaned up by panel B instead of leaving nginx bound to 80 forever.
#[cfg(windows)]
pub fn sibling_panel_listeners(own_root: &std::path::Path) -> Vec<PortOccupier> {
    use windows_sys::Win32::Foundation::{
        CloseHandle, ERROR_INSUFFICIENT_BUFFER, NO_ERROR,
    };
    use windows_sys::Win32::NetworkManagement::IpHelper::{
        GetExtendedTcpTable, MIB_TCPROW_OWNER_PID, MIB_TCPTABLE_OWNER_PID,
        TCP_TABLE_OWNER_PID_LISTENER,
    };
    use windows_sys::Win32::Networking::WinSock::AF_INET;
    use windows_sys::Win32::System::Threading::{
        OpenProcess, QueryFullProcessImageNameW, PROCESS_QUERY_LIMITED_INFORMATION,
    };

    let own = own_root
        .to_string_lossy()
        .replace('\\', "/")
        .to_lowercase()
        + "/";
    let mut out = Vec::new();
    let mut size: u32 = 0;
    let probe = unsafe {
        GetExtendedTcpTable(
            std::ptr::null_mut(),
            &mut size,
            0,
            AF_INET as u32,
            TCP_TABLE_OWNER_PID_LISTENER,
            0,
        )
    };
    if probe != ERROR_INSUFFICIENT_BUFFER || size == 0 {
        return out;
    }
    let mut table = vec![0u8; size as usize];
    if unsafe {
        GetExtendedTcpTable(
            table.as_mut_ptr().cast(),
            &mut size,
            0,
            AF_INET as u32,
            TCP_TABLE_OWNER_PID_LISTENER,
            0,
        )
    } != NO_ERROR
    {
        return out;
    }
    let rows = table.as_ptr() as *const MIB_TCPTABLE_OWNER_PID;
    let count = unsafe { (*rows).dwNumEntries } as usize;
    let first = unsafe { std::ptr::addr_of!((*rows).table) } as *const MIB_TCPROW_OWNER_PID;
    for index in 0..count {
        let row: MIB_TCPROW_OWNER_PID = unsafe { *first.add(index) };
        let handle = unsafe { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, row.dwOwningPid) };
        if handle.is_null() {
            continue;
        }
        let mut path = [0u16; 1024];
        let mut chars = path.len() as u32;
        let ok = unsafe { QueryFullProcessImageNameW(handle, 0, path.as_mut_ptr(), &mut chars) };
        unsafe { CloseHandle(handle) };
        if ok == 0 {
            continue;
        }
        let executable = String::from_utf16_lossy(&path[..chars as usize]).replace('\\', "/");
        let lower = executable.to_lowercase();
        if !lower.contains("/devdatapod/") || lower.starts_with(&own) {
            continue;
        }
        out.push(PortOccupier {
            port: u16::from_be(row.dwLocalPort as u16),
            occupied: true,
            pid: row.dwOwningPid,
            address: IpAddr::from(u32::from_be(row.dwLocalAddr).to_be_bytes()).to_string(),
            process_name: std::path::Path::new(&executable)
                .file_name()
                .map(|name| name.to_string_lossy().into_owned())
                .unwrap_or_default(),
            executable,
        });
    }
    out
}

#[cfg(not(windows))]
pub fn sibling_panel_listeners(_own_root: &std::path::Path) -> Vec<PortOccupier> {
    Vec::new()
}
