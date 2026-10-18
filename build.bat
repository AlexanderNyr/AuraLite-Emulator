@echo off
setlocal EnableExtensions

REM ============================================================================
REM build.bat -- Windows 10/11 build, test, and run script for AuraLite-Emulator
REM
REM Supports two toolchains on Windows:
REM   1. Native Windows x86_64 build via MinGW-w64 GCC (MSYS2 / WinLibs / w64devkit)
REM      using the POSIX socket/poll/ELF compatibility layer in win32\.
REM   2. WSL2 delegation (`build.bat wsl [make-target]`).
REM ============================================================================

cd /d "%~dp0"

set "TARGET=%~1"
if "%TARGET%"=="" set "TARGET=all"

if /I "%TARGET%"=="help"     goto :help
if /I "%TARGET%"=="--help"   goto :help
if /I "%TARGET%"=="-h"       goto :help
if /I "%TARGET%"=="/?"       goto :help
if /I "%TARGET%"=="clean"    goto :clean
if /I "%TARGET%"=="wsl"      goto :wsl
if /I "%TARGET%"=="all"      goto :build_all
if /I "%TARGET%"=="firmware" goto :build_firmware
if /I "%TARGET%"=="demo"     goto :run_demo
if /I "%TARGET%"=="monitor"  goto :run_monitor
if /I "%TARGET%"=="test"     goto :run_tests

echo [build.bat] Unknown command: "%TARGET%"
goto :help

REM ----------------------------------------------------------------------------
REM Compiler detection
REM ----------------------------------------------------------------------------
:detect_cc
if defined CC goto :cc_ok
where gcc >nul 2>nul
if not errorlevel 1 (
    set "CC=gcc"
    goto :cc_ok
)
where x86_64-w64-mingw32-gcc >nul 2>nul
if not errorlevel 1 (
    set "CC=x86_64-w64-mingw32-gcc"
    goto :cc_ok
)
where wsl >nul 2>nul
if not errorlevel 1 (
    echo [build.bat] Native MinGW-w64 gcc not found in PATH; delegating to WSL...
    wsl make %TARGET%
    exit /b %ERRORLEVEL%
)
echo [build.bat] ERROR: Neither MinGW-w64 gcc nor WSL was found in PATH.
echo             Install MSYS2 / WinLibs MinGW-w64 or WSL2 (see docs\WINDOWS.md).
exit /b 1

:cc_ok
set "CFLAGS=-std=c11 -O2 -Wall -Wextra -Werror -D__USE_MINGW_ANSI_STDIO=1"
set "CPPFLAGS=-Iwin32/include -Isrc -include win32/win32_compat.h"
set "CORE_SRC=src/cpu.c src/mem.c src/io.c src/platform.c src/pic.c src/pit.c src/pm.c src/rtc.c src/kbc.c src/chipset.c src/lapic.c src/ioapic.c win32/win32_compat.c"
set "DEV_SRC=src/ahci.c src/devices.c src/e1000.c src/pci.c src/serial.c %CORE_SRC%"
set "EMU_SRC=src/acpi.c src/kloader.c src/disasm.c src/dbg.c src/gdbstub.c src/snapshot.c src/ws.c src/monitor.c src/main.c %DEV_SRC%"
set "LIBS=-lws2_32 -lm"
exit /b 0

REM ----------------------------------------------------------------------------
REM Build x86emu.exe, x86dis.exe, and optional firmware/disk images
REM ----------------------------------------------------------------------------
:build_all
call :detect_cc
if errorlevel 1 exit /b 1

echo [build.bat] Compiling x86emu.exe with %CC%...
%CC% %CFLAGS% %CPPFLAGS% %EMU_SRC% -o x86emu.exe %LIBS%
if errorlevel 1 (
    echo [build.bat] ERROR: x86emu.exe build failed.
    exit /b 1
)

echo [build.bat] Compiling x86dis.exe with %CC%...
%CC% %CFLAGS% %CPPFLAGS% tools/x86dis.c src/disasm.c win32/win32_compat.c -o x86dis.exe -lws2_32
if errorlevel 1 (
    echo [build.bat] ERROR: x86dis.exe build failed.
    exit /b 1
)

echo [build.bat] Built x86emu.exe and x86dis.exe successfully.

