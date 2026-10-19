# Building and Running AuraLite-Emulator on Windows 10 / 11

`AuraLite-Emulator` supports two ways to build and run on Windows 10/11:

1. **Native Windows x86_64 (`build.bat` + MinGW-w64 GCC)** — produces standalone `x86emu.exe` and `x86dis.exe` binaries using the zero-intrusion POSIX/Winsock2/ELF compatibility layer under `win32/`.
2. **WSL2 (`build.bat wsl` or `make` inside Ubuntu on Windows)** — runs the full Linux build, sanitizer, benchmark, and shell-based integration test suite unmodified.

---

## Option 1: Native Windows 10 Build (`build.bat` + MinGW-w64)

### 1. Prerequisites

Install **MinGW-w64 GCC** (and optionally **NASM** and **Python 3**):

- **Via WinGet (Windows 10/11 Command Prompt or PowerShell):**
  ```bat
  winget install -e --id BrechtSanders.WinLibs.POSIX.UCRT
  winget install -e --id NASM.NASM
  winget install -e --id Python.Python.3.12
  ```
- **Or via MSYS2 (`https://www.msys2.org/`):**
  In the **MSYS2 UCRT64** (or **MINGW64**) shell:
  ```sh
  pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-nasm python
  ```
  Add `C:\msys64\ucrt64\bin` to your Windows `PATH`.

Verify in `cmd.exe`:
```bat
gcc --version
nasm -v
python --version
```

### 2. Building with `build.bat`

From `cmd.exe` or PowerShell in the repository root:

```bat
REM Build x86emu.exe, x86dis.exe, and (if nasm is in PATH) firmware\firmware.bin + disk\disk.img
build.bat

REM Explicitly rebuild firmware\firmware.bin and disk\disk.img (1 MiB)
build.bat firmware

REM Run the full native Windows test suite (20 C test binaries + G3/G5/G6 acceptance gates)
build.bat test

REM Clean build artifacts
build.bat clean
```

### 3. Running `x86emu.exe` on Windows 10

#### A. Sample Firmware Demo (16-bit Real Mode → 32-bit Protected → 64-bit Long Mode + USB Boot)
```bat
build.bat demo
```
Or directly:
```bat
.\x86emu.exe --platform=haswell --max-instr=200000 --dump-fb=framebuffer.ppm --stats
```

#### B. Live Browser Monitor (`http://127.0.0.1:8080/`)
```bat
build.bat monitor
```
Then open **`http://127.0.0.1:8080/`** in Chrome/Edge/Firefox to inspect registers, memory, VGA/VBE framebuffer, event log, breakpoints, and step/continue execution in real time.

#### C. Interactive CLI Debugger REPL (`--debug`)
```bat
.\x86emu.exe --platform=haswell --debug --max-instr=1000000
```
Common REPL commands at `(x86dbg)`:
- `regs`, `sregs`, `cregs`, `vcpus`, `pic`, `ioapic`, `lapic`, `ahci`, `e1000`
- `u 0xfffffff0 10` — disassemble 10 instructions
- `x/16bx 0x7c00` — examine memory
- `b 0x7c00`, `c`, `s`, `si 10`, `bt`, `savevm snap.bin`, `loadvm snap.bin`, `q`

#### D. GDB Remote Debugging (`--gdb=1234`)
In terminal 1:
```bat
.\x86emu.exe --platform=haswell --gdb=127.0.0.1:1234 --max-instr=10000000
```
In terminal 2:
```bat
gdb -ex "set architecture i386:x86-64" -ex "target remote 127.0.0.1:1234"
```

#### E. Full BIOS Boot

> The desktop GUI automates this exact recipe: `gui.bat` (or
> `build.bat gui`) -> «Обычный» -> create machine -> pick the ISO ->
> «Пуск» (see README «GUI»).  The manual command below remains the
> reference. (`SeaBIOS → Stage 2 → AuraLite-OS Kernel Shell`, 1 or 4 vCPUs)
With `bios.bin` (SeaBIOS 1.16.3, 128 KiB) and `auralite.iso`:
```bat
REM Uniprocessor BIOS boot with ICH9 ACPI + PM timer:
.\x86emu.exe --rom=bios.bin --lpc=ich9 ^
  --disk=docs\evidence\compat-g3\blank-usb-1m.img ^
  --sata=auralite.iso --max-instr=1000000000 --stats

REM 4-vCPU SMP BIOS boot + guest SMP test commands injected at shell prompt:
.\x86emu.exe --rom=bios.bin --lpc=ich9 --smp=4 ^
  --disk=docs\evidence\compat-g3\blank-usb-1m.img ^
  --sata=auralite.iso ^
  --keys-at="auralite$ :/tests/irqapwake\n/tests/smpstress\n" ^
  --max-instr=4000000000 --stats
```

#### F. Direct ELF64 Kernel Boot (`--kernel=`, bypassing BIOS)
```bat
.\x86emu.exe --kernel=kernel.elf --initrd=initrd.tar --cpus=4 --smp=4 --max-instr=500000000 --stats
```

#### G. Standalone Disassembler (`x86dis.exe`)
```bat
.\x86dis.exe -b 16 -o 0xfff00000 firmware\firmware.bin
```

---

## Option 2: WSL2 Build and Full CI Gate Suite

If WSL2 (Ubuntu/Debian) is installed on Windows 10/11:

```bat
REM Build Linux binary via WSL2
build.bat wsl

REM Run the complete Linux test suite (unit + integration + COMPAT G3/G5/G6)
build.bat wsl test

REM Run the ASan + UBSan sanitizer suite
build.bat wsl test-sanitize
```
