#!/usr/bin/env python3
"""tools/mkiso.py -- minimal ISO9660 + El Torito + MBR builder (stdlib only).

Builds one small, honest ISO image that boots through both media routes the
GUI offers:

  * attached as a disk/ISO on a SATA port  -- SeaBIOS reads the MBR at
    LBA 0 and jumps to it (the quick route, docs/WINDOWS.md E);
  * attached as an ATAPI CD (--cdrom=)     -- SeaBIOS reads the El Torito
    boot record (LBA 17), follows the catalog and loads the no-emulation
    boot image at 0x7c00 (STORE S6).

Layout (2048-byte LBAs):
    0        MBR: the boot stub (512 B) + 0x55AA
    1..15    system area (zeros)
    16       Primary Volume Descriptor
    17       El Torito boot record (catalog LBA inside)
    18       Volume Descriptor Set Terminator
    19       El Torito boot catalog (validation + default entry)
    20       El Torito boot image (the same boot stub, 2048 B)
    21/22    ISO9660 L/M path tables
    23       root directory
    24       README file
"""
import argparse
import struct
import sys

SECTOR = 2048
LBA_PVD = 16
LBA_BOOTREC = 17
LBA_TERM = 18
LBA_CATALOG = 19
LBA_BOOTIMG = 20
LBA_PATHL = 21
LBA_PATHM = 22
LBA_ROOT = 23
LBA_README = 24
TOTAL_SECTORS = 512  # 1 MiB: >= 2 LBA-translation cylinders (16*63 sectors each);
                     # a smaller image rounds LCHS cylinders to 0 and stock SeaBIOS
                     # refuses every CHS read (measured, tests/test_iso_boot.py).


def _both16(v):
    return struct.pack("<H", v) + struct.pack(">H", v)


def _both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


def _date17(text="2026101800000000"):
    return text.encode("ascii") + b"\x00"


def _pad_name(name):
    return name if name.endswith(b";1") else name + b";1"


def _dir_record(name_bytes, lba, size, is_dir, date):
    name_len = len(name_bytes)
    rec_len = 33 + name_len
    if rec_len & 1:
        rec_len += 1
    rec = bytearray(rec_len)
    rec[0] = rec_len
    rec[1] = 0
    rec[2:10] = _both32(lba)
    rec[10:18] = _both32(size)
    rec[18:25] = date[:7]
    rec[25] = 0x02 if is_dir else 0x00
    rec[26] = 0
    rec[27] = 0
    rec[28:32] = _both16(1)
    rec[32] = name_len
    rec[33:33 + name_len] = name_bytes
    return bytes(rec)


def _cat_checksum(entry32):
    """Validate-entry checksum: sum of the 16 LE words must be zero."""
    words = list(struct.unpack("<16H", entry32))
    words[14] = 0
    return (-sum(words)) & 0xFFFF


