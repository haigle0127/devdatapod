//! Managed-block configuration editing (AGENTS.md rule 6).
//!
//! The panel renders the options it owns, and never silently overwrites what the user
//! edited. The boundary is an explicit marker pair; everything outside it is copied through
//! verbatim, at byte level, so encoding and line endings are never normalised by accident.

use std::path::Path;

use crate::atomic;
use crate::log;

pub const BEGIN_MARKER: &str = "# --- devdatapod managed block (do not edit inside) ---";
pub const END_MARKER: &str = "# --- end managed block ---";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BlockOutcome {
    /// Created, or the block between the markers was replaced.
    Written,
    /// The file exists but has no markers: it belongs to the user.
    Preserved,
    /// Could not read or write.
    Failed,
}

fn find_marker(lines: &[&[u8]], marker: &[u8], from: usize) -> Option<usize> {
    let wanted = trim(marker);
    for (index, line) in lines.iter().enumerate().skip(from) {
        if trim(line) == wanted {
            return Some(index);
        }
    }
    None
}

fn trim(mut value: &[u8]) -> &[u8] {
    while let Some((first, rest)) = value.split_first() {
        if first.is_ascii_whitespace() {
            value = rest;
        } else {
            break;
        }
    }
    while let Some((last, rest)) = value.split_last() {
        if last.is_ascii_whitespace() {
            value = rest;
        } else {
            break;
        }
    }
    value
}

/// Replace or insert the block in `path`. `body` is the rendered text for the options the
/// panel owns, without the markers.
pub fn apply(path: &Path, body: &str) -> Result<BlockOutcome, String> {
    let begin = BEGIN_MARKER.as_bytes();
    let end = END_MARKER.as_bytes();

    if !path.exists() {
        let mut data = Vec::new();
        data.extend_from_slice(
            b"# written by devdatapod. Everything outside the managed block below is\n\
              # yours: the panel only ever replaces the lines between the markers.\n",
        );
        data.extend_from_slice(begin);
        data.push(b'\n');
        data.extend_from_slice(body.as_bytes());
        if !data.ends_with(b"\n") {
            data.push(b'\n');
        }
        data.extend_from_slice(end);
        data.push(b'\n');
        if !atomic::write_atomic(path, &data) {
            return Err(format!("cannot create {}", path.display()));
        }
        return Ok(BlockOutcome::Written);
    }

    let raw = atomic::read_bytes(path)?;
    let crlf = raw.windows(2).any(|pair| pair == b"\r\n");
    let lines: Vec<&[u8]> = raw.split(|byte| *byte == b'\n').collect();

    let Some(begin_index) = find_marker(&lines, begin, 0) else {
        log::info(&format!(
            "config: {} has no devdatapod block, leaving it alone",
            path.display()
        ));
        return Ok(BlockOutcome::Preserved);
    };
    let Some(end_index) = find_marker(&lines, end, begin_index + 1) else {
        return Err(format!(
            "{} has the opening marker at line {} but no closing one; refusing to guess \
             where the block ends",
            path.display(),
            begin_index + 1
        ));
    };

    let mut inserted = body.as_bytes().to_vec();
    if !inserted.is_empty() && !inserted.ends_with(b"\n") {
        inserted.push(b'\n');
    }
    let mut middle: Vec<Vec<u8>> = inserted.split(|byte| *byte == b'\n').map(Vec::from).collect();
    if middle.last().map(Vec::is_empty).unwrap_or(false) {
        middle.pop();
    }
    if crlf {
        for line in middle.iter_mut() {
            line.push(b'\r');
        }
    }

    let mut rebuilt: Vec<u8> = Vec::with_capacity(raw.len() + inserted.len() + 64);
    for line in &lines[..begin_index] {
        rebuilt.extend_from_slice(line);
        rebuilt.push(b'\n');
    }
    rebuilt.extend_from_slice(begin);
    if crlf {
        rebuilt.push(b'\r');
    }
    rebuilt.push(b'\n');
    for line in &middle {
        rebuilt.extend_from_slice(line);
        rebuilt.push(b'\n');
    }
    rebuilt.extend_from_slice(end);
    if crlf {
        rebuilt.push(b'\r');
    }
    rebuilt.push(b'\n');
    for (index, line) in lines.iter().enumerate().skip(end_index + 1) {
        if index + 1 == lines.len() && line.is_empty() {
            break;
        }
        rebuilt.extend_from_slice(line);
        if index + 1 < lines.len() {
            rebuilt.push(b'\n');
        }
    }
    if !rebuilt.ends_with(b"\n") {
        rebuilt.push(b'\n');
    }

    if !atomic::write_atomic(path, &rebuilt) {
        return Err(format!("cannot replace the block in {}", path.display()));
    }
    Ok(BlockOutcome::Written)
}

pub fn has_block(path: &Path) -> bool {
    let Ok(raw) = std::fs::read(path) else {
        return false;
    };
    let lines: Vec<&[u8]> = raw.split(|byte| *byte == b'\n').collect();
    find_marker(&lines, BEGIN_MARKER.as_bytes(), 0).is_some()
}
