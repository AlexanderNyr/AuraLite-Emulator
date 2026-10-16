#!/usr/bin/env bash
# QEMU G3 diagnostic using the exact 128-KiB SeaBIOS bios.bin used by xemu.
# Run in a terminal; capture boot_info_t with capture_qemu_bootinfo.py after
# the relevant serial boundary, then stop QEMU (all attached disks use -snapshot).
set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "usage: $0 <iso> <prefix> <ide|ahci-usb>" >&2
    exit 2
fi
ISO="$1"
PREFIX="$2"
MODE="$3"
QEMU_ROOT="${QEMU_ROOT:-/tmp/g3-qemu-root}"
QEMU="$QEMU_ROOT/usr/bin/qemu-system-x86_64"
ROM="$QEMU_ROOT/usr/share/seabios/bios.bin"
BLANK_USB="${BLANK_USB:-$(dirname "$0")/blank-usb-1m.img}"

[[ -x "$QEMU" && -f "$ROM" && -f "$ISO" && -f "$BLANK_USB" ]] || {
    echo "missing QEMU root, stock SeaBIOS ROM, ISO, or blank USB image" >&2
    exit 2
}
mkdir -p "$(dirname "$PREFIX")" "$QEMU_ROOT/usr/share/qemu"
ln -sf ../seabios/vgabios-stdvga.bin "$QEMU_ROOT/usr/share/qemu/vgabios-stdvga.bin"
rm -f "$PREFIX.qmp"

common=(
    -L "$QEMU_ROOT/usr/share/qemu"
    -cpu Haswell -m 128M -bios "$ROM"
    -display none -serial stdio -monitor none
    -qmp "unix:$PREFIX.qmp,server=on,wait=off"
    -no-reboot -snapshot
)
case "$MODE" in
    ide)
        machine=(-machine q35,accel=tcg,sata=on)
        storage=(-drive "if=ide,format=raw,file=$ISO")
        ;;
    ahci-usb)
        machine=(-machine q35,accel=tcg,sata=off,usb=off
                 -device ich9-ahci,id=ahci -device qemu-xhci,id=xhci)
        storage=(-drive "if=none,id=iso,format=raw,file=$ISO"
                 -device ide-hd,drive=iso,bus=ahci.0,bootindex=1
                 -drive "if=none,id=blank,format=raw,file=$BLANK_USB"
                 -device usb-storage,drive=blank,bus=xhci.0)
        ;;
    *)
        echo "unknown mode: $MODE (expected ide or ahci-usb)" >&2
        exit 2
        ;;
esac

set -o pipefail
LD_LIBRARY_PATH="$QEMU_ROOT/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$QEMU" "${machine[@]}" "${common[@]}" "${storage[@]}" \
    2>"$PREFIX.stderr" | tee "$PREFIX.serial"
