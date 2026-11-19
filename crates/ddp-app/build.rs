//! Embeds logo.ico as the Windows application icon (explorer / taskbar / alt-tab for the
//! exe itself; the running window also gets an explicit HICON via Config::with_icon).

#[cfg(target_os = "windows")]
fn main() {
    let mut resource = winres::WindowsResource::new();
    resource.set_icon("assets/logo.ico");
    // File properties -> Details: 2.0.0.0.
    let version: u64 = 2 << 48;
    resource.set_version_info(winres::VersionInfo::FILEVERSION, version);
    resource.set_version_info(winres::VersionInfo::PRODUCTVERSION, version);
    resource.set("ProductName", "devdatapod");
    resource.set("InternalName", "devdatapod");
    resource.set("FileDescription", "devdatapod local dev service panel");
    resource.set("LegalCopyright", "devdatapod");
    if let Err(error) = resource.compile() {
        // A missing SDK rc.exe must not break development builds; the runtime window icon
        // still applies once the app is running.
        eprintln!("warning: could not embed exe icon: {error}");
    }
}

#[cfg(not(target_os = "windows"))]
fn main() {}
