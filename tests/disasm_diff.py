#!/usr/bin/env python3
"""TOOLING T1: differential test of src/disasm.c against ndisasm.

A deterministic corpus covers every one-byte, 0F, 0F38 and 0F3A opcode in
16-, 32- and 64-bit code under a set of legacy/REX prefix combinations and
ModRM/SIB/displacement shapes. Each candidate sits at a fixed stride and is
followed by distinctive filler bytes (imm/disp material) and NOP padding,
so ndisasm's linear sweep re-synchronises before the next candidate and the
line at every stride offset is that candidate's decode.

Classification per candidate:
  match        both decode, same length and identical text
  diff         both decode, length or text differ   -> FAIL unless allowlisted
  ours-only    we decode, ndisasm emits db          -> FAIL unless allowlisted
  scope        ndisasm decodes, we emit db (VEX/EVEX/XOP/3DNow!/SSE4a/...
               and opcodes outside the declared T1 coverage) -> reported
  both-db      both reject

Exit 0 when diff == ours-only == 0 after the allowlist; 2 when ndisasm is
missing (SKIP); 1 otherwise.
"""
import argparse, os, re, shutil, subprocess, sys, collections

STRIDE = 32
FILL = bytes.fromhex("11223344")

LEGACY = ["66", "67", "f2", "f3", "f0", "26", "66 67"]
PREFIXES = {
    16: [""] + LEGACY,
    32: [""] + LEGACY,
    64: [""] + [p for p in LEGACY if p != "26"] + ["64", "48", "41", "44", "4c",
                                                  "66 48", "f3 48", "f2 48"],
}
# ModRM shapes (bytes after the opcode). FULL: every reg value in register
# and simple-memory form plus SIB/displacement/absolute/RIP-relative shapes;
# SHORT (used under prefixes): every reg value in register form + two
# memory forms.
FULL = ([bytes([0xC0 | (r << 3) | 1]) for r in range(8)] +
        [bytes([(r << 3) | 0]) for r in range(8)] +
        [bytes.fromhex(h) for h in (
            "0424", "0488", "0425f0ffffff", "442408", "05f0ffffff",
            "45f0", "4500", "8500ffffff", "848d00010000", "0c6500100000",
            "4010", "4680", "86f0ff", "0600f0", "c7", "f8", "e8", "d8",
            "1c24", "7c24f8")])
SHORT = [bytes([0xC0 | (r << 3) | 1]) for r in range(8)] + [bytes.fromhex("00"), bytes.fromhex("45f0")]
ESC3 = [bytes.fromhex(h) for h in ("c1", "00", "45f0", "0488")]

def corpus(bits):
    """candidate byte strings for one code size (deduplicated, ordered)"""
    seen = set(); out = []
    legacy = {0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65, 0x66, 0x67, 0xF0, 0xF2, 0xF3, 0x0F}
    def emit(c):
        if c not in seen:
            seen.add(c); out.append(c)
    for pfx in PREFIXES[bits]:
        p = bytes.fromhex(pfx.replace(" ", ""))
        shapes = FULL if pfx == "" else SHORT
        for op in range(256):
            if op in legacy: continue
            if bits == 64 and 0x40 <= op <= 0x4F: continue
            for m in shapes: emit(p + bytes([op]) + m)
        for op in range(256):
            if op in (0x38, 0x3A, 0x0F): continue
            for m in shapes: emit(p + bytes([0x0F, op]) + m)
        if pfx in ("", "66", "f2", "f3", "48", "66 48", "f2 48", "66 67"):
            for esc in (0x38, 0x3A):
                for op in range(256):
                    for m in ESC3: emit(p + bytes([0x0F, esc, op]) + m)
    return out

def build(cands):
    data = bytearray()
    for c in cands:
        blob = (c + FILL)[:STRIDE]
        data += blob + b"\x90" * (STRIDE - len(blob))
    return bytes(data)

# ndisasm prints a lone prefix ("rep", "o16", "es", ...) as a 1-byte line
# when the instruction after it does not decode: that is a reject too.
LONE = {"rep", "repe", "repne", "lock", "es", "cs", "ss", "ds", "fs", "gs",
        "o16", "o32", "o64", "a16", "a32", "a64", "bnd", "wait", "rex",
        "rex.w", "rex.b", "rex.x", "rex.r"}

def is_reject(e, ndisasm=True):
    if e[0] != 1: return False
    if e[1].startswith("db "): return True
    return ndisasm and (e[1] in LONE or e[1].startswith("rex"))

