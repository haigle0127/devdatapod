//! Process supervision with unconditional orphan protection (AGENTS.md rule 5).
//!
//! Windows: every child is created suspended, put inside a job object with
//! `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, and only then resumed. If the panel crashes, the OS
//! closes the job handle and takes nginx/php/redis with it — no cleanup code has to run.
//!
//! macOS: the child becomes its own process-group leader, and teardown signals the group.
//! (There is no equivalent kernel backstop, which is why the startup sweep below matters.)

use std::cell::Cell;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use crate::log;

#[derive(Debug, Clone)]
pub struct CommandResult {
    pub ran: bool,
    pub exit_code: i32,
    pub output: String,
    pub error_output: String,
}

impl Default for CommandResult {
    fn default() -> Self {
        CommandResult {
            ran: false,
            exit_code: -1,
            output: String::new(),
            error_output: String::new(),
        }
    }
}

pub struct Supervisor {
    label: String,
    valid: bool,
    problem: String,
    pid: u32,
    finished: Cell<bool>,
    exit_code: Cell<i32>,
    started_at: Cell<Option<Instant>>,
    env: Vec<(String, String)>,
    working_dir: Option<PathBuf>,
    log_path: Option<PathBuf>,
    #[cfg(windows)]
    job: Option<Handle>,
    #[cfg(windows)]
    process: Option<Handle>,
    #[cfg(not(windows))]
    child: Option<std::process::Child>,
}

impl Supervisor {
    pub fn new(label: impl Into<String>) -> Self {
        let label = label.into();
        #[cfg(windows)]
        {
            match create_job() {
                Ok(job) => Supervisor {
                    label,
                    valid: true,
                    problem: String::new(),
                    pid: 0,
                    finished: Cell::new(false),
                    exit_code: Cell::new(-1),
                    started_at: Cell::new(None),
                    env: Vec::new(),
                    working_dir: None,
                    log_path: None,
                    job: Some(job),
                    process: None,
                },
                Err(problem) => {
                    log::error(&format!("proc/{label}: container unavailable: {problem}"));
                    Supervisor {
                        label,
                        valid: false,
                        problem,
                        pid: 0,
                        finished: Cell::new(true),
                        exit_code: Cell::new(-1),
                        started_at: Cell::new(None),
                        env: Vec::new(),
                        working_dir: None,
                        log_path: None,
                        job: None,
                        process: None,
                    }
                }
            }
        }
        #[cfg(not(windows))]
        {
            Supervisor {
                label,
                valid: true,
                problem: String::new(),
                pid: 0,
                finished: Cell::new(true),
                exit_code: Cell::new(-1),
                started_at: Cell::new(None),
                env: Vec::new(),
                working_dir: None,
                log_path: None,
                child: None,
            }
        }
    }

    pub fn label(&self) -> &str {
        &self.label
    }
    pub fn is_valid(&self) -> bool {
        self.valid
    }
    pub fn problem(&self) -> &str {
        &self.problem
    }
    pub fn pid(&self) -> u32 {
        if self.is_running() {
            self.pid
        } else {
            0
        }
    }

    pub fn set_env(&mut self, name: &str, value: &str) {
        if let Some(entry) = self
            .env
            .iter_mut()
            .find(|(key, _)| key.eq_ignore_ascii_case(name))
        {
            entry.1 = value.to_string();
        } else {
            self.env.push((name.to_string(), value.to_string()));
        }
    }

    pub fn set_working_dir(&mut self, dir: impl Into<PathBuf>) {
        self.working_dir = Some(dir.into());
    }

    pub fn set_log_file(&mut self, path: impl Into<PathBuf>) {
        self.log_path = Some(path.into());
    }

    pub fn start(&mut self, program: &Path, arguments: &[String]) -> Result<(), String> {
        if !self.valid {
            return Err(format!(
                "no orphan protection for {} ({}), refusing to spawn",
                self.label, self.problem
            ));
        }
        if self.pid != 0 && self.is_running() {
            return Err(format!("{} is already running as pid {}", self.label, self.pid));
        }
        if !program.exists() {
            return Err(format!("{} does not exist", program.display()));
        }

        #[cfg(windows)]
        {
            self.start_windows(program, arguments)
        }
        #[cfg(not(windows))]
        {
            self.start_posix(program, arguments)
        }
    }

