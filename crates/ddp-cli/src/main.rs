//! Console entry point, the Rust replacement for `devdatapod-selftest.exe`.
//!
//! Three modes:
//!   * no arguments — run the invariant suite inside a throwaway temp root, exit 0/1;
//!   * `--stage <dir>...` — register component directories that already exist under
//!     `Extensions/`, reconcile ports, and prove each binary reports its directory's version;
//!   * `--run <id>` — start one instance, wait for its protocol probe to say Running, stop it.
//!
//! A console binary, not the GUI: a windowed process does not make a shell wait, and the
//! acceptance evidence is an exit code plus the check list on stdout.

use std::process::ExitCode;

use ddp_core::paths::Paths;
use ddp_core::{log, selftest, stage};

fn main() -> ExitCode {
    let arguments: Vec<String> = std::env::args().skip(1).collect();
    let paths = Paths::discover();
    // Staging writes the real data/ and logs/; a failed assertion with no log line is far
    // harder to diagnose, so logging is initialised in every mode.
    paths.ensure_writable_dirs();
    log::init(&paths);

    let code = if let Some(index) = arguments.iter().position(|item| item == "--run") {
        match arguments.get(index + 1) {
            Some(id) => stage::run_instance(&paths, id),
            None => {
                println!("usage: ddp-cli --run <instance id>");
                1
            }
        }
    } else if let Some(index) = arguments.iter().position(|item| item == "--stage") {
        stage::run_stage(&paths, &arguments[index + 1..])
    } else if arguments.iter().any(|item| item == "--help" || item == "-h") {
        println!(
            "devdatapod maintenance CLI\n\n  \
             ddp-cli                  run the invariant suite\n  \
             ddp-cli --selftest       same, spelled out\n  \
             ddp-cli --stage <dir>... register Extensions/<kind><version> directories\n  \
             ddp-cli --run <id>       start one instance, prove it, stop it\n"
        );
        0
    } else {
        run_selftest()
    };

    log::shutdown();
    ExitCode::from(code.clamp(0, 255) as u8)
}

fn run_selftest() -> i32 {
    let report = selftest::run();
    for line in &report.lines {
        println!("{line}");
    }
    println!(
        "\n{} passed, {} failed{}",
        report.passed,
        report.failed,
        if report.success() { "" } else { " - see [FAIL] above" }
    );
    if report.success() {
        0
    } else {
        1
    }
}
