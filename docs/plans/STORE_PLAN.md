# STORE Plan — real AHCI storage for the emulator

> House style (AuraLite-OS/docs/plans): dependency-ordered phases, each with
> a definition of done and a test gate, shipping as **one `.patch` per
> phase** under `patches/`. Everything below is measured against the trees,
> not assumed. Baseline measured 2026-10-07 on
> `AuraLite-Emulator@b00c4d9` (== upstream tip, K7-K8 absorbed) with
> `AuraLite-OS@0ed0d29`.

**Status: in progress — S0 ✅, S1 ✅, S2 ✅, S3 ✅, S4–S5 planned 📋**

| Phase | Scope | Status | Deliverable |
|---|---|---|---|
| S0 | Baseline measurement + this plan | ✅ done | — |
| S1 | HBA registers + disk presence: BAR5/ABAR MMIO @0xFEB10000, CAP/PI/VS/GHC, port register files, `--sata=`/`--sata-portN=` CLI, COMRESET path, stop/start handshake | ✅ done | `patches/0031-STORE-S1-ahci-hba.patch` |
| S2 | DMA read: command engine (CL/TBL/CFIS/PRDT), READ DMA EXT, IDENTIFY DEVICE, read-only guest path, TFES error channel, `--keys-at=prompt` testability | ✅ done | `patches/0032-STORE-S2-ahci-read.patch` |
| S3 | DMA write: WRITE DMA EXT, guest `/disk` + `/fat` write→read round-trip (QEMU `test_ahci_rw.sh` parity) | ✅ done | `patches/0033-STORE-S3-ahci-write.patch` |
| S4 | Breadth: up to 4 disks, port placement, second controller (QEMU `test_ahci_matrix.sh` parity) | 📋 | `patches/0034-STORE-S4-ahci-matrix.patch` |
| S5 | Large transfers + hardening (QEMU `test_ahci_large_read.sh` parity), IRQ honesty, determinism, docs | 📋 | `patches/0035-STORE-S5-ahci-hardening.patch` |

## 1. Measured baseline

### 1.1 What the machine presents today

`src/devices.c` (303 lines, mostly the behavioural EHCI block) registers a
SATA/AHCI **PCI stub** at the Intel-conventional slot, and nothing else:

```
pci_add_device(m, 0,31,2, "sata-ahci", 0x8086, 0x1E03, 0x01, 0x06, 1);
```

Class 0x01/0x06/0x01 is what the guest wants — but the stub has **no BAR5**
and no register model behind it. There is no `ahci.c` in the tree and no
disk-image plumbing (`--disk=` does not exist; `initrd.tar` is loaded by the
kernel loader, not through storage).

### 1.2 What the guest concludes from that (measured boot receipt)

From a full UP boot of the emulator at the baseline (serial capture,
`~/k8/runUP.log`):

```
[vmdrv] PCI 0:31.2 8086:1e03 class=01/06/01 SATA/AHCI storage
[boot] initialising AHCI SATA driver...
[ahci] controller 0 at PCI 0:31.2
[ahci] controller 0: BAR5 empty, skipping
[ahci] no AHCI controller found
[ahci] self-test: no devices
[diskfs] no AHCI disk available; /disk not mounted
[fat32] no AHCI disk; FAT32 disabled
[ext2] no second AHCI disk; /ext2 not mounted
[exfat] no 3rd AHCI disk; /exfat not mounted
[ext4] no 4th AHCI disk; /ext4 not mounted
```

The whole guest storage subsystem is offline: diskfs, FAT32, ext2, exFAT,
ext4 — all gated on finding at least one AHCI controller port with a disk.

### 1.3 The guest's AHCI contract (read from `AuraLite-OS/drivers/ahci/ahci.c`, 731 lines)

The driver binds **every** PCI function with class 0x01/0x06 (multi
controller; `ctrl_count`, `port_hw[]`, `port_abar[]`, flat public port
numbers up to `AHCI_MAX_PORTS=32`). Per controller it requires:

- `pci_get_bar(...,5) & ~0xF != 0` — a 32-bit MMIO BAR5 (ABAR); it maps 8 KiB.
- ABAR global regs: `CAP@0x00` (`NP = (cap & 0x1f) + 1` — only field used),
  `GHC@0x04` (kernel sets AE bit 31), `PI@0x0C` (implemented-port mask),
  `VS@0x10` (informational).
