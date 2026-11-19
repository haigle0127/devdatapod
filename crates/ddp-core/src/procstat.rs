//! Per-service CPU / memory sampling for the home page gauges.
//!
//! CPU is a rate, so it needs two observations. The sampler keeps the previous reading and
//! returns 0% on the first call instead of inventing a number — the home-page rings tick
//! every second and the second tick is the first honest one.
//!
//! Aggregating over `Supervisor::members()` is what makes "MySQL" on the dial mean *this*
//! instance, including php-fpm/nginx workers forked after startup.

use std::time::Instant;

#[derive(Clone, Copy, Debug, Default)]
pub struct ResourceSample {
    pub valid: bool,
    /// 0..100 for the whole machine, summed over the instance.
    pub cpu_percent: f64,
    pub rss_bytes: u64,
}

#[derive(Debug, Default)]
pub struct ResourceSampler {
    baseline_ns: Option<i64>,
    baseline_at: Option<Instant>,
}

impl ResourceSampler {
    pub fn reset(&mut self) {
        self.baseline_ns = None;
        self.baseline_at = None;
    }

    pub fn sample(&mut self, pids: &[u32]) -> ResourceSample {
        let mut out = ResourceSample::default();
        if pids.is_empty() {
            self.reset();
            return out;
        }

        let mut cpu_total: i64 = 0;
        let mut rss_total: u64 = 0;
        let mut observed = 0usize;
        for pid in pids {
            let Some((cpu_ns, rss)) = process_time_ns(*pid) else {
                continue;
            };
            observed += 1;
            cpu_total += cpu_ns;
            rss_total += rss;
        }
        if observed == 0 {
            self.reset();
            return out;
        }

        out.valid = true;
        out.rss_bytes = rss_total;

        let now = Instant::now();
        match (self.baseline_ns, self.baseline_at) {
            (Some(baseline), Some(at)) if cpu_total >= baseline => {
                let wall_ns = now.duration_since(at).as_nanos() as f64;
                let cores = std::thread::available_parallelism()
                    .map(|value| value.get())
                    .unwrap_or(1)
                    .max(1) as f64;
                if wall_ns > 0.0 {
                    out.cpu_percent = (cpu_total - baseline) as f64 / wall_ns / cores * 100.0;
                }
            }
            // First tick, or the set shrank and the sum went backwards: no rate yet.
            _ => {}
        }
        self.baseline_ns = Some(cpu_total);
        self.baseline_at = Some(now);
        out
    }
}

#[cfg(windows)]
fn process_time_ns(pid: u32) -> Option<(i64, u64)> {
    use windows_sys::Win32::Foundation::{CloseHandle, FILETIME};
    use windows_sys::Win32::System::ProcessStatus::{
        K32GetProcessMemoryInfo, PROCESS_MEMORY_COUNTERS,
    };
    use windows_sys::Win32::System::Threading::{
        GetProcessTimes, OpenProcess, PROCESS_QUERY_INFORMATION, PROCESS_VM_READ,
    };

    let handle = unsafe { OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, 0, pid) };
    if handle.is_null() {
        return None;
    }
    let mut creation = FILETIME::default();
    let mut exit_time = FILETIME::default();
    let mut kernel = FILETIME::default();
    let mut user = FILETIME::default();
    let ok = unsafe { GetProcessTimes(handle, &mut creation, &mut exit_time, &mut kernel, &mut user) };
    let mut total = -1i64;
    if ok != 0 {
        let ticks = |value: &FILETIME| -> i64 {
            let combined = (u64::from(value.dwHighDateTime) << 32) | u64::from(value.dwLowDateTime);
            (combined as i64).saturating_mul(100) // 100 ns ticks
        };
        total = ticks(&kernel) + ticks(&user);
    }
    let mut rss = 0u64;
    let mut counters = PROCESS_MEMORY_COUNTERS {
        cb: std::mem::size_of::<PROCESS_MEMORY_COUNTERS>() as u32,
        ..Default::default()
    };
    if unsafe {
        K32GetProcessMemoryInfo(
            handle,
            &mut counters,
            std::mem::size_of::<PROCESS_MEMORY_COUNTERS>() as u32,
        )
    } != 0
    {
        rss = counters.WorkingSetSize as u64;
    }
    unsafe { CloseHandle(handle) };
    if total < 0 {
        None
    } else {
        Some((total, rss))
    }
}

#[cfg(not(windows))]
fn process_time_ns(_pid: u32) -> Option<(i64, u64)> {
    None
}
