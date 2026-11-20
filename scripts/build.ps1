# devdatapod build script (Rust workspace).
#
# Usage:
#   powershell -File scripts/build.ps1                      # debug build + selftest
#   powershell -File scripts/build.ps1 -Release             # + portable exe/zip in dist/
#   powershell -File scripts/build.ps1 -Release -Installer  # + Inno Setup installer in dist/
#
# ASCII only: this file is read by Windows PowerShell 5.1 which decodes a no-BOM
# .ps1 as GBK on Chinese systems (see AGENTS.md rule 9).

param(
    [switch]$Release,
    [switch]$Installer
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

$profile = if ($Release) { "release" } else { "debug" }
$version = [regex]::Match((Get-Content Cargo.toml -Raw), '(?m)^version = "([0-9.]+)"').Groups[1].Value
$portable = "DevDataPod-v$version-Windows-portable"

Write-Host "==> cargo build ($profile)"
cargo build --profile $profile -p ddp-cli -p ddp-app
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "==> selftest (118 invariants)"
$env:DEVDATAPOD_ROOT = $root
& "target\$profile\ddp-cli.exe" --selftest
if ($LASTEXITCODE -ne 0) { Write-Host "selftest FAILED"; exit 1 }

if ($Release) {
    New-Item -ItemType Directory -Force dist | Out-Null
    Copy-Item "target\$profile\devdatapod.exe" "dist\$portable.exe" -Force
    Copy-Item "target\$profile\ddp-cli.exe" "dist\ddp-cli.exe" -Force
    if (Test-Path "dist\$portable.zip") { Remove-Item "dist\$portable.zip" -Force }
    Compress-Archive -Path "dist\$portable.exe" -DestinationPath "dist\$portable.zip" -Force
    Write-Host "==> dist\$portable.exe / .zip ready"

    if ($Installer) {
        $iscc = $env:ISCC
        if (-not $iscc -or -not (Test-Path $iscc)) { $iscc = "C:\InnoSetup6\ISCC.exe" }
        if (-not (Test-Path $iscc)) { throw "Inno Setup compiler not found (set `$env:ISCC)" }
        New-Item -ItemType Directory -Force dist\stage | Out-Null
        Copy-Item "target\$profile\devdatapod.exe" "dist\stage\devdatapod.exe" -Force
        Write-Host "==> ISCC installer/devdatapod.iss"
        & $iscc installer\devdatapod.iss
        if ($LASTEXITCODE -ne 0) { throw "ISCC failed" }
        Write-Host "==> dist\DevDataPod-v$version-Windows.exe (setup) ready"
    }
}

Write-Host "build OK"
