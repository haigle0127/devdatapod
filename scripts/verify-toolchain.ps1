<#
  devdatapod P0 toolchain acceptance check.

  KEEP THIS FILE PURE ASCII. Windows PowerShell 5.1 decodes .ps1 using the
  system ANSI codepage (GBK on Chinese Windows) unless a UTF-8 BOM is present.

  Existence of qmake.exe / vcvars64.bat is not proof the toolchain works.
  This builds and runs a real Qt6 Core executable through MSVC + CMake + Ninja,
  so it exercises headers, import libs, the CRT flavour, and Qt6Core.dll
  resolution at runtime. That is the P0 acceptance criterion.

  Every line is teed to logs\verify-<ts>.log so a killed process still leaves
  a trace of how far it got.
#>
param(
    [string]$QtRoot    = 'D:\Qt\6.8.3\msvc2022_64',
    [string]$VcVars    = 'D:\BuildTools\VC\Auxiliary\Build\vcvars64.bat',
    [string]$ScriptBin = 'C:\Users\haigle\AppData\Roaming\Python\Python314\Scripts',
    [string]$ProbeDir  = '',
    [ValidateSet('files', 'all')]
    [string]$Stage     = 'all',
    [switch]$Keep,
    [switch]$NoPause
)

# Deliberately not 'Stop': a native command writing to stderr must not become
# a terminating error (that produced a false FAILED in setup-toolchain.ps1).
$ErrorActionPreference = 'Continue'
$ProgressPreference    = 'SilentlyContinue'

$repoRoot = Split-Path -Parent $PSScriptRoot
$logDir   = Join-Path $repoRoot 'logs'
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir -Force | Out-Null }
$LogFile  = Join-Path $logDir ('verify-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.log')

if (-not $ProbeDir) { $ProbeDir = Join-Path $repoRoot '.p0-probe' }

$script:Failed = $false

