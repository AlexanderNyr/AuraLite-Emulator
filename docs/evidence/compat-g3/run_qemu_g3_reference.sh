#!/usr/bin/env bash
# Run a controlled QEMU BIOS reference. Start it as a background process,
# wait for the serial shell-active marker, then use capture_qemu_bootinfo.py
# on PREFIX.qmp. Stop QEMU after the memory capture.
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 <iso> <prefix>" >&2
    exit 2
fi
QEMU_ROOT="${QEMU_ROOT:-/tmp/g3-qemu-root}"
ISO="$1"
PREFIX="$2"
QEMU="$QEMU_ROOT/usr/bin/qemu-system-x86_64"
ROM="$QEMU_ROOT/usr/share/seabios/bios-256k.bin"

[[ -x "$QEMU" && -f "$ROM" && -f "$ISO" ]] || {
    echo "missing QEMU root, SeaBIOS ROM, or ISO" >&2
    exit 2
}
mkdir -p "$(dirname "$PREFIX")" "$QEMU_ROOT/usr/share/qemu"
ln -sf ../seabios/vgabios-stdvga.bin "$QEMU_ROOT/usr/share/qemu/vgabios-stdvga.bin"
rm -f "$PREFIX.qmp"

LD_LIBRARY_PATH="$QEMU_ROOT/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  "$QEMU" \
  -L "$QEMU_ROOT/usr/share/qemu" \
  -machine q35,accel=tcg -cpu Haswell -m 128M \
  -bios "$ROM" -display none -serial "file:$PREFIX.serial" -monitor none \
  -qmp "unix:$PREFIX.qmp,server=on,wait=off" \
  -drive "if=ide,format=raw,file=$ISO" -snapshot -no-reboot \
  >"$PREFIX.stdout" 2>"$PREFIX.stderr"
