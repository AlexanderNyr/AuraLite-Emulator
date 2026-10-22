#!/usr/bin/env python3
"""TOOLING T3: GDB Remote Serial Protocol client vectors for --gdb=.

Pure sockets (no gdb needed). Lanes:
  A  firmware session: qSupported/NoAck, target.xml, reset registers,
     thread list, Z0 at the guest entry 0x100000, continue -> T05 swbreak,
     rip/memory equal to disk/test_kernel.bin, single step (+1 byte: cli),
     Z2 watchpoint on framebuffer pixel 0 -> T05watch, monitor command,
     delete everything, continue -> W00; stdout identical to a plain run.
  B  Ctrl-C: an endless `jmp $` ROM keeps running until 0x03 -> T02, then
     `k` ends the emulator with exit code 0.
  C  protocol hygiene: a bad checksum is NAKed ('-') in ack mode; unknown
     packets get the empty reply; out-of-range register/memory errors.
  D  x87 (UPKEEP U3): a ROM payload pushes 1.0/pi/log2e/+0 (TOP=4) and
     stops before four FSTP m80 stores to 0x500.  g/p must show the
     LOGICAL (TOP-rotated) stack, the full physical FTAG, and truthful
     FCW/FSW/FIP/FOP -- the convention measured from GDB's i387-tdep.c
     and QEMU's gdbstub read side.  P then rewrites st0/st3/an empty
     slot + fctrl; resume runs the stores, so guest memory proves the
     CPU consumed exactly the written bytes (a physical-order write
     would land 0.5 in the wrong slot and 1.0 would store instead).
Exit 0 = PASS, 1 = FAIL.
"""
import os, socket, subprocess, sys, tempfile, time

X86EMU = os.environ.get("X86EMU", "./x86emu")
fails = 0


def check(cond, label):
    global fails
    print(("ok   " if cond else "FAIL ") + label)
    if not cond:
        fails += 1


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class RSP:
    def __init__(self, port, timeout=20.0):
        deadline = time.time() + timeout
        while True:
            try:
                self.s = socket.create_connection(("127.0.0.1", port), timeout=2)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.05)
        self.s.settimeout(timeout)
        self.buf = b""
        self.ack = True

    def _read(self):
        d = self.s.recv(65536)
        if not d:
            raise EOFError("stub closed the connection")
        self.buf += d

    def send_raw(self, b):
        self.s.sendall(b)

    def send(self, data):
        cs = sum(data.encode()) & 0xFF
        self.s.sendall(b"$" + data.encode() + b"#%02x" % cs)
        if self.ack:
            while not self.buf:
                self._read()
            c, self.buf = self.buf[:1], self.buf[1:]
            if c != b"+":
                raise RuntimeError("expected ack, got %r" % c)

    def recv(self):
        while True:
            i = self.buf.find(b"$")
            if i >= 0:
                j = self.buf.find(b"#", i)
                if j >= 0 and len(self.buf) >= j + 3:
                    data = self.buf[i + 1:j]
                    cs = int(self.buf[j + 1:j + 3], 16)
                    self.buf = self.buf[j + 3:]
                    assert sum(data) & 0xFF == cs, "bad checksum from stub"
                    if self.ack:
                        self.s.sendall(b"+")
                    return data.decode("latin1")
            self._read()

    def cmd(self, data):
        self.send(data)
        return self.recv()


def le(hexstr):
    return int.from_bytes(bytes.fromhex(hexstr), "little")


SIZES = [8] * 17 + [4] * 7 + [10] * 8 + [4] * 8 + [16] * 16 + [4] + [8] * 7


def regs(reply):
    """split a g reply (gdb amd64 layout of the stub's target.xml)"""
    out, o = [], 0
    for sz in SIZES:
        out.append(le(reply[o:o + 2 * sz]))
        o += 2 * sz
    return out


def reghex(reply):
    """same split, raw hex per register (byte-exact for the 80-bit STs)"""
    out, o = [], 0
    for sz in SIZES:
        out.append(reply[o:o + 2 * sz])
        o += 2 * sz
    return out