where nasm >nul 2>nul
if not errorlevel 1 (
    call :build_firmware
) else (
    echo [build.bat] Note: 'nasm' not in PATH ^(skipping firmware\firmware.bin rebuild^).
)
exit /b 0

REM ----------------------------------------------------------------------------
REM Build sample firmware & 1 MiB disk image
REM ----------------------------------------------------------------------------
:build_firmware
where nasm >nul 2>nul
if errorlevel 1 (
    echo [build.bat] ERROR: 'nasm' is required to assemble firmware\sample_firmware.nas.
    exit /b 1
)
echo [build.bat] Assembling firmware\firmware.bin...
nasm -f bin firmware/sample_firmware.nas -o firmware/firmware.bin
if errorlevel 1 exit /b 1

echo [build.bat] Assembling disk\test_kernel.bin...
nasm -f bin disk/test_kernel.asm -o disk/test_kernel.bin
if errorlevel 1 exit /b 1

call :detect_python
if defined PY_CMD (
    echo [build.bat] Creating disk\disk.img ^(1 MiB^)...
    %PY_CMD% -c "k=open('disk/test_kernel.bin','rb').read(); open('disk/disk.img','wb').write(k+b'\0'*(1024*1024-len(k)))"
    if errorlevel 1 exit /b 1
) else (
    echo [build.bat] Note: Python 3 not found; copying disk\test_kernel.bin to disk\disk.img.
    copy /y disk\test_kernel.bin disk\disk.img >nul
)
exit /b 0

:detect_python
set "PY_CMD="
where python >nul 2>nul
if not errorlevel 1 (
    set "PY_CMD=python"
    exit /b 0
)
where python3 >nul 2>nul
if not errorlevel 1 (
    set "PY_CMD=python3"
    exit /b 0
)
where py >nul 2>nul
if not errorlevel 1 (
    set "PY_CMD=py -3"
    exit /b 0
)
exit /b 0

REM ----------------------------------------------------------------------------
REM Demo & Live Monitor targets
REM ----------------------------------------------------------------------------
:run_demo
if not exist x86emu.exe call :build_all
if errorlevel 1 exit /b 1
if not exist firmware\firmware.bin call :build_firmware
if errorlevel 1 exit /b 1
echo [build.bat] Running sample firmware demo...
.\x86emu.exe --platform=haswell --max-instr=200000 --dump-fb=framebuffer.ppm --stats
exit /b %ERRORLEVEL%

:run_monitor
if not exist x86emu.exe call :build_all
if errorlevel 1 exit /b 1
if not exist firmware\firmware.bin call :build_firmware
if errorlevel 1 exit /b 1
echo [build.bat] Starting live browser monitor on http://127.0.0.1:8080/ ...
echo             Open http://127.0.0.1:8080/ in your browser.
.\x86emu.exe --platform=haswell --max-instr=5000000 --monitor=127.0.0.1:8080 --debug
exit /b %ERRORLEVEL%

REM ----------------------------------------------------------------------------
REM Unit, Tooling, and COMPAT acceptance tests
REM ----------------------------------------------------------------------------
:run_tests
call :detect_cc
if errorlevel 1 exit /b 1
if not exist x86emu.exe call :build_all
if errorlevel 1 exit /b 1

echo [build.bat] Building and running unit test suite...