function Say {
    param([string]$Message, [string]$Color = 'Gray')
    $line = ('[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $Message)
    Write-Host $line -ForegroundColor $Color
    Add-Content -Path $LogFile -Value $line -Encoding UTF8 -ErrorAction SilentlyContinue
}

function Step {
    param([string]$Name, [bool]$Ok, [string]$Detail = '')
    if ($Ok) {
        Say ('  [ok]   ' + $Name) 'Green'
    }
    else {
        Say ('  [FAIL] ' + $Name) 'Red'
        $script:Failed = $true
    }
    if ($Detail) { Say ('         ' + $Detail) 'DarkGray' }
}

# Run a native command inside a cmd that has already loaded the MSVC env,
# capture stdout AND stderr as text, and echo the exit code. 2>&1 stays inside
# the cmd string on purpose - see the note above.
function Invoke-Env {
    param([string]$Body, [string]$Label)
    # /v:on + !ERRORLEVEL! is mandatory: %ERRORLEVEL% on a line that chains
    # commands with & is expanded at PARSE time, so it would report the exit
    # code of whatever ran before this cmd even started.
    $cmd = 'call "' + $VcVars + '" >NUL 2>&1 && ' + $Body + ' & echo __EXIT=!ERRORLEVEL!'
    Say ('--- ' + $Label + ' ---') 'Cyan'
    Say (($cmd.Substring(0, [Math]::Min(160, $cmd.Length))) + ' ...') 'DarkGray'
    $raw = & cmd.exe /v:on /c $cmd 2>&1
    $text = @($raw | ForEach-Object { [string]$_ })
    $code = $null
    foreach ($l in $text) {
        if ($l -match '__EXIT=(-?\d+)') { $code = [int]$matches[1] }
        if ($l -notmatch '__EXIT=') { Say ('    ' + $l) 'DarkGray' }
    }
    if ($null -eq $code) { Say '    exit code = <not reported>' 'Yellow' } else { Say ('    exit code = ' + $code) 'Gray' }
    return [pscustomobject]@{ Code = $code; Lines = $text }
}

Say ''
Say '=== devdatapod P0 toolchain acceptance ===' 'White'
Say ('log     : ' + $LogFile) 'DarkGray'
Say ('QtRoot  : ' + $QtRoot) 'DarkGray'
Say ('VcVars  : ' + $VcVars) 'DarkGray'
Say ('ScriptBin: ' + $ScriptBin) 'DarkGray'
Say ('probe   : ' + $ProbeDir) 'DarkGray'
Say ('stage   : ' + $Stage) 'DarkGray'
Say ''
Say 'If this window disappears with no output, powershell.exe was killed from' 'Yellow'
Say 'outside the script (a script error would still stop at the pause). The' 'Yellow'
Say 'usual suspect is 360 / Huorong behavioural protection reacting to a' 'Yellow'
Say 'compiler writing an exe and executing it. Exit the suite from its tray' 'Yellow'
Say 'icon, or add an exclusion for the probe directory, then re-run.' 'Yellow'
Say ('probe dir: ' + $ProbeDir) 'Yellow'

try {
    $required = [ordered]@{
        'cmake'    = (Join-Path $ScriptBin 'cmake.exe')
        'ninja'    = (Join-Path $ScriptBin 'ninja.exe')
        'qmake'    = (Join-Path $QtRoot 'bin\qmake.exe')
        'vcvars64' = $VcVars
    }
    foreach ($key in $required.Keys) {
        $path = $required[$key]
        Step $key (Test-Path -LiteralPath $path) $path
    }
    if ($script:Failed) {
        Say '' 'White'
        Say 'Toolchain files missing. Run scripts\setup-toolchain.ps1 first.' 'Red'
        throw 'toolchain files missing'
    }

    # cl only answers once the MSVC environment exists, so probe it the same
    # way the build will: inside a cmd that has called vcvars64 first.
    # cl with no arguments prints its banner and returns exit code 1
    # ("No filename specified"), so the exit code proves nothing here - match
    # the banner text instead. It is localized, but this fragment is fixed.
    $cl = Invoke-Env -Body 'cl 2>&1' -Label 'MSVC compiler banner'
    $banner = ($cl.Lines | Where-Object { $_ -match 'C/C\+\+' } | Select-Object -First 1)
    $ver = ''
    if ($banner -match '(\d+\.\d+\.\d+(?:\.\d+)?)') { $ver = 'version ' + $matches[1] }
    Step 'MSVC compiler responds' ([bool]$banner) "$banner $ver"

    if ($Stage -eq 'files') {
        Say '' 'White'
        Say 'Stage "files" only - stopped before writing or building anything.' 'Yellow'
        # A bare `return` here would skip the SUMMARY block and the press-Enter
        # guard at the bottom, which is indistinguishable from a crash.
        throw 'stop-after-files'
    }

    if (Test-Path -LiteralPath $ProbeDir) {
        if ($Keep) {
            Say 'keeping existing probe dir (-Keep)' 'Yellow'
        }
        else {
            Remove-Item -LiteralPath $ProbeDir -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
    New-Item -ItemType Directory -Force -Path (Join-Path $ProbeDir 'build') | Out-Null

    @'
cmake_minimum_required(VERSION 3.22)
project(devdatapod_p0_probe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core)
qt_standard_project_setup()

qt_add_executable(probe main.cpp)
target_link_libraries(probe PRIVATE Qt6::Core)
'@ | Set-Content -LiteralPath (Join-Path $ProbeDir 'CMakeLists.txt') -Encoding ASCII

    @'
#include <QCoreApplication>
#include <QDebug>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    qDebug() << "devdatapod P0 probe: Qt runtime" << qVersion() << "compiled against" << QT_VERSION_STR;
    return 0;
}
'@ | Set-Content -LiteralPath (Join-Path $ProbeDir 'main.cpp') -Encoding ASCII

    $cmake = Join-Path $ScriptBin 'cmake.exe'
    $src   = $ProbeDir
    $bin   = Join-Path $ProbeDir 'build'
    $exe   = Join-Path $bin 'probe.exe'

    # configure / build / run are three separate cmd invocations on purpose:
    # one long && chain hides which stage actually failed.
    $cfg = Invoke-Env -Label 'CMake configure' -Body (
        '"' + $cmake + '" -S "' + $src + '" -B "' + $bin + '" -G Ninja' +
        ' -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="' + $QtRoot + '" 2>&1')
    Step 'CMake configured with the Ninja generator' ($cfg.Code -eq 0)

    $bld = Invoke-Env -Label 'CMake build' -Body (
        '"' + $cmake + '" --build "' + $bin + '" 2>&1')
    Step 'MSVC + Ninja compiled and linked probe.exe' (($bld.Code -eq 0) -and (Test-Path -LiteralPath $exe))

    $run = Invoke-Env -Label 'Run probe.exe' -Body (
        'set "PATH=%PATH%;' + $QtRoot + '\bin" && "' + $exe + '" 2>&1')
    $ranLine = $run.Lines | Where-Object { $_ -match 'devdatapod P0 probe' } | Select-Object -First 1
    Step 'Qt6 binary ran and resolved Qt6Core.dll' ([bool]$ranLine) "$ranLine"
}
catch {
    if ($_.Exception.Message -eq 'stop-after-files') {
        # Not an error - the files-only stage asked to stop here.
    }
    else {
        $msg = $_.Exception.Message
        if ($_.InvocationInfo -and $_.InvocationInfo.ScriptLineNumber) {
            $msg += '  (line ' + $_.InvocationInfo.ScriptLineNumber + ')'
        }
        if ($_.Exception.InnerException) { $msg += ' | inner: ' + $_.Exception.InnerException.Message }
        Say ('EXCEPTION: ' + $msg) 'Red'
        $script:Failed = $true
    }
}

Say ''
Say '================ SUMMARY ================' 'White'
if ($script:Failed) {
    Say 'P0 NOT accepted - see [FAIL] / EXCEPTION lines above.' 'Red'
}
else {
    Say 'P0 accepted. Toolchain is usable.' 'Green'
    Say ('  CMAKE_PREFIX_PATH = ' + $QtRoot) 'DarkGray'
}
Say ('full log: ' + $LogFile) 'DarkGray'

if (-not $NoPause) { Read-Host 'Press Enter to close' }
if ($script:Failed) { exit 1 }
exit 0