def parse_listing(lines):
    """offset -> (len, text) for stride-aligned lines of an ndisasm-style listing"""
    res = {}
    last = None
    for ln in lines:
        if not ln.strip(): continue
        if ln[0].isspace():
            s = ln.strip()
            if s.startswith("-") and last is not None:
                l, t = res[last]
                res[last] = (l + (len(s) - 1) // 2, t)
            continue
        off = int(ln[:8], 16)
        if off % STRIDE:
            last = None
            continue
        parts = ln.split(None, 2)
        hexb = parts[1] if len(parts) > 1 else ""
        txt = parts[2] if len(parts) > 2 else ""
        res[off] = (len(hexb) // 2, txt.strip())
        last = off
    return res

def run_listing(cmd):
    with subprocess.Popen(cmd, stdout=subprocess.PIPE, text=True, bufsize=1 << 20) as p:
        res = parse_listing(p.stdout)
        p.wait()
        if p.returncode: raise SystemExit("%s failed rc=%d" % (cmd[0], p.returncode))
    return res

# ---- allowlist: documented divergences, each with a reason -------------
VEC = re.compile(r"\bx?mm\d+\b")
SIZEKW = re.compile(r"\b(byte|word|dword|qword|tword|oword) (?=\[)")
IMMKW = re.compile(r",byte (0x[0-9a-f]+)$")

def sse_norm(t):
    return IMMKW.sub(r",\1", SIZEKW.sub("", t))

CYRIX = {"paddsiw", "pmagw", "psubsiw", "pmulhrwc", "pmulhriw", "paveb", "pdistib",
         "pmachriw", "pmvzb", "pmvnzb", "pmvlzb", "pmvgezb", "pmulhrw", "rdshr", "wrshr",
         "rsdc", "svdc", "rsldt", "svldt", "rsts", "svts", "smint", "smintold", "dmint", "rdm"}

def split_prefixes(bits, b):
    i = 0
    while i < len(b) and b[i] in (0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65, 0x66, 0x67, 0xF0, 0xF2, 0xF3):
        i += 1
    rex = 0
    if bits == 64 and i < len(b) and 0x40 <= b[i] <= 0x4F:
        rex = b[i]; i += 1
    return i, rex

def allow(bits, cand, ours, theirs):
    """return a reason string when a divergence is accepted"""
    i, rex = split_prefixes(bits, cand)
    op = cand[i] if i < len(cand) else None
    op2 = cand[i + 1] if op == 0x0F and i + 1 < len(cand) else None
    modrm = cand[i + (2 if op == 0x0F else 1)] if i + (2 if op == 0x0F else 1) < len(cand) else 0
    reg = (modrm >> 3) & 7
    ot, tt = ours[1], theirs[1]
    if op == 0x9B:
        return "9B: ndisasm folds FWAIT into the next instruction as a prefix; the CPU executes it alone"
    if bits == 64 and op == 0x63 and not (rex & 8):
        return "63 /r without REX.W in 64-bit: ndisasm leaves it undecoded; SDM MOVSXD r32,r/m32 (CPU: 2-byte move)"
    if bits == 64 and op in (0x9C, 0x9D) and (rex & 8):
        return "REX.W PUSHF/POPF: ndisasm rejects; the CPU ignores REX.W on these default-64 ops"
    if bits == 64 and 0x66 in cand[:i] and (op in (0xE8, 0xE9) or (op == 0x0F and op2 is not None and 0x80 <= op2 <= 0x8F)):
        return "66-prefixed near branch in 64-bit: ndisasm decodes Intel (66 ignored, rel32); cpu.c E8/E9/0F8x honour it (rel16, AMD)"
    if tt.split()[0] in CYRIX:
        return "ndisasm decodes Cyrix EMMI opcodes (no CPU modelled here implements them)"
    if op in (0xC0, 0xC1, 0xD0, 0xD1, 0xD2, 0xD3) and reg == 6 and ot.split()[-2:-1] != ["?"]:
        return "shift group /6: the CPU executes SAL (alias of SHL, cpu.c 'SAL aliases SHL'); ndisasm rejects it"
    if op in (0xF6, 0xF7) and reg == 1:
        return "F6/F7 /1: the CPU executes TEST (alias of /0); ndisasm rejects it"
    if op == 0x0F and op2 is not None and 0x90 <= op2 <= 0x9F and reg != 0:
        return "SETcc with ModRM.reg != 0: the CPU ignores the reg field; ndisasm requires /0"
    if op == 0x0F and op2 in (0x1A, 0x1B, 0x1C, 0x1E) and ours[0] == theirs[0]:
        return "0F 1A-1C/1E: ndisasm names the MPX/CLDEMOTE/CET forms; the CPU runs the 0F 18-1F block as hint NOPs (same length)"
    if op == 0x0F and op2 == 0x18 and reg in (6, 7) and ours[0] == theirs[0]:
        return "0F 18 /6,/7: ndisasm names PREFETCHIT1/IT0; the CPU runs the 0F 18 block as hint NOPs (same length)"
    if bits != 64 and re.search(r"\bxmm(8|9|1[0-5])\b", tt) and ours[0] == theirs[0]:
        return "ndisasm bug: 16-bit-addressed SSE memory operand printed as xmm8-15 outside 64-bit mode"
    if bits == 64 and (rex & 8) and ours[0] == theirs[0] and (VEC.search(ot) or VEC.search(tt)):
        a2 = sse_norm(re.sub(r"\bo64 ", "", ot)); b2 = sse_norm(re.sub(r"\bo64 ", "", tt))
        if a2 == b2:
            return "REX.W on a legacy SSE/MMX encoding: no architectural effect; ndisasm prints 'o64' for some templates only"
    if ours[0] == theirs[0] and (VEC.search(ot) or VEC.search(tt)) and sse_norm(ot) == sse_norm(tt):
        return "SSE/MMX size keyword: NASM's sized-vs-unsized template choice (same operands, same length)"
    return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--x86dis", default="./x86dis")
    ap.add_argument("--ndisasm", default=shutil.which("ndisasm") or "ndisasm")
    ap.add_argument("--work", default="/tmp/disasm-diff")
    ap.add_argument("--show", type=int, default=25)
    ap.add_argument("--bits", type=int, nargs="*", default=[16, 32, 64])
    ap.add_argument("--summary", action="store_true", help="print mismatch histogram by opcode")
    a = ap.parse_args()
    if not shutil.which(a.ndisasm) and not os.path.exists(a.ndisasm):
        print("SKIP disasm-diff: ndisasm not found (ships with nasm)")
        return 2
    os.makedirs(a.work, exist_ok=True)
    tot = collections.Counter()
    shown = 0
    hist = collections.Counter()
    allowed = collections.Counter()
    scope = collections.Counter()
    for bits in a.bits:
        cands = corpus(bits)
        blob = build(cands)
        path = os.path.join(a.work, "corpus%d.bin" % bits)
        with open(path, "wb") as f: f.write(blob)
        ndl = run_listing([a.ndisasm, "-b%d" % bits, path])
        usl = run_listing([a.x86dis, "-b%d" % bits, "--stride=%d" % STRIDE, path])
        for k, c in enumerate(cands):
            off = k * STRIDE
            t = ndl.get(off); o = usl.get(off)
            if t is None:
                tot["ndisasm-desync"] += 1
                continue
            if o is None:
                tot["x86dis-missing"] += 1
                continue
            tdb = is_reject(t)
            odb = is_reject(o, ndisasm=False)
            if tdb and odb: tot["both-db"] += 1; continue
            if odb and not tdb:
                tot["scope"] += 1
                key = "%d:%s" % (bits, c[:min(len(c), 4)].hex())
                scope[t[1].split()[0] if t[1] else "?"] += 1
                continue
            if o == t: tot["match"] += 1; continue
            why = allow(bits, c, o, t)
            if why:
                tot["allowed"] += 1; allowed[why] += 1; continue
            kind = "ours-only" if tdb else "diff"
            tot[kind] += 1
            opk = c.hex()[:8]
            hist[(bits, t[1].split()[0] if t[1] else "?")] += 1
            if shown < a.show:
                shown += 1
                print("%-9s b%d %-28s ndisasm(len %d) %-40s ours(len %d) %s" % (kind, bits, c.hex(), t[0], t[1], o[0], o[1]))
        tot["candidates"] += len(cands)
    print("disasm-diff: candidates=%d match=%d allowed=%d diff=%d ours-only=%d scope=%d both-db=%d desync=%d" % (
        tot["candidates"], tot["match"], tot["allowed"], tot["diff"], tot["ours-only"], tot["scope"],
        tot["both-db"], tot["ndisasm-desync"] + tot["x86dis-missing"]))
    for why, n in sorted(allowed.items(), key=lambda x: -x[1]):
        print("  allowed %6d  %s" % (n, why))
    if a.summary:
        for (bits, mn), n in hist.most_common(60):
            print("  diff b%d %-16s %d" % (bits, mn, n))
        print("  scope (ndisasm decodes, T1 does not):", ", ".join("%s=%d" % kv for kv in scope.most_common(40)))
    bad = tot["diff"] + tot["ours-only"] + tot["ndisasm-desync"] + tot["x86dis-missing"]
    print("PASS disasm-diff" if bad == 0 else "FAIL disasm-diff (%d)" % bad)
    return 0 if bad == 0 else 1

if __name__ == "__main__":
    sys.exit(main())