    #[cfg(windows)]
    fn start_windows(&mut self, program: &Path, arguments: &[String]) -> Result<(), String> {
        use windows_sys::Win32::Foundation::CloseHandle;
        use windows_sys::Win32::System::JobObjects::AssignProcessToJobObject;
        use windows_sys::Win32::System::Threading::{
            CreateProcessW, ResumeThread, CREATE_BREAKAWAY_FROM_JOB, CREATE_NO_WINDOW,
            CREATE_SUSPENDED, CREATE_UNICODE_ENVIRONMENT, PROCESS_INFORMATION,
            STARTF_USESHOWWINDOW, STARTF_USESTDHANDLES, STARTUPINFOW,
        };
        use windows_sys::Win32::UI::WindowsAndMessaging::SW_HIDE;

        // A restart on the same supervisor is the normal path. The old handle refers to an
        // exited process; keeping it open would leak one per switch.
        self.process = None;
        self.pid = 0;

        let absolute = std::fs::canonicalize(program).unwrap_or_else(|_| program.to_path_buf());
        let application = wide(&absolute.to_string_lossy());

        let mut line = quote_argument(program.to_string_lossy().as_ref());
        for argument in arguments {
            line.push(' ');
            line.push_str(&quote_argument(argument));
        }
        let mut command_line = wide(&line);

        let block = self.environment_block();

        // An inheritable log handle gives the child stdout+stderr without a pipe we would
        // have to drain from the UI thread.
        let log_handle = self.open_log_handle();
        let mut startup: STARTUPINFOW = unsafe { std::mem::zeroed() };
        startup.cb = std::mem::size_of::<STARTUPINFOW>() as u32;
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE as u16;
        if let Some(handle) = log_handle {
            startup.dwFlags |= STARTF_USESTDHANDLES;
            startup.hStdOutput = handle;
            startup.hStdError = handle;
        }

        let working = self
            .working_dir
            .as_ref()
            .map(|dir| wide(&dir.to_string_lossy()));

        let job = self.job.as_ref().expect("valid supervisor has a job").0;
        // CREATE_NO_WINDOW: nginx/mysqld/redis-server are console-subsystem binaries; our
        // GUI process has no console to inherit, so without this flag every start would
        // flash a blank terminal window.
        let mut flags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW;
        let mut info: PROCESS_INFORMATION = unsafe { std::mem::zeroed() };
        let mut created = unsafe {
            CreateProcessW(
                application.as_ptr(),
                command_line.as_mut_ptr(),
                std::ptr::null(),
                std::ptr::null(),
                1,
                flags,
                block.as_ptr().cast(),
                working.as_ref().map(|dir| dir.as_ptr()).unwrap_or(std::ptr::null()),
                &startup,
                &mut info,
            )
        } != 0;

        if created {
            // Assign before the thread runs. If the panel dies between these two calls the
            // OS still owns the child through the job.
            let assigned = unsafe { AssignProcessToJobObject(job, info.hProcess) } != 0;
            if !assigned {
                unsafe {
                    CloseHandle(info.hThread);
                    CloseHandle(info.hProcess);
                }
                // Reached when the panel itself runs inside a job that forbids nesting.
                flags |= CREATE_BREAKAWAY_FROM_JOB;
                info = unsafe { std::mem::zeroed() };
                created = unsafe {
                    CreateProcessW(
                        application.as_ptr(),
                        command_line.as_mut_ptr(),
                        std::ptr::null(),
                        std::ptr::null(),
                        1,
                        flags,
                        block.as_ptr().cast(),
                        working.as_ref().map(|dir| dir.as_ptr()).unwrap_or(std::ptr::null()),
                        &startup,
                        &mut info,
                    )
                } != 0;
                if created {
                    if unsafe { AssignProcessToJobObject(job, info.hProcess) } == 0 {
                        unsafe {
                            windows_sys::Win32::System::Threading::TerminateProcess(info.hProcess, 1);
                            CloseHandle(info.hThread);
                            CloseHandle(info.hProcess);
                        }
                        self.problem =
                            "cannot put the child in a job object: orphan protection is impossible"
                                .into();
                        return Err(self.problem.clone());
                    }
                } else {
                    self.problem = format!(
                        "CreateProcessW for {} failed ({})",
                        self.label,
                        unsafe { windows_sys::Win32::Foundation::GetLastError() }
                    );
                    return Err(self.problem.clone());
                }
            }

            unsafe { ResumeThread(info.hThread) };
            self.process = Some(Handle(info.hProcess));
            self.pid = info.dwProcessId;
            unsafe { CloseHandle(info.hThread) };
            self.finished.set(false);
            self.exit_code.set(-1);
            self.started_at.set(Some(Instant::now()));
            log::info(&format!("proc/{}: started pid {}", self.label, self.pid));
            return Ok(());
        }

        self.problem = format!(
            "CreateProcessW for {} failed ({})",
            self.label,
            unsafe { windows_sys::Win32::Foundation::GetLastError() }
        );
        Err(self.problem.clone())
    }

