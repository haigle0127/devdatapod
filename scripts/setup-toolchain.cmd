@echo off
rem devdatapod toolchain installer launcher. Keep this file pure ASCII.
rem Runs the PowerShell script in THIS window so output stays visible,
rem and pauses at the end so nothing can flash past unreadable.
title devdatapod toolchain setup
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup-toolchain.ps1" %*
echo.
echo If the window closed instantly, the .ps1 failed to parse.
echo Instead run: powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup-toolchain.ps1"
pause
