@echo off
setlocal
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\prepare-hd-wizard.ps1" %*
if errorlevel 1 (
  echo HD media preparation failed. The installed game, the disc copy and the saves were kept.
  pause
  exit /b 1
)
pause