    #[cfg(windows)]
    fn open_log_handle(&self) -> Option<windows_sys::Win32::Foundation::HANDLE> {
        use windows_sys::Win32::Foundation::{GENERIC_WRITE, INVALID_HANDLE_VALUE};
        use windows_sys::Win32::Storage::FileSystem::{
            CreateFileW, SetFilePointerEx, FILE_ATTRIBUTE_NORMAL, FILE_END, FILE_SHARE_READ,
            FILE_SHARE_WRITE, OPEN_ALWAYS,
        };

        let path = self.log_path.as_ref()?;
        if let Some(parent) = path.parent() {
            let _ = std::fs::create_dir_all(parent);
        }
        let wide_path = wide(&path.to_string_lossy());
        let mut attributes: windows_sys::Win32::Security::SECURITY_ATTRIBUTES =
            unsafe { std::mem::zeroed() };
        attributes.nLength =
            std::mem::size_of::<windows_sys::Win32::Security::SECURITY_ATTRIBUTES>() as u32;
        attributes.bInheritHandle = 1;
        let handle = unsafe {
            CreateFileW(
                wide_path.as_ptr(),
                GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                &attributes,
                OPEN_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                std::ptr::null_mut(),
            )
        };
        if handle == INVALID_HANDLE_VALUE {
            log::warn(&format!(
                "proc/{}: cannot open log {}, output will be lost",
                self.label,
                path.display()
            ));
            return None;
        }
        unsafe { SetFilePointerEx(handle, 0, std::ptr::null_mut(), FILE_END) };
        Some(handle)
    }

    #[cfg(windows)]
    fn environment_block(&self) -> Vec<u16> {
        use std::collections::BTreeMap;
        let mut merged: BTreeMap<String, (String, String)> = BTreeMap::new();
        for (name, value) in std::env::vars() {
            merged.insert(name.to_ascii_uppercase(), (name, value));
        }
        for (name, value) in &self.env {
            merged.insert(name.to_ascii_uppercase(), (name.clone(), value.clone()));
        }
        // CreateProcessW documents a sorted block; an unstable order makes some child CRTs
        // treat the environment as broken.
        let mut entries: Vec<(String, String)> = merged.into_values().collect();
        entries.sort_by(|a, b| a.0.to_ascii_lowercase().cmp(&b.0.to_ascii_lowercase()));

        let mut block: Vec<u16> = Vec::new();
        for (name, value) in entries {
            block.extend(format!("{name}={value}").encode_utf16());
            block.push(0);
        }
        block.push(0);
        block
    }

