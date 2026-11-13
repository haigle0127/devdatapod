@echo off
rem devdatapod P0 toolchain acceptance launcher. Keep this file pure ASCII.
setlocal
title devdatapod P0 acceptance
set "ROOT=%~dp0"
set "LOGDIR=%ROOT%..\logs"
if not exist "%LOGDIR%" mkdir "%LOGDIR%"
echo launcher: starting powershell >> "%LOGDIR%\launcher.log"
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%verify-toolchain.ps1" -NoPause %*
set "RC=%ERRORLEVEL%"
echo launcher: powershell returned %RC% >> "%LOGDIR%\launcher.log"
echo.
echo  exit code   = %RC%
echo  script log  = %LOGDIR%\verify-*.log
echo  launcher log= %LOGDIR%\launcher.log
echo.
echo  No output at all above means powershell.exe was blocked or killed from
echo  outside. Check whether launcher.log has one line or two, and look at
echo  your security suite quarantine list.
echo  Files-only pass:  verify-toolchain.cmd -Stage files
echo.
pause
