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


def regs(reply):
    """split a g reply (gdb amd64 layout of the stub's target.xml)"""
    sizes = [8] * 17 + [4] * 7 + [10] * 8 + [4] * 8 + [16] * 16 + [4] + [8] * 7
    out, o = [], 0
    for sz in sizes:
        out.append(le(reply[o:o + 2 * sz]))
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


if __name__ == "__main__":
    for lane in (lane_a, lane_b, lane_c):
        try:
            lane()
        except Exception as exc:          # a protocol failure is a FAIL, not a hang
            check(False, "%s raised %r" % (lane.__name__, exc))
    print("PASS test-gdbstub" if not fails else "FAIL test-gdbstub (%d)" % fails)
    sys.exit(1 if fails else 0)