    #[cfg(not(windows))]
    fn start_posix(&mut self, program: &Path, arguments: &[String]) -> Result<(), String> {
        let mut command = std::process::Command::new(program);
        command.args(arguments);
        for (name, value) in &self.env {
            command.env(name, value);
        }
        if let Some(dir) = &self.working_dir {
            command.current_dir(dir);
        }
        if let Some(log_path) = &self.log_path {
            if let Some(parent) = log_path.parent() {
                let _ = std::fs::create_dir_all(parent);
            }
            let out = std::fs::OpenOptions::new()
                .create(true)
                .append(true)
                .open(log_path)
                .map_err(|error| format!("cannot open {}: {error}", log_path.display()))?;
            let err = out
                .try_clone()
                .map_err(|error| format!("cannot duplicate the log handle: {error}"))?;
            command.stdout(out).stderr(err);
        }
        let child = command
            .spawn()
            .map_err(|error| format!("spawn {} failed: {error}", program.display()))?;
        self.pid = child.id();
        self.finished.set(false);
        self.exit_code.set(-1);
        self.started_at.set(Some(Instant::now()));
        self.child = Some(child);
        log::info(&format!("proc/{}: started pid {}", self.label, self.pid));
        Ok(())
    }

    // ------------------------------------------------------------------ observe
    pub fn is_running(&self) -> bool {
        if self.pid == 0 || self.finished.get() {
            return false;
        }
        #[cfg(windows)]
        {
            use windows_sys::Win32::System::Threading::GetExitCodeProcess;
            // STILL_ACTIVE is 259; windows-sys keeps it in a module gated on older features.
            const STILL_ACTIVE: u32 = 259;
            let Some(handle) = self.process.as_ref() else {
                self.finished.set(true);
                return false;
            };
            let mut code: u32 = 0;
            if unsafe { GetExitCodeProcess(handle.0, &mut code) } == 0 {
                self.finished.set(true);
                return false;
            }
            if code == STILL_ACTIVE as u32 {
                return true;
            }
            self.finished.set(true);
            self.exit_code.set(code as i32);
            false
        }
        #[cfg(not(windows))]
        {
            let Some(child) = self.child.as_mut() else {
                self.finished.set(true);
                return false;
            };
            match child.try_wait() {
                Ok(Some(status)) => {
                    self.finished.set(true);
                    self.exit_code.set(status.code().unwrap_or(-1));
                    false
                }
                Ok(None) => true,
                Err(_) => {
                    self.finished.set(true);
                    false
                }
            }
        }
    }

    pub fn wait_finished(&self, timeout: Duration) -> bool {
        #[cfg(windows)]
        {
            use windows_sys::Win32::Foundation::WAIT_OBJECT_0;
            let Some(handle) = self.process.as_ref() else {
                return true;
            };
            let millis = timeout.as_millis().min(u32::MAX as u128) as u32;
            let result = unsafe {
                windows_sys::Win32::System::Threading::WaitForSingleObject(handle.0, millis)
            };
            self.is_running();
            result == WAIT_OBJECT_0
        }
        #[cfg(not(windows))]
        {
            let started = Instant::now();
            while self.is_running() && started.elapsed() < timeout {
                std::thread::sleep(Duration::from_millis(20));
            }
            !self.is_running()
        }
    }

    pub fn exit_code(&self) -> i32 {
        self.is_running();
        self.exit_code.get()
    }

    pub fn uptime(&self) -> Option<Duration> {
        self.started_at.get().map(|started| started.elapsed())
    }