- Per implemented port at `0x100 + 0x80*port`:
  `PxSSTS@0x28` (`DET` nibble: **3** = disk present; **1** = PHY down →
  driver runs COMRESET through `PxSCTL@0x2C` DET=1/DET=0 and then requires
  DET==3), `PxSIG@0x24 == 0x00000101` to accept the device as a SATA disk,
  `PxCLB/CLBU@0x00/0x04`, `PxFB/FBU@0x08/0x0C` (guest writes frames),
  `PxCMD@0x18` (guest drives ST b0 / FRE b4; waits for CR b15 / FR b14),
  `PxIS@0x10` + `PxSERR@0x30` W1C, `PxIE@0x14`,
  `PxTFD@0x20` (polled: `BSY|DRQ` mask 0x88 must be 0; b0 = error),
  `PxCI@0x38` (guest writes 1 → **polls** until the machine clears it).
- Command engine: guest builds cmd header (CFL=5, W bit 6 for writes,
  PRDTL=1), a command table with an H2D Register FIS (type 0x27, device
  0x40 LBA mode) and one PRDT entry (`dbc = bytes-1`, `buf_len <= 4 MiB`,
  guest hosts a 128-KiB per-port bounce buffer as its largest transfer).
- **Completion is polled, never interrupt-driven** (`PxIE = 0`; the kernel
  comment documents it), so correctness of `PxCI` clearing / `PxTFD` /
  `PxIS.TFES` (b30) is the entire contract. IRQ wiring is deferred to S5.
- ATA opcodes used by the guest: `0x25` READ DMA EXT, `0x35` WRITE DMA EXT
  (LBA48). No IDENTIFY from this driver — the machine still gets one in S2
  for machine-side honesty (firmware stages probe disks by IDENTIFY).

Boot self-test (`ahci_self_test`): reads LBA 0; blank LBA 0 → skip write
verify; otherwise writes sector 1 `"AURALAHCI-WRITE"+0x55AA`, reads back,
restores the original if a partition table is detected →
`[ahci] PASS: SATA read/write DMA works`. Unformatted disks are formatted
by the guest itself (`diskfs` + FAT32) at first mount, so our test images
can be `dd`-blank.

### 1.4 The guest's own QEMU reference gates (what parity means)

`AuraLite-OS/tests/integration/cases/` (they pass on QEMU; grep-strings are
the assertions):

- `test_ahci_rw.sh` — shell `write /disk/ci.txt <token>` + `cat` back, same
  for `/fat/CI.TXT`; asserts `[ahci] .* SATA device`,
  `[ahci] PASS: SATA read/write DMA`, `[diskfs] PASS:`, `[fat32] PASS:`,
  token round-trips, and **no** `[ahci]/[diskfs]/[fat32] FAIL` lines; plus
  a blank-16 MiB lane that must stay error-free.
- `test_ahci_matrix.sh` — lane A one disk on port 0; lane B disk on port 1
  (empty port 0 skipped by enumeration); lane C two `-device ahci`, one disk
  each (multi-controller scan).
- `test_ahci_large_read.sh` — large-file read through the 128-KiB bounce
  path.

Machine-side gaps compared to that reference: no ABAR, no ports, no command
engine, no disk images, no second controller, no CLI for any of it.

## 2. Phases

### S1 — HBA registers + disk presence

