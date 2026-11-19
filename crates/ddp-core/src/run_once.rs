//! Run a helper command to completion and collect its output: `nginx -t`, `mysqld
//! --initialize-insecure`, `mysqld --version`. Not for long-lived services — those go
//! through `Supervisor`.

use std::io::Read;
use std::path::Path;
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

use crate::log;
use crate::log_tail::decode;
use crate::supervisor::CommandResult;

pub fn run_once(
    program: &Path,
    arguments: &[String],
    timeout: Duration,
    working_dir: Option<&Path>,
) -> CommandResult {
    let mut out = CommandResult::default();

    let mut command = Command::new(program);
    command.args(arguments);
    if let Some(dir) = working_dir {
        command.current_dir(dir);
    }
    command.stdout(Stdio::piped()).stderr(Stdio::piped());

    let mut child = match command.spawn() {
        Ok(child) => child,
        Err(error) => {
            log::warn(&format!(
                "runOnce: {} never started ({error})",
                program.display()
            ));
            return out;
        }
    };
    out.ran = true;

    let started = Instant::now();
    let mut timed_out = false;
    loop {
        match child.try_wait() {
            Ok(Some(_)) => break,
            Ok(None) => {}
            Err(error) => {
                log::warn(&format!("runOnce: cannot poll {}: {error}", program.display()));
                break;
            }
        }
        if started.elapsed() >= timeout {
            timed_out = true;
            let _ = child.kill();
            let _ = child.wait();
            break;
        }
        std::thread::sleep(Duration::from_millis(40));
    }
    if timed_out {
        log::warn(&format!(
            "runOnce: {} timed out after {}ms, killed",
            program.display(),
            timeout.as_millis()
        ));
    }

    let mut stdout = Vec::new();
    let mut stderr = Vec::new();
    if let Some(mut pipe) = child.stdout.take() {
        let _ = pipe.read_to_end(&mut stdout);
    }
    if let Some(mut pipe) = child.stderr.take() {
        let _ = pipe.read_to_end(&mut stderr);
    }
    let status = child.wait().ok();
    out.exit_code = status.and_then(|status| status.code()).unwrap_or(-1);
    out.output = decode(&stdout).0;
    out.error_output = decode(&stderr).0;
    out
}

/// First `x.y` or `x.y.z` in the text, which is what every `--version` banner leads with.
pub fn extract_version(text: &str) -> String {
    let bytes: Vec<char> = text.chars().collect();
    let mut index = 0usize;
    while index < bytes.len() {
        if !bytes[index].is_ascii_digit() {
            index += 1;
            continue;
        }
        let start = index;
        let mut dots = 0usize;
        let mut end = index;
        while end < bytes.len() {
            let ch = bytes[end];
            if ch.is_ascii_digit() {
                end += 1;
                continue;
            }
            if ch == '.' && dots < 2 {
                // A trailing dot means this was not a version after all.
                if end + 1 >= bytes.len() || !bytes[end + 1].is_ascii_digit() {
                    break;
                }
                dots += 1;
                end += 1;
                continue;
            }
            break;
        }
        if end > start {
            let candidate: String = bytes[start..end].iter().collect();
            if candidate.contains('.') {
                return candidate;
            }
        }
        index = end.max(index + 1);
    }
    String::new()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn version_extraction_matches_the_qt_regex() {
        assert_eq!(extract_version("mysqld  Ver 8.0.12 for Win64"), "8.0.12");
        assert_eq!(extract_version("nginx version: nginx/1.15.11"), "1.15.11");
        assert_eq!(extract_version("Redis server v=3.0.504 sha=00000000"), "3.0.504");
        assert_eq!(extract_version("Python 3.14"), "3.14");
        assert_eq!(extract_version("no digits here"), "");
        assert_eq!(extract_version("build 2."), "");
    }
}
