# devdatapod build script (Rust workspace).
#
# Usage:
#   powershell -File scripts/build.ps1            # debug build + selftest
#   powershell -File scripts/build.ps1 -Release   # release build + selftest, exe copied to dist/
#
# ASCII only: this file is read by Windows PowerShell 5.1 which decodes a no-BOM
# .ps1 as GBK on Chinese systems (see AGENTS.md rule 9).

param(
    [switch]$Release
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

$profile = if ($Release) { "release" } else { "debug" }

Write-Host "==> cargo build ($profile)"
cargo build --profile $profile -p ddp-cli -p ddp-app
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "==> selftest (118 invariants)"
$env:DEVDATAPOD_ROOT = $root
& "target\$profile\ddp-cli.exe" --selftest
if ($LASTEXITCODE -ne 0) { Write-Host "selftest FAILED"; exit 1 }

if ($Release) {
    New-Item -ItemType Directory -Force dist | Out-Null
    Copy-Item "target\$profile\devdatapod.exe" dist\devdatapod.exe -Force
    Copy-Item "target\$profile\ddp-cli.exe" dist\ddp-cli.exe -Force
    Write-Host "==> dist\devdatapod.exe ready"
}

Write-Host "build OK"
