//! Inline SVG glyphs in the same 24x24 grid the Qt build drew (legacy IconFactory), tinted by
//! the current text color via `stroke: currentColor`.

use dioxus::prelude::*;

pub const HOUSE: &str = "M3 10.5 12 3l9 7.5M5 9.5V21h14V9.5M9 21v-6h6v6";
pub const GLOBE: &str =
    "M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18ZM3 12h18M12 3c2.5 2.4 4 5.6 4 9s-1.5 6.6-4 9c-2.5-2.4-4-5.6-4-9s1.5-6.6 4-9Z";
pub const PACKAGE: &str =
    "M12 3 3 7.5v9L12 21l9-4.5v-9L12 3ZM3 7.5l9 4.5 9-4.5M12 12v9M7.5 5.3l9 4.4";
pub const SETTINGS: &str =
    "M12 15a3 3 0 1 0 0-6 3 3 0 0 0 0 6ZM19.4 15a1.7 1.7 0 0 0 .34 1.87l.06.06a2 2 0 1 1-2.83 2.83l-.06-.06a1.7 1.7 0 0 0-1.87-.34 1.7 1.7 0 0 0-1 1.55V21a2 2 0 1 1-4 0v-.09a1.7 1.7 0 0 0-1-1.55 1.7 1.7 0 0 0-1.87.34l-.06.06a2 2 0 1 1-2.83-2.83l.06-.06a1.7 1.7 0 0 0 .34-1.87 1.7 1.7 0 0 0-1.55-1H3a2 2 0 1 1 0-4h.09a1.7 1.7 0 0 0 1.55-1 1.7 1.7 0 0 0-.34-1.87l-.06-.06a2 2 0 1 1 2.83-2.83l.06.06a1.7 1.7 0 0 0 1.87.34h.01a1.7 1.7 0 0 0 1-1.55V3a2 2 0 1 1 4 0v.09a1.7 1.7 0 0 0 1 1.55 1.7 1.7 0 0 0 1.87-.34l.06-.06a2 2 0 1 1 2.83 2.83l-.06.06a1.7 1.7 0 0 0-.34 1.87v.01a1.7 1.7 0 0 0 1.55 1H21a2 2 0 1 1 0 4h-.09a1.7 1.7 0 0 0-1.55 1Z";
pub const ACTIVITY: &str = "M22 12h-4l-3 9L9 3l-3 9H2";
pub const CHART_PIE: &str =
    "M21.2 15.9A10 10 0 1 1 8.1 2.8M22 12A10 10 0 0 0 12 2v10l8.5 5.5";
pub const FILE_TEXT: &str =
    "M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8l-5-5ZM14 3v5h5M9 13h6M9 17h6";
pub const SPEAKER: &str =
    "M11 5 6 9H2v6h4l5 4V5ZM15.5 8.5a5 5 0 0 1 0 7M19 5a9 9 0 0 1 0 14";
pub const MINUS: &str = "M5.5 12h13";
pub const SQUARE: &str = "M6.5 6.5h11v11h-11z";
pub const RESTORE: &str =
    "M9 4.5h8a1.5 1.5 0 0 1 1.5 1.5v8H16V6.5H9V4.5ZM5 9.5h9A1.5 1.5 0 0 1 15.5 11v7A1.5 1.5 0 0 1 14 19.5H5A1.5 1.5 0 0 1 3.5 18v-7A1.5 1.5 0 0 1 5 9.5Z";
pub const CLOSE: &str = "M18 6 6 18M6 6l12 12";
pub const SEARCH: &str = "M11 19a8 8 0 1 0 0-16 8 8 0 0 0 0 16ZM21 21l-4.3-4.3";
pub const STAR: &str = "M12 3l2.7 5.7 6.3.8-4.6 4.3 1.2 6.2-5.6-3-5.6 3 1.2-6.2L3 9.5l6.3-.8L12 3Z";
pub const TRASH: &str = "M4 7h16M9 7V4h6v3M6 7l1 13h10l1-13M10 11v6M14 11v6";
pub const FOLDER_OPEN: &str =
    "M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v2H5.5L3 19V7Z";
pub const REFRESH: &str =
    "M21 12a9 9 0 1 1-2.6-6.4M21 3v6h-6";
pub const DOWNLOAD: &str = "M12 3v12M7 10l5 5 5-5M4 21h16";

#[component]
pub fn Icon(name: &'static str, size: Option<i32>) -> Element {
    let size = size.unwrap_or(16);
    let d = match name {
        "house" => HOUSE,
        "globe" => GLOBE,
        "package" => PACKAGE,
        "settings" => SETTINGS,
        "activity" => ACTIVITY,
        "chart-pie" => CHART_PIE,
        "file-text" => FILE_TEXT,
        "speaker" => SPEAKER,
        "minus" => MINUS,
        "square" => SQUARE,
        "restore" => RESTORE,
        "close" => CLOSE,
        "search" => SEARCH,
        "star" => STAR,
        "trash" => TRASH,
        "folder-open" => FOLDER_OPEN,
        "refresh" => REFRESH,
        "download" => DOWNLOAD,
        _ => MINUS,
    };
    rsx! {
        svg {
            width: "{size}",
            height: "{size}",
            view_box: "0 0 24 24",
            fill: "none",
            stroke: "currentColor",
            stroke_width: "1.75",
            stroke_linecap: "round",
            stroke_linejoin: "round",
            path { d: "{d}" }
        }
    }
}
