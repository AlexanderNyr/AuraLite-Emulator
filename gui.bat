@echo off
REM gui.bat -- double-click to open the AuraLite Emulator desktop application.
REM Machines, ISO boot, live screen, snapshots and artifacts all happen in the
REM window -- no command line and no browser needed (Normal / Expert modes).
cd /d "%~dp0"
call build.bat gui