    /// Every process currently inside the job, so the UI can show a php-fpm pool's true size.
    pub fn members(&self) -> Vec<u32> {
        if !self.is_running() {
            return Vec::new();
        }
        #[cfg(windows)]
        {
            use windows_sys::Win32::System::JobObjects::{
                JobObjectBasicProcessIdList, QueryInformationJobObject,
                JOBOBJECT_BASIC_PROCESS_ID_LIST,
            };
            let Some(job) = self.job.as_ref() else {
                return Vec::new();
            };
            let slots = 64usize;
            let size = std::mem::size_of::<JOBOBJECT_BASIC_PROCESS_ID_LIST>()
                + slots * std::mem::size_of::<usize>();
            let mut buffer = vec![0u8; size];
            let mut needed: u32 = 0;
            let ok = unsafe {
                QueryInformationJobObject(
                    job.0,
                    JobObjectBasicProcessIdList,
                    buffer.as_mut_ptr().cast(),
                    size as u32,
                    &mut needed,
                )
            };
            if ok == 0 {
                return Vec::new();
            }
            // `ProcessIdList` is a fixed `[usize; 1]` in the windows-sys type; the runtime
            // list lives past the struct end in our buffer. Indexing the array field with
            // `count > 1` is an out-of-bounds panic (debug) / UB (release) — walk the raw
            // buffer instead.
            let base = buffer.as_ptr();
            let list = base as *const JOBOBJECT_BASIC_PROCESS_ID_LIST;
            let count = unsafe {
                (*list)
                    .NumberOfProcessIdsInList
                    .min((*list).NumberOfAssignedProcesses)
            } as usize;
            let ids = unsafe {
                base.add(std::mem::offset_of!(
                    JOBOBJECT_BASIC_PROCESS_ID_LIST,
                    ProcessIdList
                )) as *const usize
            };
            let mut out = Vec::with_capacity(count.min(slots));
            for index in 0..count.min(slots) {
                out.push(unsafe { *ids.add(index) } as u32);
            }
            out
        }
        #[cfg(not(windows))]
        {
            vec![self.pid]
        }
    }

    // ------------------------------------------------------------------ teardown
    /// Drivers send the service's own shutdown verb (redis SHUTDOWN, nginx -s quit) first;
    /// this is the fallback for the ones that have none.
    pub fn graceful(&self) -> bool {
        #[cfg(windows)]
        {
            log::info(&format!(
                "proc/{}: Windows has no group signal, use the service verb",
                self.label
            ));
            false
        }
        #[cfg(not(windows))]
        {
            // The child was started in the same group in this build; kill() is the only
            // signal std exposes, and it is the escalation path anyway.
            false
        }
    }

    pub fn kill_forced(&mut self) -> bool {
        #[cfg(windows)]
        {
            use windows_sys::Win32::Foundation::CloseHandle;
            use windows_sys::Win32::System::JobObjects::TerminateJobObject;
            let Some(job) = self.job.as_ref() else {
                return !self.is_running();
            };
            unsafe { TerminateJobObject(job.0, 0) };
            // The job may be empty; the direct handle makes sure of the leader too.
            if let Some(handle) = self.process.as_ref() {
                unsafe {
                    windows_sys::Win32::System::Threading::TerminateProcess(handle.0, 0);
                    CloseHandle(handle.0);
                }
            }
            self.process = None;
            self.wait_finished(Duration::from_secs(2));
            self.finished.set(true);
            self.pid = 0;
            true
        }
        #[cfg(not(windows))]
        {
            if let Some(child) = self.child.as_mut() {
                let _ = child.kill();
                let _ = child.wait();
            }
            self.child = None;
            self.finished.set(true);
            self.pid = 0;
            true
        }
    }
}

impl Drop for Supervisor {
    fn drop(&mut self) {
        #[cfg(windows)]
        {
            // Windows needs nothing here: closing the job handle fires KILL_ON_JOB_CLOSE.
            // Dropping the struct closes both handles through `Handle`.
            if self.is_running() {
                log::info(&format!("proc/{}: supervisor dropped while running", self.label));
            }
        }
        #[cfg(not(windows))]
        {
            if self.is_running() {
                self.kill_forced();
            }
        }
    }
}

// ---------------------------------------------------------------- platform helpers
#[cfg(windows)]
#[derive(Debug)]
struct Handle(windows_sys::Win32::Foundation::HANDLE);

#[cfg(windows)]
impl Drop for Handle {
    fn drop(&mut self) {
        if !self.0.is_null() {
            unsafe { windows_sys::Win32::Foundation::CloseHandle(self.0) };
        }
    }
}