New `src/ahci.c` with the register model; BAR5 assigned (fixed MMIO window,
colocated with the other fixed BARs: EHCI 0xFEB00000, VRAM 0xD0000000, CAR
0xFEF00000 → **ABAR 0xFEB10000, 4 KiB**); `mem_register_mmio_owned`.
Global regs: `CAP` (NP=6 ⇒ bits 4:0 = 5, plus 64-bit-addressing and
AHCI-only bits set), `PI = 0x3F`, `VS = 0x00010300`, `GHC` R/W with AE.
Port file `0x100+0x80*p`, p<6: `PxSSTS` reports DET=3 when a disk image is
attached to that port (DET=0 otherwise), `PxSIG = 0x00000101` on present
ports, `PxSCTL` accepts the DET=1/DET=0 COMRESET pulse (detached→DET=3 with
the guest's timing loops), `PxCMD` ST/FRE writable, CR/FR mirror the held
ST/FRE after an immediate state change, `PxIS/PxSERR` W1C, `PxIE` store.
CLI: `--disk=path` raw image (default port 0), `--disk-portN=path` for
S4. Unit tests in `tests/test_ahci.c` for every register behaviour above.

(S1 DoD wording refined while shipping: with a populated BAR5 the guest
necessarily binds the controller even without a disk, so the "exact S0"
shape was impossible-by-construction and is restated as the receipts below.)

**DoD (measured, both captured):** unattached boot:
`[ahci] controller 0 at PCI 0:31.2` + `[ahci] version=0x10300,
CAP=0x80040005 (NP=6), PI=0x0000003f` + `1 controller(s), 0 SATA device(s)
ready` + `self-test: no devices` + all mount-skips unchanged → `auralite#`.
Attached (`--sata=` blank 16 MiB): `[ahci] hw port 0: SATA disk
(sig=0x00000101)` + `1 controller(s), 1 SATA device(s) ready` + self-test
`reading sector 0...` → machine `[ahci] port 0: PxCI=0x1 latched (command
engine pending: S2)` → guest `port 0: timeout ... TFD=0x40` + `FAIL:
sector read not yet functional` (the documented S1 receipt: reads are S2)
→ boot still reaches `auralite#` (513-514 ticks, ~5 s).
**Gate:** 8-vector `tests/test_ahci.c` green + `make test` +
`make test-sanitize` green (LSan: teardown frees both the test mmio
regions and the attached images); UP boot measured above; 2-vCPU boot
caveat recorded in STATUS.md (guest ahci_exec times out deterministically,
also on QEMU: 2-vCPU + storage re-validates with the real engine in S2).

### S2 — DMA read

Command engine: on `PxCI` write, walk cmd header → table → H2D FIS; execute
`READ DMA EXT` from the host image with `pread` (virtual-host byte copy into
the PRDT buffer), set `prdbc`, clear `PxCI`, raise/clear TFD honestly;
IDENTIFY DEVICE (`0xEC`) implemented machine-side (standard 512-byte block:
LBA28 max sectors + LBA48 capacity from image size, ATA version words
zero-honest); error path sets `PxIS.TFES` + `PxTFD` b0 and clears `PxCI`.

**DoD (measured, captured):** with `--sata=` pointing at a blank 16 MiB
image, UP boot log: `self-test: reading sector 0 from port 0...` +
`sector 0: data, first bytes: <16 x 00>` +
`self-test: blank LBA0 read successfully; write verification skipped
(no scratch marker)`; then blkdev registers blk0 and the guest's format
attempts take the documented S2 TFES receipt (`error PxIS=0x40000000
PxTFD=0x41`) until shell.  2-vCPU receipt (same image): R5 line +
boot-to-shell ~5 s + typed `/tests/smpstress` -> `O_APPEND 6x200x32
intact`, `100 fork/wait cycles precise`, `8x10 signal deliveries
counted`, `SMPSTRESS PASS`.  Testability: `--keys-at=prompt` fires the
key injection on the guest's own shell echo (prompt arrival measured
budget-noisy across machine shapes: 2.23G K8-era vs 2.88G storage-boot).
Unit vectors (13 total): single + multi-PRDT reads with golden-byte
comparison, IDENTIFY block, OOB -> TFES + TFD.ERR + W1C recovery,
S3-scope WRITE honest TFES.
**Gate:** `make test` + `make test-sanitize` green; two identical
`--max-instr=11G` UP boots byte-equal.

### S3 — DMA write + guest round-trip

WRITE DMA EXT with `pwrite`; guest-visible effects. DoD mirrors QEMU
`test_ahci_rw.sh` with the emulator's own input lane (KBC `--keys-at`
recipes): boot with a blank 16 MiB image → guest formats diskfs + FAT32 →
type `write /disk/ci.txt <token>` / `cat /disk/ci.txt` /
`write /fat/CI.TXT <token2>` / `cat` / `exit`; assert in the host log:
`[ahci] PASS: SATA read/write DMA works` (non-blank after format),
`[diskfs] PASS:`, `[fat32] PASS:`, both tokens round-trip, **no**
`[ahci | diskfs | fat32] FAIL` lines; blank-disk lane clean. Two runs
byte-identical.

**Gate:** the receipts above + tests; harness script committed under
`tests/integration/`.

**Receipts (2026-10-07, head of S3):**

- Engine: the S2 `ahci_dma_read` became `ahci_dma_xfer(..., w)` — one
  shared PRDT walk for 0x25/0x35 with a length-overflow guard
  (`lba > UINT64_MAX/512`, `off+bytes > image`); WRITE DMA EXT takes the
  guest-RAM→image direction off the same command-header `W` bit the read
  path already honoured.  The "S3-scope WRITE" TFES stub is gone.