def start(args):
    port = free_port()
    p = subprocess.Popen([X86EMU] + args + ["--gdb=127.0.0.1:%d" % port],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return p, RSP(port)


def lane_a():
    plain = subprocess.run([X86EMU, "--platform=haswell", "--max-instr=20000000", "--stats"],
                           capture_output=True).stdout
    p, r = start(["--platform=haswell", "--max-instr=20000000", "--stats"])
    sup = r.cmd("qSupported:multiprocess+;swbreak+;hwbreak+;qRelocInsn+")
    check("qXfer:features:read+" in sup and "swbreak+" in sup, "A qSupported advertises target.xml and swbreak")
    check(r.cmd("QStartNoAckMode") == "OK", "A QStartNoAckMode")
    r.ack = False
    xml, off = "", 0
    while True:
        part = r.cmd("qXfer:features:read:target.xml:%x,800" % off)
        xml += part[1:]
        off += len(part) - 1
        if part[0] == "l":
            break
    check("<architecture>i386:x86-64</architecture>" in xml and 'name="xmm15"' in xml and 'name="cr3"' in xml,
          "A target.xml: i386:x86-64 core + sse + segments + sys (%d bytes)" % len(xml))
    check(r.cmd("?").startswith("T05thread:1"), "A '?' reports the start stop on thread 1")
    rg = regs(r.cmd("g"))
    check(rg[16] == 0xFFF0 and rg[18] == 0xF000, "A reset state: rip=0xfff0 cs=0xf000 (got %x/%x)" % (rg[16], rg[18]))
    check(r.cmd("qfThreadInfo") == "m1" and r.cmd("qsThreadInfo") == "l", "A one thread (vCPU)")
    check(r.cmd("Hg1") == "OK" and r.cmd("qC") == "QC1", "A Hg1 / qC")
    check(r.cmd("Z0,100000,1") == "OK", "A Z0 at the guest entry")
    stop = r.cmd("c")
    check(stop.startswith("T05") and "swbreak" in stop, "A continue -> breakpoint stop (%s)" % stop)
    check(le(r.cmd("p10")) == 0x100000, "A rip == 0x100000")
    kern = open("disk/test_kernel.bin", "rb").read()[:16]
    mem = bytes.fromhex(r.cmd("m100000,10"))
    check(mem == kern, "A memory at 0x100000 == disk/test_kernel.bin[0:16]")
    check(r.cmd("z0,100000,1") == "OK", "A z0")
    stop = r.cmd("s")
    check(stop.startswith("T05") and le(r.cmd("p10")) == 0x100001, "A stepi over `cli` -> rip 0x100001")
    check(r.cmd("Z2,d0000000,4") == "OK", "A Z2 write watch on framebuffer pixel 0")
    stop = r.cmd("vCont;c")
    check(stop.startswith("T05watch:d0000000"), "A vCont;c -> watch stop (%s)" % stop)
    check(bytes.fromhex(r.cmd("md0000000,4")) == bytes.fromhex("00002000"), "A pixel 0 now holds the 0x00200000 marker")
    r.send("qRcmd," + "info vcpu".encode().hex())
    out = ""
    while True:
        pk = r.recv()
        if pk == "OK":
            break
        if pk.startswith("O"):
            out += bytes.fromhex(pk[1:]).decode()
    check("vcpu0" in out and "long64" in out, "A monitor info vcpu")
    check(r.cmd("z2,d0000000,4") == "OK", "A z2")
    end = r.cmd("c")
    check(end == "W00", "A run to the end -> W00 (%s)" % end)
    try:
        o, e = p.communicate(timeout=30)
    except subprocess.TimeoutExpired:
        p.kill()
        o, e = p.communicate()
    check(p.returncode == 0, "A emulator exit code 0 (%s)" % p.returncode)
    check(o == plain, "A stdout incl. --stats identical to the plain run")


def lane_b():
    rom = tempfile.NamedTemporaryFile(delete=False, suffix=".rom")
    img = bytearray(b"\xf4" * 131072)          # hlt everywhere
    img[0x1FFF0:0x1FFF2] = b"\xeb\xfe"         # reset vector: jmp $
    rom.write(img)
    rom.close()
    p, r = start(["--rom=" + rom.name, "--max-instr=100000000000"])
    r.cmd("QStartNoAckMode")
    r.ack = False
    r.send("c")
    time.sleep(0.6)
    t0 = time.time()
    r.send_raw(b"\x03")
    r.s.settimeout(5.0)
    try:
        stop = r.recv()
    except (socket.timeout, OSError):
        stop = "<no stop reply within 5 s>"
    dt = time.time() - t0
    check(stop.startswith("T02"), "B Ctrl-C (0x03) while running -> T02 (%s, %.0f ms)" % (stop, dt * 1000))
    if not stop.startswith("T02"):
        p.kill(); p.wait(); os.unlink(rom.name)
        return
    r.s.settimeout(20.0)
    rg = regs(r.cmd("g"))
    check(rg[16] == 0xFFF0, "B stopped inside the jmp $ loop (rip=%x)" % rg[16])
    r.send("k")
    try:
        p.wait(timeout=30)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
    check(p.returncode == 0, "B kill ends the emulator (rc=%s)" % p.returncode)
    os.unlink(rom.name)


def lane_c():
    p, r = start(["--platform=haswell", "--max-instr=20000000"])
    r.s.sendall(b"$g#00")
    while not r.buf:
        r._read()
    check(r.buf[:1] == b"-", "C bad checksum is NAKed")
    r.buf = b""
    check(r.cmd("vMustReplyEmpty") == "", "C unknown packet -> empty reply")
    check(r.cmd("p99") == "E01", "C register out of range -> E01")
    check(r.cmd("m200000000,4") == "ffffffff", "C open bus reads 0xff (side-effect free)")
    check(r.cmd("mfeb10000,4").startswith("E"), "C device register (AHCI ABAR) is never read -> error")
    r.send("k")
    try:
        p.wait(timeout=30)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
    check(p.returncode == 0, "C kill from the start stop (rc=%s)" % p.returncode)


F80_ZERO = "00" * 10
F80_ONE  = "0000000000000080ff3f"
F80_PI   = "35c26821a2da0fc90040"
F80_L2E  = "bcf0175c293baab8ff3f"
F80_M2_5 = "00000000000000a000c0"   # -2.5
F80_HALF = "0000000000000080fe3f"   # 0.5
F80_DEN  = "01000000000000000000"   # smallest denormal -> tag "special"


def lane_d():
    # 16-bit payload at CS:FF00 (linear 0xFFFFFF00); reset vector jumps in.
    # fninit; fld1; fldpi; fldl2e; fldz  -> TOP=4; ST0=+0 ST1=l2e ST2=pi ST3=1
    # then four fstp tbyte [0x500+16k] pop the stack into RAM; jmp $.
    rom = tempfile.NamedTemporaryFile(delete=False, suffix=".rom")
    img = bytearray(b"\xf4" * 131072)
    code = bytes([
        0xDB, 0xE3,                          # FF00 fninit
        0xD9, 0xE8,                          # FF02 fld1
        0xD9, 0xEB,                          # FF04 fldpi
        0xD9, 0xEA,                          # FF06 fldl2e
        0xD9, 0xEE,                          # FF08 fldz
        0xDB, 0x3E, 0x00, 0x05,              # FF0A fstp tbyte [0500]
        0xDB, 0x3E, 0x10, 0x05,              # FF0E fstp tbyte [0510]
        0xDB, 0x3E, 0x20, 0x05,              # FF12 fstp tbyte [0520]
        0xDB, 0x3E, 0x30, 0x05,              # FF16 fstp tbyte [0530]
        0xEB, 0xFE,                          # FF1A jmp $
    ])
    img[0x1FF00:0x1FF00 + len(code)] = code
    img[0x1FFF0:0x1FFF3] = b"\xe9\x0d\xff"   # reset: jmp FF00
    rom.write(img)
    rom.close()
    bp1, bp2 = 0xFFFFFF0A, 0xFFFFFF1A        # linears of fstp #1 / jmp $
    p, r = start(["--rom=" + rom.name, "--max-instr=100000000000"])
    r.cmd("QStartNoAckMode")
    r.ack = False
    check(r.cmd("Z0,%x,1" % bp1) == "OK", "D Z0 at the first FSTP")
    stop = r.cmd("c")
    check(stop.startswith("T05") and "swbreak" in stop, "D stop with the x87 stack loaded (%s)" % stop)
    gr = r.cmd("g")
    gx, gv = reghex(gr), regs(gr)
    check(len(gr) == 1184, "D g packet size unchanged (592 bytes / 64 registers)")
    check(gx[24] == F80_ZERO, "D st0 is +0 (logical order: TOP=4, not the physical slot)")
    check(gx[25] == F80_L2E, "D st1 is log2(e)")
    check(gx[26] == F80_PI, "D st2 is pi")
    check(gx[27] == F80_ONE, "D st3 is 1.0 (physical order would read an empty slot here)")
    check(gv[32] == 0x037F, "D fctrl is the post-FNINIT 0x037f (got %x)" % gv[32])
    check(gv[33] & 0xFFFF == 0x2000, "D fstat 0x2000: TOP=4, no exceptions (got %x)" % gv[33])
    check(gv[34] == 0x01FF, "D ftag 0x01ff: full tag word, PHYSICAL order (phys4 zero, 5-7 valid, 0-3 empty) (got %x)" % gv[34])
    check(gv[35] == 0xF000 and gv[36] == 0xFF08 and gv[39] == 0x1EE,
          "D fiseg/fioff/fop name the FLDZ (F000:FF08, op 0x1ee) (got %x/%x/%x)" % (gv[35], gv[36], gv[39]))
    check(gv[16] == 0xFF0A, "D rip canary 0xff0a (non-x87 registers unchanged)")
    check(r.cmd("p18") == F80_ZERO and r.cmd("p21") == "00200000" and r.cmd("p22") == "ff010000",
          "D p reads agree with g (st0/fstat/ftag)")
    oks = [r.cmd("P18=" + F80_M2_5), r.cmd("P1b=" + F80_HALF),
           r.cmd("P1f=" + F80_DEN), r.cmd("P20=3f0f0000")]
    check(all(o == "OK" for o in oks), "D P accepts st0, st3, st7 (empty slot), fctrl")
    gr = r.cmd("g")
    gx, gv = reghex(gr), regs(gr)
    check(gx[24] == F80_M2_5 and gx[31] == F80_DEN, "D written st0/st7 read back in logical places")
    check(gx[27] == F80_HALF, "D written st3 reads back (write rotated by TOP like the read)")
    check(gv[34] == 0x00BF, "D ftag re-classified on write: zero->valid, empty->special (0x00bf) (got %x)" % gv[34])
    check(r.cmd("p20") == "3f0f0000", "D written fctrl 0x0f3f sticks")
    check(r.cmd("z0,%x,1" % bp1) == "OK" and r.cmd("Z0,%x,1" % bp2) == "OK", "D breakpoints swapped to the spin")
    stop = r.cmd("c")
    check(stop.startswith("T05") and "swbreak" in stop, "D resumed through the four FSTPs to the spin (%s)" % stop)
    check(r.cmd("m500,a") == F80_M2_5, "D FSTP #1 stored exactly the P-written -2.5")
    check(r.cmd("m510,a") == F80_L2E and r.cmd("m520,a") == F80_PI, "D FSTP #2/#3 stored the untouched constants")
    check(r.cmd("m530,a") == F80_HALF, "D FSTP #4 stored the P-written logical st3 (physical write would have stored 1.0)")
    gr = r.cmd("g")
    gx, gv = reghex(gr), regs(gr)
    check(gv[33] & 0xFFFF == 0 and gv[34] == 0xFFBF,
          "D after 4 pops: fstat 0 (TOP wrapped to 0), only the written denormal remains tagged (ftag %x)" % gv[34])
    check(gx[27] == F80_DEN, "D the denormal is now logical st3 (phys3 at TOP=0)")
    check(r.cmd("P21=00280000") == "OK" and r.cmd("p21") == "00280000" and r.cmd("p18") == F80_L2E,
          "D fstat write moves TOP: st0 now shows physical 5 (stale log2e bits)")
    r.send("k")
    try:
        p.wait(timeout=30)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
    check(p.returncode == 0, "D kill ends the emulator (rc=%s)" % p.returncode)
    os.unlink(rom.name)


if __name__ == "__main__":
    for lane in (lane_a, lane_b, lane_c, lane_d):
        try:
            lane()
        except Exception as exc:          # a protocol failure is a FAIL, not a hang
            check(False, "%s raised %r" % (lane.__name__, exc))
    print("PASS test-gdbstub" if not fails else "FAIL test-gdbstub (%d)" % fails)
    sys.exit(1 if fails else 0)
