#Requires -Version 5.1
# devdatapod P1 build helper. KEEP THIS FILE PURE ASCII (AGENTS.md rule 9).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -SelfTest
#   powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Run
#   powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 `
#       -Stage Extensions\redis3.0.504, Extensions\redis7.2.5
#
# What it does: imports the MSVC x64 environment, configures with CMake + Ninja,
# builds, and reports the executable. The whole session is captured to
# logs\build-<timestamp>.log because a double-clicked window closes too fast to read.

[CmdletBinding()]
param(
    [string]$Config = 'Debug',
    [string]$Qt,
    [switch]$Run,
    [switch]$SelfTest,
    [string[]]$Stage,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$Proj = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$BuildDir = Join-Path $Proj 'build'
$LogDir = Join-Path $Proj 'logs'
if (-not (Test-Path $LogDir)) { $null = New-Item -ItemType Directory -Path $LogDir }

$LogPath = Join-Path $LogDir ("build-{0}.log" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
Start-Transcript -Path $LogPath | Out-Null

function Step($text) { Write-Host ''; Write-Host "== $text" -ForegroundColor Cyan }
function Fail($text) {
    Write-Host ''
    Write-Host "FAILED: $text" -ForegroundColor Red
    Write-Host "transcript: $LogPath"
    exit 1
}

try {
    Step 'Locating toolchain'

    # PATH order decides this; the P0 setup installed cmake+ninja under the user's
    # Roaming Python Scripts directory.
    foreach ($tool in 'cmake', 'ninja', 'git', 'node') {
        if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
            Fail "$tool is not on PATH. Run scripts\setup-toolchain.cmd first, or open a shell where it is."
        }
        Write-Host ("  {0,-6} {1}" -f $tool, (Get-Command $tool).Source)
    }

    if (-not $Qt) { $Qt = $env:DEVDATAPOD_QT }
    if (-not $Qt) { $Qt = 'D:\Qt\6.8.3\msvc2022_64' }
    if (-not (Test-Path (Join-Path $Qt 'lib\cmake\Qt6'))) {
        Fail "no Qt6 CMake package under $Qt (set -Qt or DEVDATAPOD_QT to the msvc2022_64 directory)"
    }
    Write-Host "  qt     $Qt"

    $vcvarsCandidates = @()
    if ($env:DEVDATAPOD_VCVARS) { $vcvarsCandidates += $env:DEVDATAPOD_VCVARS }
    $vcvarsCandidates += @(
        'D:\BuildTools\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
    )
    $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $found = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                              -property installationPath
        if ($LASTEXITCODE -eq 0 -and $found) {
            $vcvarsCandidates += (Join-Path $found 'VC\Auxiliary\Build\vcvars64.bat')
        }
    }
    $VcVars = $vcvarsCandidates | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
    if (-not $VcVars) {
        Fail 'vcvars64.bat not found. Set DEVDATAPOD_VCVARS or -Qt-style override is not enough - install the VS Build Tools (P0 step 3).'
    }
    Write-Host "  vcvars $VcVars"

    if ($Clean -and (Test-Path $BuildDir)) {
        Step 'Cleaning build directory'
        Remove-Item -Recurse -Force $BuildDir
    }

    Step 'Configuring (this also regenerates the theme from design\devconsole.pen)'
    $exe = Join-Path $BuildDir 'devdatapod.exe'
    $configure = 'call "{0}" && cmake -S "{1}" -B "{2}" -G Ninja -DCMAKE_BUILD_TYPE={3} -DCMAKE_PREFIX_PATH="{4}"' -f $VcVars, $Proj, $BuildDir, $Config, $Qt

    Step 'Building'
    $full = '{0} && cmake --build "{1}"' -f $configure, $BuildDir
    # Native tools write progress to stderr; under "Stop" PowerShell 5.1 turns the first
    # such line into a NativeCommandError and aborts a build that is actually fine.
    $preference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $env:ComSpec /d /s /c $full 2>&1 | ForEach-Object { Write-Host "$_" }
    $rc = $LASTEXITCODE
    $ErrorActionPreference = $preference
    if ($rc -ne 0) { Fail "cmake/ninja exited with code $rc (theme generation is the other thing that can abort configure)" }

    if (-not (Test-Path $exe)) { Fail "build reported success but $exe is missing" }

    Step 'Result'
    Write-Host "  ok     $exe" -ForegroundColor Green
    Write-Host ("  size   {0:n0} bytes" -f (Get-Item $exe).Length)
    Write-Host "  run    `"$exe`""
    Write-Host "  log    $LogPath"

    # A dev build tree has the exe in build\, which would make AppPaths treat build\ as the
    # install root. .gitignore reserves data\ and Extensions\ for the project root, so pin it.
    # Start-Process inherits the environment, which covers -Run as well.
    $env:DEVDATAPOD_ROOT = $Proj

    $selftestExe = Join-Path $BuildDir 'devdatapod-selftest.exe'
    if ($SelfTest -or $Stage) {
        if (-not (Test-Path $selftestExe)) { Fail "$selftestExe is missing" }
        $env:PATH = "$Qt\bin;$env:PATH"
    }

    if ($SelfTest) {
        Step 'Selftest'
        # Console subsystem on purpose: this waits and gives us $LASTEXITCODE, which
        # `devdatapod.exe --selftest` never could (a windowed process makes no shell wait).
        & $selftestExe 2>&1 | ForEach-Object { Write-Host "$_" }
        $stRc = $LASTEXITCODE
        if ($stRc -ne 0) { Fail "devdatapod-selftest exited with code $stRc" }
        Write-Host "  ok     P2 port/instance invariants" -ForegroundColor Green
    }

    if ($Stage) {
        Step 'Staging components'
        # The exe resolves its install root from its own location, so data/services.json and
        # data/ports.json land in build\ unless DEVDATAPOD_ROOT points at a shipped layout.
        & $selftestExe --stage $Stage 2>&1 | ForEach-Object { Write-Host "$_" }
        $stRc = $LASTEXITCODE
        if ($stRc -ne 0) { Fail "devdatapod-selftest --stage exited with code $stRc" }
    }

    if ($Run) {
        Step 'Launching'
        # The exe needs Qt6Core.dll etc. on PATH when run from the build tree.
        # Start-Process has no -Environment on PowerShell 5.1, so edit PATH instead.
        $env:PATH = "$Qt\bin;$env:PATH"
        Start-Process -FilePath $exe -WorkingDirectory $BuildDir
    }
    Stop-Transcript | Out-Null
    exit 0
}
catch {
    Stop-Transcript -ErrorAction SilentlyContinue | Out-Null
    Fail $_.Exception.Message
}
