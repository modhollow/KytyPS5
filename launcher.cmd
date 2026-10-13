@echo off
setlocal
rem Windows PowerShell rebuilds its own module path instead of inheriting PowerShell 7 modules.
set "PSModulePath="
rem The settings window (launcher.ps1): game folder, resolution, fullscreen, console language, precompile.
start "" powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0launcher.ps1"
