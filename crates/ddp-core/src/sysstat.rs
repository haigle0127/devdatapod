//! Machine-wide counters for the home page: CPU, memory, uptime, CPU topology.
//! Read-only, core-level, no UI dependency (AGENTS.md rule 3).

#[derive(Clone, Copy, Debug, Default)]
pub struct Snapshot {
    pub valid: bool,
    /// 0..1, system-wide CPU usage.
    pub cpu_fraction: f64,
    pub total_phys_bytes: u64,
    pub avail_phys_bytes: u64,
}

impl Snapshot {
    pub fn memory_fraction(&self) -> f64 {
        if self.total_phys_bytes == 0 {
            0.0
        } else {
            (self.total_phys_bytes - self.avail_phys_bytes) as f64 / self.total_phys_bytes as f64
        }
    }
}

#[derive(Clone, Debug, Default)]
pub struct CpuTopology {
    pub valid: bool,
    pub physical_cores: usize,
    pub logical_processors: usize,
    pub performance_cores: usize,
    pub efficient_cores: usize,
    pub model: String,
}

impl CpuTopology {
    pub fn hybrid(&self) -> bool {
        self.efficient_cores > 0
    }
}

/// CPU usage is a delta between two points; the first `sample()` only establishes the
/// baseline and reports `valid = true` with `cpu_fraction = 0`.
#[derive(Debug, Default)]
pub struct Sampler {
    has_baseline: bool,
    #[cfg(windows)]
    previous: (u64, u64, u64),
}

impl Sampler {
    pub fn has_baseline(&self) -> bool {
        self.has_baseline
    }

    pub fn sample(&mut self) -> Snapshot {
        #[cfg(windows)]
        {
            use windows_sys::Win32::System::SystemInformation::{GlobalMemoryStatusEx, MEMORYSTATUSEX};
            use windows_sys::Win32::System::Threading::GetSystemTimes;

            let mut idle = 0u64;
            let mut kernel = 0u64;
            let mut user = 0u64;
            let mut out = Snapshot::default();
            let ok = unsafe {
                GetSystemTimes(
                    (&mut idle as *mut u64).cast(),
                    (&mut kernel as *mut u64).cast(),
                    (&mut user as *mut u64).cast(),
                )
            };
            if ok == 0 {
                return out;
            }
            // GetSystemTimes reports kernel time inclusive of idle.
            let busy = kernel.saturating_sub(idle).saturating_add(user);
            let total = kernel.saturating_add(user);
            if self.has_baseline {
                let busy_delta = busy.saturating_sub(self.previous.0);
                let total_delta = total.saturating_sub(self.previous.1);
                if total_delta > 0 {
                    out.cpu_fraction = busy_delta as f64 / total_delta as f64;
                }
            }
            self.previous = (busy, total, self.previous.2);
            self.has_baseline = true;

            let mut memory: MEMORYSTATUSEX = unsafe { std::mem::zeroed() };
            memory.dwLength = std::mem::size_of::<MEMORYSTATUSEX>() as u32;
            if unsafe { GlobalMemoryStatusEx(&mut memory) } != 0 {
                out.total_phys_bytes = memory.ullTotalPhys;
                out.avail_phys_bytes = memory.ullAvailPhys;
                out.valid = true;
            }
            out
        }
        #[cfg(not(windows))]
        {
            self.has_baseline = true;
            Snapshot::default()
        }
    }
}

#[cfg(windows)]
pub fn cpu_topology() -> CpuTopology {
    use windows_sys::Win32::System::SystemInformation::{
        GetLogicalProcessorInformation, RelationProcessorCore, SYSTEM_LOGICAL_PROCESSOR_INFORMATION,
    };

    let mut out = CpuTopology {
        model: std::env::var("PROCESSOR_IDENTIFIER").unwrap_or_default(),
        ..Default::default()
    };

    let mut size: u32 = 0;
    unsafe { GetLogicalProcessorInformation(std::ptr::null_mut(), &mut size) };
    if size == 0 {
        return out;
    }
    let entries = size as usize / std::mem::size_of::<SYSTEM_LOGICAL_PROCESSOR_INFORMATION>();
    let mut buffer: Vec<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> = Vec::with_capacity(entries);
    if unsafe { GetLogicalProcessorInformation(buffer.as_mut_ptr(), &mut size) } == 0 {
        return out;
    }
    let count = size as usize / std::mem::size_of::<SYSTEM_LOGICAL_PROCESSOR_INFORMATION>();
    let slice = unsafe { std::slice::from_raw_parts(buffer.as_ptr(), count) };
    let mut logical = 0usize;
    for entry in slice {
        if entry.Relationship == RelationProcessorCore {
            out.physical_cores += 1;
            logical += entry.ProcessorMask.count_ones() as usize;
        }
    }
    out.logical_processors = logical.max(out.physical_cores);
    out.performance_cores = out.physical_cores;
    out.valid = out.physical_cores > 0;
    out
}

#[cfg(not(windows))]
pub fn cpu_topology() -> CpuTopology {
    CpuTopology {
        model: std::env::var("PROCESSOR_IDENTIFIER").unwrap_or_default(),
        ..Default::default()
    }
}

/// Milliseconds since boot, or `None` where the platform has no cheap answer.
pub fn uptime_ms() -> Option<u64> {
    #[cfg(windows)]
    {
        Some(unsafe { windows_sys::Win32::System::SystemInformation::GetTickCount64() })
    }
    #[cfg(not(windows))]
    {
        None
    }
}
