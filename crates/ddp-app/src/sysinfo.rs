//! Static machine facts for the 资源信息 page. Everything here is read once per render of
//! the page and is cheap; the 3-minute counters live in the poll loop instead.

use std::sync::OnceLock;

use ddp_core::paths::Paths;
use ddp_core::sysstat::CpuTopology;

static TOPOLOGY: OnceLock<CpuTopology> = OnceLock::new();

pub fn topology() -> &'static CpuTopology {
    TOPOLOGY.get_or_init(ddp_core::sysstat::cpu_topology)
}

pub fn os_name() -> String {
    if let Ok(name) = std::env::var("OS") {
        if !name.is_empty() {
            return name;
        }
    }
    std::env::consts::OS.to_string()
}

pub fn host_name() -> String {
    std::env::var("COMPUTERNAME")
        .or_else(|_| std::env::var("HOSTNAME"))
        .unwrap_or_else(|_| "未知主机".into())
}

pub fn cpu_arch() -> String {
    std::env::var("PROCESSOR_ARCHITECTURE").unwrap_or_else(|_| std::env::consts::ARCH.into())
}

/// First non-loopback IPv4 on an up-and-running interface. A UDP "connect" to a public
/// address routes without sending any packet, so this works offline.
pub fn lan_ip() -> String {
    let socket = match std::net::UdpSocket::bind("0.0.0.0:0") {
        Ok(socket) => socket,
        Err(_) => return "--".into(),
    };
    if socket.connect("8.8.8.8:80").is_err() {
        return "--".into();
    }
    match socket.local_addr() {
        Ok(address) if !address.ip().is_loopback() => address.ip().to_string(),
        _ => "--".into(),
    }
}

/// Total size of the flat download cache, one decimal MB like the Qt build.
pub fn downloads_cache_text(paths: &Paths) -> String {
    let mut total = 0u64;
    if let Ok(entries) = std::fs::read_dir(paths.downloads_dir()) {
        for entry in entries.flatten() {
            if let Ok(meta) = entry.metadata() {
                if meta.is_file() {
                    total += meta.len();
                }
            }
        }
    }
    format!("{:.1} MB", total as f64 / 1048576.0)
}
