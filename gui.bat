@echo off
REM gui.bat -- double-click to open the AuraLite-Emulator GUI in your browser.
REM Everything (machine setup, start/stop, live view, snapshots, artifacts)
REM happens in the browser at http://127.0.0.1:8080/ -- no command line needed.
cd /d "%~dp0"
call build.bat gui