def build_iso(boot_stub, readme=b"AuraLite-Emulator test ISO (tools/mkiso.py)\n"):
    """Return the complete ISO image as bytes.  boot_stub must be 512 bytes."""
    if len(boot_stub) != 512:
        raise ValueError("boot stub must be exactly 512 bytes")
    if boot_stub[510:512] != b"\x55\xaa":
        raise ValueError("boot stub must carry the 0x55AA boot signature")
    date = _date17()

    img = bytearray(TOTAL_SECTORS * SECTOR)

    # -- LBA 0: the MBR half of the isohybrid trick ----------------------
    img[0:512] = boot_stub

    # -- LBA 16: Primary Volume Descriptor --------------------------------
    pvd = bytearray(SECTOR)
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    pvd[6] = 1
    pvd[8:40] = b"LINUX".ljust(32)
    pvd[40:72] = b"AURALITE_TEST".ljust(32)
    pvd[80:88] = _both32(TOTAL_SECTORS)
    pvd[120:124] = _both16(1)                 # volume set size
    pvd[124:128] = _both16(1)                 # volume sequence number
    pvd[128:132] = _both16(SECTOR)            # logical block size
    pvd[132:140] = _both32(10)                # path table size
    pvd[140:144] = struct.pack("<I", LBA_PATHL)
    pvd[148:152] = struct.pack(">I", LBA_PATHM)
    root_size = 3 * 34 + 42                   # ".", "..", README + terminator
    pvd[156:190] = _dir_record(b"\x00", LBA_ROOT, root_size, True, date[:7])
    pvd[318:446] = b"AURALITE".ljust(128)
    pvd[446:574] = b"AURALITE".ljust(128)
    pvd[881:898] = _date17()
    pvd[898:915] = _date17()
    pvd[915:932] = b"0" * 16 + b"\x00"        # never expires
    pvd[932:949] = _date17()
    pvd[949] = 1                              # file structure version
    img[LBA_PVD * SECTOR:(LBA_PVD + 1) * SECTOR] = pvd

    # -- LBA 17: El Torito boot record ------------------------------------
    br = bytearray(SECTOR)
    br[0] = 0
    br[1:6] = b"CD001"
    br[6] = 1
    br[7:39] = b"EL TORITO SPECIFICATION".ljust(32, b"\x00")
    br[0x47:0x4B] = struct.pack("<I", LBA_CATALOG)
    img[LBA_BOOTREC * SECTOR:(LBA_BOOTREC + 1) * SECTOR] = br

    # -- LBA 18: terminator ------------------------------------------------
    term = bytearray(SECTOR)
    term[0] = 255
    term[1:6] = b"CD001"
    term[6] = 1
    img[LBA_TERM * SECTOR:(LBA_TERM + 1) * SECTOR] = term

    # -- LBA 19: boot catalog ---------------------------------------------
    cat = bytearray(SECTOR)
    val = bytearray(32)
    val[0] = 0x01                            # validation entry
    val[1] = 0x00                            # platform: x86
    val[30] = 0x55
    val[31] = 0xAA
    struct.pack_into("<H", val, 28, _cat_checksum(bytes(val)))
    cat[0:32] = val
    ent = bytearray(32)
    ent[0] = 0x88                            # bootable
    ent[1] = 0x00                            # no emulation
    ent[2:4] = struct.pack("<H", 0)          # load segment
    ent[4] = 0                               # system type
    ent[6:8] = struct.pack("<H", 4)          # virtual sector count (2048 B)
    ent[8:12] = struct.pack("<I", LBA_BOOTIMG)   # Load RBA, little-endian
    cat[32:64] = ent
    img[LBA_CATALOG * SECTOR:(LBA_CATALOG + 1) * SECTOR] = cat

    # -- LBA 20: boot image (the stub, zero-padded to one sector) ----------
    img[LBA_BOOTIMG * SECTOR:LBA_BOOTIMG * SECTOR + 512] = boot_stub

    # -- LBA 21/22: path tables -------------------------------------------
    pt = bytearray(10)
    pt[0] = 1
    pt[1] = 0
    pt[2:6] = struct.pack("<I", LBA_ROOT)
    pt[6:8] = struct.pack("<H", 1)
    img[LBA_PATHL * SECTOR:LBA_PATHL * SECTOR + 10] = pt
    ptm = bytearray(pt)
    ptm[2:6] = struct.pack(">I", LBA_ROOT)
    ptm[6:8] = struct.pack(">H", 1)
    img[LBA_PATHM * SECTOR:LBA_PATHM * SECTOR + 10] = ptm

    # -- LBA 23: root directory -------------------------------------------
    root = bytearray()
    root += _dir_record(b"\x00", LBA_ROOT, root_size, True, date[:7])
    root += _dir_record(b"\x01", LBA_ROOT, root_size, True, date[:7])
    root += _dir_record(_pad_name(b"README"), LBA_README, len(readme),
                        False, date[:7])
    img[LBA_ROOT * SECTOR:LBA_ROOT * SECTOR + len(root)] = root

    # -- LBA 24: README ----------------------------------------------------
    img[LBA_README * SECTOR:LBA_README * SECTOR + len(readme)] = readme
    return bytes(img)


def main(argv=None):
    ap = argparse.ArgumentParser(description="build the AuraLite test ISO")
    ap.add_argument("--bootstub", required=True,
                    help="512-byte boot stub binary (tests/fixtures/bootstub.nas)")
    ap.add_argument("-o", "--output", required=True)
    a = ap.parse_args(argv)
    with open(a.bootstub, "rb") as f:
        stub = f.read()
    iso = build_iso(stub)
    with open(a.output, "wb") as f:
        f.write(iso)
    print("mkiso: %s (%d bytes, %d sectors)" % (a.output, len(iso), len(iso) // SECTOR))
    return 0


if __name__ == "__main__":
    sys.exit(main())
