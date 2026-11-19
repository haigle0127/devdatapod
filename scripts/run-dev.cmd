@echo off
rem Dev launcher: run the panel from target\debug against this repo as install root.
rem ASCII only (AGENTS.md rule 9).
set DEVDATAPOD_ROOT=%~dp0..
"%~dp0..\target\debug\devdatapod.exe"
