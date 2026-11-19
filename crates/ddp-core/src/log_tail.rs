//! Decoded slice of a service log, shared by every driver's `logs()` implementation so
//! the quirks are fixed once: the window must start on a line boundary, and a Chinese
//! Windows redis writes GBK.

use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;

#[derive(Clone, Debug, Default)]
pub struct LogTail {
    pub text: String,
    pub encoding: String,
    pub path: String,
    pub truncated: bool,
}

/// Strict UTF-8 well-formedness: rejects overlong forms, surrogate halves and >U+10FFFF,
/// because a lossy decode of those is what "it decoded" looks like when the guess was wrong.
pub fn looks_like_utf8(data: &[u8]) -> bool {
    let mut index = 0usize;
    while index < data.len() {
        let lead = data[index];
        if lead < 0x80 {
            index += 1;
            continue;
        }
        let (extra, mut code): (usize, u32) = if lead & 0xE0 == 0xC0 {
            (1, u32::from(lead & 0x1F))
        } else if lead & 0xF0 == 0xE0 {
            (2, u32::from(lead & 0x0F))
        } else if lead & 0xF8 == 0xF0 {
            (3, u32::from(lead & 0x07))
        } else {
            return false;
        };
        if extra == 1 && code < 2 {
            return false;
        }
        if index + extra >= data.len() {
            return false;
        }
        for offset in 1..=extra {
            let follow = data[index + offset];
            if follow & 0xC0 != 0x80 {
                return false;
            }
            code = (code << 6) | u32::from(follow & 0x3F);
        }
        if extra == 2 && code < 0x800 {
            return false;
        }
        if extra == 3 && code < 0x10000 {
            return false;
        }
        if code > 0x10FFFF {
            return false;
        }
        if (0xD800..=0xDFFF).contains(&code) {
            return false;
        }
        index += extra + 1;
    }
    true
}

/// Decode with the best guess, filling `encoding` with what was used.
pub fn decode(data: &[u8]) -> (String, String) {
    if looks_like_utf8(data) {
        return (String::from_utf8_lossy(data).into_owned(), "UTF-8".into());
    }
    #[cfg(windows)]
    {
        use windows_sys::Win32::Globalization::GetACP;
        use windows_sys::Win32::Globalization::MultiByteToWideChar;
        let codepage = unsafe { GetACP() };
        if codepage != 0 {
            let needed = unsafe {
                MultiByteToWideChar(
                    codepage,
                    0,
                    data.as_ptr(),
                    data.len() as i32,
                    std::ptr::null_mut(),
                    0,
                )
            };
            if needed > 0 {
                let mut buffer = vec![0u16; needed as usize];
                let written = unsafe {
                    MultiByteToWideChar(
                        codepage,
                        0,
                        data.as_ptr(),
                        data.len() as i32,
                        buffer.as_mut_ptr(),
                        needed,
                    )
                };
                if written > 0 {
                    buffer.truncate(written as usize);
                    return (
                        String::from_utf16_lossy(&buffer),
                        format!("CP{codepage}"),
                    );
                }
            }
        }
    }
    (String::from_utf8_lossy(data).into_owned(), "UTF-8 (best effort)".into())
}

/// Last `lines` lines of `path`, reading at most `max_bytes` from the end.
pub fn read_tail(path: &Path, lines: usize, max_bytes: u64) -> LogTail {
    let mut tail = LogTail {
        path: path.display().to_string(),
        ..Default::default()
    };
    if !path.exists() {
        tail.encoding = "UTF-8".into();
        return tail;
    }
    let Ok(mut file) = File::open(path) else {
        tail.text = format!("<cannot open {}>", path.display());
        tail.encoding = "UTF-8".into();
        return tail;
    };
    let size = file.metadata().map(|meta| meta.len()).unwrap_or(0);
    let window = max_bytes.max(1024);
    let start = size.saturating_sub(window);
    if start > 0 {
        tail.truncated = true;
        if file.seek(SeekFrom::Start(start)).is_err() {
            tail.text = format!("<cannot seek {}>", path.display());
            return tail;
        }
    }
    let mut raw = Vec::new();
    if file.read_to_end(&mut raw).is_err() {
        tail.text = format!("<cannot read {}>", path.display());
        return tail;
    }
    // The window opened mid-line: drop the first fragment rather than show half a line.
    if start > 0 {
        if let Some(newline) = raw.iter().position(|byte| *byte == b'\n') {
            raw.drain(..=newline);
        }
    }
    let (decoded, encoding) = decode(&raw);
    tail.encoding = encoding;
    let mut rows: Vec<&str> = decoded.split('\n').filter(|row| !row.is_empty()).collect();
    if lines > 0 && rows.len() > lines {
        rows = rows[rows.len() - lines..].to_vec();
    }
    tail.text = rows
        .iter()
        .map(|row| row.strip_suffix('\r').unwrap_or(row))
        .collect::<Vec<_>>()
        .join("\n");
    tail
}