call :compile_and_run test-cpu           "tests/test_cpu.c %CORE_SRC%" || exit /b 1
call :compile_and_run test-usb           "tests/test_usb.c %DEV_SRC%" || exit /b 1
call :compile_and_run test-pam           "tests/test_pam.c %DEV_SRC%" || exit /b 1
call :compile_and_run test-video         "tests/test_video.c %DEV_SRC%" || exit /b 1
call :compile_and_run test-net           "tests/test_net.c %DEV_SRC%" || exit /b 1
call :compile_and_run test-pic           "tests/test_pic.c %CORE_SRC%" || exit /b 1
call :compile_and_run test-pit           "tests/test_pit.c %CORE_SRC%" || exit /b 1
call :compile_and_run test-pm            "tests/test_pm.c src/pci.c %CORE_SRC%" || exit /b 1
call :compile_and_run test-rtc           "tests/test_rtc.c %CORE_SRC%" || exit /b 1
call :compile_and_run test-kbc           "tests/test_kbc.c %CORE_SRC%" || exit /b 1
call :compile_and_run test-serial        "tests/test_serial.c src/serial.c %CORE_SRC%" || exit /b 1
call :compile_and_run test-chipset       "tests/test_chipset.c %CORE_SRC%" || exit /b 1
call :compile_and_run test-lapic         "tests/test_lapic.c %CORE_SRC%" || exit /b 1
call :compile_and_run test-ioapic        "tests/test_ioapic.c %CORE_SRC%" || exit /b 1
call :compile_and_run test-acpi          "tests/test_acpi.c src/acpi.c win32/win32_compat.c" || exit /b 1
call :compile_and_run test-ahci          "tests/test_ahci.c src/ahci.c src/pci.c %CORE_SRC%" || exit /b 1
call :compile_and_run test-compat        "tests/test_compat.c %DEV_SRC%" || exit /b 1
call :compile_and_run test-disasm        "tests/test_disasm.c src/dbg.c src/disasm.c src/snapshot.c src/kloader.c src/acpi.c %DEV_SRC%" || exit /b 1
call :compile_and_run test-dbg           "tests/test_dbg.c src/dbg.c src/disasm.c src/snapshot.c src/kloader.c src/acpi.c %DEV_SRC%" || exit /b 1
call :compile_and_run test-monitor-codec "tests/test_monitor_codec.c src/ws.c win32/win32_compat.c" || exit /b 1

call :detect_python
if defined PY_CMD (
    echo [build.bat] Running COMPAT G3/G5/G6 acceptance gates...
    %PY_CMD% tests/test_g3_acceptance.py || exit /b 1
    %PY_CMD% tests/test_g5_acceptance.py || exit /b 1
    %PY_CMD% tests/test_g6_acceptance.py || exit /b 1
) else (
    echo [build.bat] Note: Python 3 not found ^(skipping test_g3/g5/g6_acceptance.py^).
)

echo [build.bat] ALL WINDOWS TESTS PASSED.
exit /b 0

:compile_and_run
set "TNAME=%~1"
set "TSRC=%~2"
echo   [TEST] %TNAME%
%CC% %CFLAGS% %CPPFLAGS% %TSRC% -o %TNAME%.exe %LIBS%
if errorlevel 1 exit /b 1
.\%TNAME%.exe
if errorlevel 1 (
    del /q %TNAME%.exe 2>nul
    exit /b 1
)
del /q %TNAME%.exe 2>nul
exit /b 0

REM ----------------------------------------------------------------------------
REM WSL delegation
REM ----------------------------------------------------------------------------
:wsl
set "WSL_TARGET=%~2"
if "%WSL_TARGET%"=="" set "WSL_TARGET=all"
echo [build.bat] Running 'wsl make %WSL_TARGET%'...
wsl make %WSL_TARGET%
exit /b %ERRORLEVEL%

REM ----------------------------------------------------------------------------
REM Clean
REM ----------------------------------------------------------------------------
:clean
echo [build.bat] Cleaning build artifacts...
del /q x86emu.exe x86dis.exe test-*.exe framebuffer.ppm fb.ppm firmware\firmware.bin disk\test_kernel.bin disk\disk.img 2>nul
exit /b 0

REM ----------------------------------------------------------------------------
REM Help
REM ----------------------------------------------------------------------------
:help
echo Usage: build.bat [command]
echo.
echo Commands:
echo   all        Build x86emu.exe, x86dis.exe, and ^(if nasm is installed^) firmware/disk images ^(default^)
echo   firmware   Assemble firmware\firmware.bin, disk\test_kernel.bin, and disk\disk.img
echo   demo       Build and run the Haswell sample firmware demo ^(writes framebuffer.ppm^)
echo   monitor    Build and start the live HTTP/WebSocket monitor on http://127.0.0.1:8080/
echo   test       Build and run the C unit/tooling tests + Python G3/G5/G6 acceptance gates
echo   wsl [tgt]  Run `make [tgt]` inside WSL2 ^(e.g. `build.bat wsl test`^)
echo   clean      Remove built .exe binaries and generated images
echo   help       Show this help message
exit /b 0
