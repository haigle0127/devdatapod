@echo off
rem devdatapod build launcher. Keep this file pure ASCII (AGENTS.md rule 9).
setlocal
title devdatapod build
set "ROOT=%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%build.ps1" %*
set "RC=%ERRORLEVEL%"
echo.
echo  exit code   = %RC%
echo  transcript  = %ROOT%..\logs\build-*.log
echo.
echo  Nothing at all above means powershell.exe was blocked - check the
echo  security suite quarantine list, same as the P0 setup incident.
echo.
echo  Common invocations:
echo    build.cmd                  configure + build (Debug)
echo    build.cmd -SelfTest        then run the P2 invariant checks
echo    build.cmd -Run             then launch build\devdatapod.exe
echo    build.cmd -Stage Extensions\redis3.0.504, Extensions\redis7.2.5
echo                               register staged components in data\services.json
echo    build.cmd -Clean           throw away build\ first
echo    build.cmd -Config Release
echo.
pause