- Unit suite `tests/test_ahci.c`: 13 → **16 vectors, all pass**
  (`make test` + `make test-sanitize` green): write single+read-back
  (pattern→image bytes verified host-side, then a full engine READ round
  trip of the written sectors), write multi-PRDT (0x11/0x22/0x33 spread
  across three PRDTs), write OOB → TFES with image tails untouched.
  Fixture foot-gun recorded: the command-header `W` bit is sticky guest
  RAM, so a write-then-read sequence must rebuild the header — the S2
  readback vector learned to re-issue `t_hdr(w=0)`.
- Guest lane A (marked image = `AURALHCI` apex + 55AA @ LBA0, 16 MiB,
  frozen kernel.elf+initrd): **`[ahci] PASS: SATA read/write DMA works`**
  (guest scratch-sector write at LBA1 verified+restored through 0x35),
  AUFS auto-format at LBA2 + FAT32 at LBA64 (auto-format gate is the
  experimental-FS one; diskfs/FAT32 follow their own contract boot-flag
  `auto-format: DISABLED (build)` — they format table-less scratch disks
  by design), then the typed script via `--keys-at=prompt`
  (`write /disk/ci.txt s3disk1ok` → `cat` echoes `s3disk1ok`;
  `write /fat/ci.txt s3fat22ok` → `cat` echoes `s3fat22ok`).
  Zero `unsupported ATA cmd`, zero `[ahci|diskfs|fat32] FAIL`.
  Injection needed `KBC_QUEUE` 64 → 512 (the script is ~180 scancode
  bytes; queue depth is a test-lane constant, not silicon).
- Guest lane B (blank 16 MiB), run **twice sequentially**: blank-LBA0
  line `write verification skipped (no scratch marker)`, same format +
  mount + PASS lines; both serial logs **byte-identical** (97,406 B) —
  determinism pair.  A parallel-run attempt diverged (host scheduling,
  not guest state) — pairs are sequential, same as S2.
- Persistence policy (measured, unchanged): `--sata=` is copy-on-attach
  in-memory; after lane A the host image is byte-pristine (LBA2/LBA64
  untouched).  Writethrough semantics stay on the S5 plate.
- Harness: `tests/integration/test_ahci_rw.sh` automates lanes A+B and
  the byte-compare.

### S4 — Breadth matrix

Multiple `--disk-portN=` attachments (0..5 exposed of NP=6), optional second
controller (`--ahci2` adds a second PCI function so the guest's multi-scan
`ctrl_count` path runs), up to 4 guest-visible volumes (`/disk /fat` on
disk 1, `/ext2 /exfat /ext4` per further disks).

**DoD (measured):** the three `test_ahci_matrix.sh` lanes reproduced
guest-side: A port 0 r/w token; B port 1 only (empty port 0 skipped —
guest log shows no port-0 disk); C two controllers one disk each, guest
`controller 1` line + both tokens round-trip; 2-disk boot shows
`[ext2]` mounted.
**Gate:** matrix receipts + tests.

### S5 — Large transfers + hardening

128-KiB multi-sector reads (mirror `test_ahci_large_read.sh`: a multi-MB
file the guest reads fully), dirty-image writethrough semantics,
honest INTx line (PxIS.IPS → PIC/IOAPIC, currently documented-not-wired),
negative-control run without `--disk=` (S0 receipt verbatim), determinism
×2 on the full matrix, sanitize lanes, docs refresh
(`ROADMAP.md` ledger row, `docs/STATUS.md` evidence, cumulative patch
regeneration).

**DoD:** large-read token fully round-tripped; two identical runs; the no
`--disk=` boot byte-identical to the S0 baseline receipt.
**Gate:** all above + `make test` + `make test-sanitize`.

## 3. Standing constraints (all phases)

1. Measured before claimed — every DoD row above is a grep over a captured
   serial log, not a hope.
2. One patch per phase; fix + regression vector together.
3. `make test` and `make test-sanitize` green at each boundary.
4. K0–K8 receipts stay intact: UP boot to shell and the 2-vCPU
   `SMPSTRESS PASS` / `IRQAPWAKE PASS` reruns must not change content.
5. No guest-visible contract of existing devices is changed by adding the
   storage model.