#[cfg(windows)]
fn create_job() -> Result<Handle, String> {
    use windows_sys::Win32::System::JobObjects::{
        CreateJobObjectW, JobObjectExtendedLimitInformation, SetInformationJobObject,
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION, JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE,
    };

    let raw = unsafe { CreateJobObjectW(std::ptr::null(), std::ptr::null()) };
    if raw.is_null() {
        return Err("CreateJobObjectW failed".into());
    }
    let job = Handle(raw);
    let mut limits: JOBOBJECT_EXTENDED_LIMIT_INFORMATION = unsafe { std::mem::zeroed() };
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    let ok = unsafe {
        SetInformationJobObject(
            job.0,
            JobObjectExtendedLimitInformation,
            &limits as *const _ as *const std::ffi::c_void,
            std::mem::size_of::<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>() as u32,
        )
    };
    if ok == 0 {
        return Err("SetInformationJobObject failed".into());
    }
    Ok(job)
}

#[cfg(windows)]
fn wide(text: &str) -> Vec<u16> {
    let mut out: Vec<u16> = text.encode_utf16().collect();
    out.push(0);
    out
}

/// MSVC's `argv` quoting rules, the ones the child CRT applies when it re-splits the command
/// line. nginx and php parse their own arguments and are unforgiving about a mangled
/// `-d foo=bar`, so backslashes before a quote get doubled and the quote gets escaped.
pub fn quote_argument(raw: &str) -> String {
    if raw.is_empty() {
        return "\"\"".into();
    }
    if !raw.contains([' ', '\t', '"']) {
        return raw.to_string();
    }
    let mut out = String::with_capacity(raw.len() + 2);
    out.push('"');
    let mut backslashes = 0usize;
    for ch in raw.chars() {
        if ch == '\\' {
            backslashes += 1;
            out.push(ch);
            continue;
        }
        if ch == '"' {
            for _ in 0..=backslashes {
                out.push('\\');
            }
            out.push('"');
            backslashes = 0;
            continue;
        }
        backslashes = 0;
        out.push(ch);
    }
    for _ in 0..backslashes {
        out.push('\\');
    }
    out.push('"');
    out
}

/// Split a command line back into arguments, for tests and for echo-ing what will run.
pub fn split_arguments(line: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut current = String::new();
    let mut in_quotes = false;
    let mut backslashes = 0usize;
    for ch in line.chars() {
        if ch == '\\' {
            backslashes += 1;
            continue;
        }
        if ch == '"' {
            for _ in 0..backslashes / 2 {
                current.push('\\');
            }
            if backslashes % 2 == 0 {
                in_quotes = !in_quotes;
            } else {
                current.push('"');
            }
            backslashes = 0;
            continue;
        }
        for _ in 0..backslashes {
            current.push('\\');
        }
        backslashes = 0;
        if ch.is_whitespace() && !in_quotes {
            if !current.is_empty() {
                out.push(std::mem::take(&mut current));
            }
            continue;
        }
        current.push(ch);
    }
    for _ in 0..backslashes {
        current.push('\\');
    }
    if !current.is_empty() {
        out.push(current);
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn quoting_matches_msvc_rules() {
        assert_eq!(quote_argument("plain"), "plain");
        assert_eq!(quote_argument(""), "\"\"");
        assert_eq!(quote_argument("a b"), "\"a b\"");
        assert_eq!(quote_argument(r#"a"b"#), r#""a\"b""#);
        assert_eq!(quote_argument(r#"a\"#), r"a\");
        assert_eq!(quote_argument(r"C:\path with space\"), "\"C:\\path with space\\\\\"");
    }

    #[test]
    fn split_round_trips() {
        let cases = [
            vec!["nginx.exe".to_string(), "-p".into(), "C:\\a b\\".into()],
            vec!["-d".into(), "foo=bar".into()],
            vec!["a\"b".into()],
        ];
        for case in cases {
            let line = case
                .iter()
                .map(|item| quote_argument(item))
                .collect::<Vec<_>>()
                .join(" ");
            assert_eq!(split_arguments(&line), case, "line: {line}");
        }
    }
}
