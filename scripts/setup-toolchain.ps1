<#
  devdatapod P0 toolchain installer

  KEEP THIS FILE PURE ASCII. Windows PowerShell 5.1 decodes .ps1 using the
  system ANSI codepage (GBK on Chinese Windows) unless a UTF-8 BOM is present;
  non-ASCII literals become mojibake and can break parsing outright.

  Ordering and fault isolation are deliberate:
    - Qt / CMake / Ninja need no admin and no compiler, so they run FIRST.
    - VS Build Tools runs last and only asks for elevation for itself.
    - Every step is independently guarded. One failure marks that step FAILED
      and the rest still run, so a single partial success is still progress.
#>
[CmdletBinding()]
param(
    [string]$QtDir   = 'D:\Qt',
    [string]$VsDir   = 'D:\BuildTools',
    [string]$QtMinor = '6.8',
    [switch]$SkipQt,
    [switch]$SkipVs
)

$ErrorActionPreference = 'Stop'
$LogDir = 'D:\code\devdatapod\logs'
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$LogFile = Join-Path $LogDir ('setup-{0}.log' -f (Get-Date -Format 'yyyyMMdd-HHmmss'))

function Log {
    param([string]$Message, [string]$Color = 'Gray')
    Write-Host ('[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $Message) -ForegroundColor $Color
    Add-Content -Path $LogFile -Value ('[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $Message) -Encoding UTF8 -ErrorAction SilentlyContinue
}

$script:Results = [ordered]@{}
function Invoke-Step {
    # $When must be [bool], not [switch]: a switch never consumes a value in
    # argument position, so `-When (-not $SkipVs)` would read as "present"
    # (= true) and silently run the step it was meant to skip.
    param([string]$Name, [bool]$When = $true, [scriptblock]$Body)
    Log '' 'White'
    Log "--- $Name ---" 'Cyan'
    if (-not $When) { Log 'skipped by switch' 'Yellow'; $script:Results[$Name] = 'SKIP'; return }
    try {
        & $Body
        $script:Results[$Name] = 'OK'
    }
    catch {
        $msg = $_.Exception.Message
        if ($_.Exception.InnerException) { $msg += ' | inner: ' + $_.Exception.InnerException.Message }
        Log "FAILED: $msg" 'Red'
        $script:Results[$Name] = "FAILED: $msg"
    }
}

$script:Py = $null
function Invoke-Py {
    param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments)
    & $script:Py @Arguments
    if ($LASTEXITCODE -ne 0) { throw "py $($Arguments -join ' ') exited with $LASTEXITCODE" }
}

function Get-FileRobust {
    param([string]$Url, [string]$Out, [long]$MinBytes = 1048576)
    $curl = Join-Path $env:SystemRoot 'System32\curl.exe'
    if (Test-Path $curl) {
        Log "curl -L --retry 5 $Url" 'DarkGray'
        & $curl --silent --show-error --location --retry 5 --retry-delay 3 --output $Out $Url
        if ($LASTEXITCODE -ne 0) { throw "curl exited $LASTEXITCODE for $Url" }
    }
    else {
        try { [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 } catch { }
        Invoke-WebRequest -Uri $Url -OutFile $Out -UseBasicParsing
    }
    if (-not (Test-Path $Out)) { throw "no file written for $Url" }
    $size = (Get-Item $Out).Length
    if ($size -lt $MinBytes) { throw "downloaded only $size bytes from $Url - looks like an error page, not the real file" }
    Log "downloaded $size bytes" 'DarkGray'
}

Log '=== devdatapod toolchain install ===' 'White'
Log "log: $LogFile" 'DarkGray'
Log "QtDir=$QtDir  VsDir=$VsDir  SkipQt=$SkipQt  SkipVs=$SkipVs" 'DarkGray'

Invoke-Step '1/7 Python' -Body {
    foreach ($cand in @('py', 'python')) {
        if (Get-Command $cand -ErrorAction SilentlyContinue) { $script:Py = $cand; break }
    }
    if (-not $script:Py) { throw 'Python not found. Install https://www.python.org/downloads/ and tick "Add python.exe to PATH".' }
    Invoke-Py -c "import sys;print('python  ' + sys.version.split()[0] + '  ' + sys.executable)"
}

Invoke-Step '2/7 CMake + Ninja + aqtinstall (pip, user scope, no admin)' -Body {
    Invoke-Py -m pip install --user --upgrade aqtinstall cmake ninja

    # pip --user on Windows puts console scripts in %APPDATA%\Python\PythonXY\Scripts.
    # That is NOT site.USER_BASE + '\Scripts' - it misses the PythonXY level.
    $UserSite = (Invoke-Py -c "import site;print(site.getusersitepackages())").Trim()
    $Prefix   = (Invoke-Py -c "import sys;print(sys.prefix)").Trim()
    $VerDir   = Split-Path $UserSite -Parent
    $script:Candidates = @((Join-Path $VerDir 'Scripts'), (Join-Path $Prefix 'Scripts'))
    $script:PyScripts  = $Candidates[0]

    function Find-Exe {
        param([string]$Name)
        foreach ($d in $script:Candidates) {
            if ($d) { $p = Join-Path $d $Name; if (Test-Path $p) { return (Resolve-Path $p).Path } }
        }
        foreach ($root in @($VerDir, $Prefix)) {
            if ($root -and (Test-Path $root)) {
                $hit = Get-ChildItem -Path $root -Recurse -Filter $Name -ErrorAction SilentlyContinue |
                    Select-Object -First 1
                if ($hit) { return $hit.FullName }
            }
        }
        return $null
    }
    $script:CMakeExe = Find-Exe 'cmake.exe'
    $script:NinjaExe = Find-Exe 'ninja.exe'
    if ($script:CMakeExe) { Log "cmake  $script:CMakeExe" 'Green' } else { Log 'cmake.exe not found' 'Yellow' }
    if ($script:NinjaExe) { Log "ninja  $script:NinjaExe" 'Green' } else { Log 'ninja.exe not found' 'Yellow' }
    if (-not $script:CMakeExe -and -not $script:NinjaExe) { throw 'neither cmake nor ninja could be located after pip install' }
}

Invoke-Step '3/7 Resolve Qt version' -When (-not $SkipQt) -Body {
    $Versions = ((Invoke-Py -m aqt list-qt windows desktop) -split '\s+') | Where-Object { $_ -match '^\d+\.\d+\.\d+$' }
    if (-not $Versions) { throw 'aqt returned no version list - check proxy / firewall on https://download.qt.io' }
    $script:Chosen = $Versions | Where-Object { $_ -like "$QtMinor.*" } |
        Sort-Object { [version]$_ } -Descending | Select-Object -First 1
    if (-not $script:Chosen) {
        Log "no $QtMinor.x available, falling back to newest" 'Yellow'
        $script:Chosen = $Versions | Sort-Object { [version]$_ } -Descending | Select-Object -First 1
    }
    $script:QtRoot = Join-Path $QtDir ($script:Chosen + '\msvc2022_64')
    Log "selected Qt $script:Chosen" 'Green'
}

Invoke-Step '4/7 Download Qt (8-12 GB, silent for a long time - that is normal)' -When (-not $SkipQt) -Body {
    if (Test-Path (Join-Path $script:QtRoot 'bin\qmake.exe')) { Log 'already installed, skipping' 'Green'; return }
    # Must splat this one: a bare -O in a function call binds to the common
    # parameter -OutVariable/-OutBuffer (ambiguous) and never reaches aqt.
    $aqtArgs = @('-m', 'aqt', 'install-qt', 'windows', 'desktop', $script:Chosen,
                 'win64_msvc2022_64', '-O', $QtDir)
    Invoke-Py @aqtArgs
    if (-not (Test-Path (Join-Path $script:QtRoot 'bin\qmake.exe'))) {
        throw 'aqt finished but qmake.exe is missing - a partial download may be stuck; delete ' + $script:QtRoot + ' and re-run'
    }
}

Invoke-Step '5/7 Qt Creator (optional)' -When (-not $SkipQt) -Body {
    $aqtArgs = @('-m', 'aqt', 'install-tool', 'windows', 'desktop', 'tools_qtcreator', '-O', $QtDir)
    Invoke-Py @aqtArgs
    Log 'Qt Creator ready' 'Green'
}

Invoke-Step '6/7 VS Build Tools + MSVC (one UAC prompt)' -When (-not $SkipVs) -Body {
    $script:VcVars = Join-Path $VsDir 'VC\Auxiliary\Build\vcvars64.bat'
    if (Test-Path $script:VcVars) { Log 'already installed' 'Green'; return }
    $Boot = Join-Path $env:TEMP 'vs_BuildTools.exe'
    if (-not (Test-Path $Boot)) { Get-FileRobust -Url 'https://aka.ms/vs/17/release/vs_BuildTools.exe' -Out $Boot -MinBytes 1048576 }
    Log 'About 9 GB, 20-60 min. A VS Installer window with progress will open. Click Yes on the UAC prompt.' 'Yellow'
    $vsArgs = @(
        '--passive', '--wait', '--norestart', '--nocache',
        '--installPath', $VsDir,
        '--add', 'Microsoft.VisualStudio.Workload.VCTools',
        '--add', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
        '--includeRecommended'
    )
    $proc = Start-Process -FilePath $Boot -Verb RunAs -PassThru -Wait -ArgumentList $vsArgs
    if ($proc.ExitCode -notin @(0, 3010)) {
        if ($proc.ExitCode -eq 1603) {
            Log '1603 means the MSI rolled back. The usual cause is a security suite' 'Yellow'
            Log 'blocking writes to HKCR\CLSID\*\InprocServer32 (360 / Huorong / etc).' 'Yellow'
            Log 'Fully exit it from the tray icon - closing the window is not enough - then re-run.' 'Yellow'
            $detail = Get-ChildItem -Path $env:TEMP -Filter 'dd_setup_*.log' -ErrorAction SilentlyContinue |
                Sort-Object LastWriteTime -Descending | Select-Object -First 1
            if ($detail) { Log "latest setup log: $($detail.FullName)" 'DarkGray' }
        }
        throw "vs_BuildTools exited with code $($proc.ExitCode)"
    }
    if (-not (Test-Path $script:VcVars)) { throw "installer succeeded but $script:VcVars is missing" }
    Log 'VS Build Tools ready' 'Green'
}

Invoke-Step '7/7 PATH + verification' -Body {
    $UserPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    if ([string]::IsNullOrEmpty($UserPath)) { $UserPath = '' }
    $Add = @()
    if ($script:PyScripts -and (Test-Path $script:PyScripts)) { $Add += $script:PyScripts }
    if ($script:QtRoot  -and (Test-Path $script:QtRoot))      { $Add += $script:QtRoot }
    $Add = $Add | Where-Object { $UserPath -notlike ('*' + $_ + '*') }
    if ($Add) {
        [Environment]::SetEnvironmentVariable('Path', ($UserPath.TrimEnd(';') + ';' + ($Add -join ';')), 'User')
        Log ('PATH updated: ' + ($Add -join ' ; ')) 'Green'
    }
    if ($script:QtRoot) { [Environment]::SetEnvironmentVariable('QT_DIR', $script:QtRoot, 'User') }

    if ($script:CMakeExe) { & $script:CMakeExe --version | Select-Object -First 1 | ForEach-Object { Log "  $_" 'Green' } }
    if ($script:NinjaExe) { Log "  ninja $(& $script:NinjaExe --version)" 'Green' }
    if ($script:QtRoot -and (Test-Path (Join-Path $script:QtRoot 'bin\qmake.exe'))) { Log "  qmake   $script:QtRoot" 'Green' }
    else { Log '  qmake   not installed' 'Yellow' }
    if (-not $script:VcVars) { $script:VcVars = Join-Path $VsDir 'VC\Auxiliary\Build\vcvars64.bat' }
    if (Test-Path $script:VcVars) {
        # 2>&1 must live inside cmd: redirecting at the PowerShell level turns
        # native stderr into a terminating NativeCommandError under ErrorActionPreference Stop.
        $cl = (cmd /c ('"' + $script:VcVars + '" >NUL 2>&1 && cl 2>&1') | Select-Object -First 1)
        if ($cl) { Log "  cl      $cl" 'Green' } else { Log '  cl      vcvars ran but cl printed nothing' 'Yellow' }
    }
    else { Log '  cl      not installed - re-run with:  .\setup-toolchain.ps1 -SkipQt' 'Yellow' }
}

Log '' 'White'
Log '================ SUMMARY ================' 'White'
foreach ($key in $script:Results.Keys) {
    $val = $script:Results[$key]
    $color = if ($val -eq 'OK') { 'Green' } elseif ($val -eq 'SKIP') { 'Yellow' } else { 'Red' }
    if ($val.Length -gt 110) { $val = $val.Substring(0, 110) + '...' }
    Log ('  {0,-52} {1}' -f $key, $val) $color
}
Log "full log: $LogFile" 'DarkGray'

Write-Host @'

Re-run notes:
  everything again (completed steps are skipped)   .\setup-toolchain.ps1
  only the missing MSVC, Qt already installed      .\setup-toolchain.ps1 -SkipQt
  only Qt, skip the VS prompt                      .\setup-toolchain.ps1 -SkipVs

'@ -ForegroundColor White

Read-Host 'Press Enter to close' | Out-Null
