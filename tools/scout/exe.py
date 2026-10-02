#!/usr/bin/env python3
"""
Scout probes for the Road Rash: Jailbreak (USA, SLUS_01053) main executable and overlays.

Throwaway-but-rerunnable analysis probes. Reads ONLY from work\\disc_us (gitignored game data)
and writes ONLY to stdout or to an explicitly named output path. Never copies game bytes into
the repo.

Usage (from the project root):
    python tools\\scout\\exe.py header
    python tools\\scout\\exe.py strings   [--min 4] [--filter REGEX]
    python tools\\scout\\exe.py io
    python tools\\scout\\exe.py bios
    python tools\\scout\\exe.py funcs
    python tools\\scout\\exe.py xref --addr 0x800xxxxx
    python tools\\scout\\exe.py ptrtable  <overlay>
    python tools\\scout\\exe.py loadaddr  <overlay>
    python tools\\scout\\exe.py dis --addr 0x800xxxxx [--count 64] [--file NAME]
    python tools\\scout\\exe.py emit-json --out db\\exe.json

Every probe prints the inputs it used (file + sha1) so results are reproducible.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
from dataclasses import dataclass, field

# ---------------------------------------------------------------------------
# Configuration - the only place that knows where game data lives.
# ---------------------------------------------------------------------------

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DISC = os.path.join(ROOT, "work", "disc_us")
OBJDUMP = r"C:\PSn00bSDK\bin\mipsel-none-elf-objdump.exe"

EXE_NAME = "SLUS_010.53"
OVERLAYS = ["RASHCDF.BIN", "RASHCDG.BIN", "RASHCDI.BIN"]

PSX_EXE_HEADER = 0x800  # file offset of the first text byte


# ---------------------------------------------------------------------------
# Image abstraction
# ---------------------------------------------------------------------------


@dataclass
class Image:
    """A flat guest-memory image: `data` maps to [base, base+len(data))."""

    name: str
    path: str
    data: bytes
    base: int          # guest vaddr of data[0]
    file_off: int      # file offset of data[0]
    sha1: str

    def contains(self, addr: int) -> bool:
        return self.base <= addr < self.base + len(self.data)

    def off(self, addr: int) -> int:
        return addr - self.base

    def word(self, addr: int) -> int:
        o = self.off(addr)
        return struct.unpack_from("<I", self.data, o)[0]

    def words(self):
        """Yield (vaddr, u32) for every aligned word."""
        n = len(self.data) // 4
        for i, (w,) in enumerate(struct.iter_unpack("<I", self.data[: n * 4])):
            yield self.base + i * 4, w


def sha1_of(path: str) -> str:
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def load_exe() -> Image:
    path = os.path.join(DISC, EXE_NAME)
    raw = open(path, "rb").read()
    assert raw[:8] == b"PS-X EXE", "not a PS-X EXE"
    text_addr = struct.unpack_from("<I", raw, 0x18)[0]
    text_size = struct.unpack_from("<I", raw, 0x1C)[0]
    return Image(
        name=EXE_NAME,
        path=path,
        data=raw[PSX_EXE_HEADER : PSX_EXE_HEADER + text_size],
        base=text_addr,
        file_off=PSX_EXE_HEADER,
        sha1=sha1_of(path),
    )


def exe_header() -> dict:
    path = os.path.join(DISC, EXE_NAME)
    raw = open(path, "rb").read()
    f = lambda o: struct.unpack_from("<I", raw, o)[0]
    return {
        "file": EXE_NAME,
        "file_size": len(raw),
        "sha1": sha1_of(path),
        "magic": raw[:8].decode("ascii"),
        "pc0": f(0x10),
        "gp0": f(0x14),
        "text_addr": f(0x18),
        "text_size": f(0x1C),
        "data_addr": f(0x20),
        "data_size": f(0x24),
        "bss_addr": f(0x28),
        "bss_size": f(0x2C),
        "sp_base": f(0x30),
        "sp_offset": f(0x34),
        "region": raw[0x4C:0x54 + 40].split(b"\x00")[0].decode("ascii", "replace"),
    }


def load_overlay(name: str, base: int | None = None) -> Image:
    """Load an overlay blob. `base` is the guest address of file offset 0.

    When `base` is None the caller only wants raw file inspection; base is set to 0.
    """
    path = os.path.join(DISC, name)
    raw = open(path, "rb").read()
    return Image(
        name=name,
        path=path,
        data=raw,
        base=0 if base is None else base,
        file_off=0,
        sha1=sha1_of(path),
    )


# ---------------------------------------------------------------------------
# MIPS decoding helpers (only what the probes need)
# ---------------------------------------------------------------------------

OP = lambda w: (w >> 26) & 0x3F
RS = lambda w: (w >> 21) & 0x1F
RT = lambda w: (w >> 16) & 0x1F
RD = lambda w: (w >> 11) & 0x1F
FUNCT = lambda w: w & 0x3F
IMM = lambda w: w & 0xFFFF
SIMM = lambda w: (w & 0xFFFF) - 0x10000 if (w & 0x8000) else (w & 0xFFFF)
TARGET = lambda w: w & 0x03FFFFFF

OP_SPECIAL = 0x00
OP_J = 0x02
OP_JAL = 0x03
OP_LUI = 0x0F
OP_ADDIU = 0x09
OP_ORI = 0x0D

F_JR = 0x08
F_JALR = 0x09
F_SYSCALL = 0x0C
F_BREAK = 0x0D

LOAD_OPS = {0x20: "lb", 0x21: "lh", 0x23: "lw", 0x24: "lbu", 0x25: "lhu", 0x22: "lwl", 0x26: "lwr"}
STORE_OPS = {0x28: "sb", 0x29: "sh", 0x2B: "sw", 0x2A: "swl", 0x2E: "swr"}

REGNAMES = [
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra",
]


# ---------------------------------------------------------------------------
# Probe: hardware I/O sites
# ---------------------------------------------------------------------------
#
# Technique: PS1 MMIO lives at physical 0x1F801000..0x1F802000, reached as
# KSEG0 0x9F801xxx or (overwhelmingly) KSEG1 0xBF801xxx. A load/store to a
# register is nearly always built as:
#       lui   rX, 0x1F80 | 0x9F80 | 0xBF80
#       ...
#       lw/sw rY, imm(rX)
# We scan for the lui, then follow the next N instructions looking for a
# memory op whose base register is rX and whose rX has not been clobbered.
# effective address = (lui_imm << 16) + simm.
# This is a *syntactic* scan: it finds direct MMIO accesses. Accesses made
# through a pointer held in a struct/global are NOT found by it, and are
# reported separately via the `bios` probe and by xref on the found sites.

IO_HI = {0x1F80, 0x9F80, 0xBF80}

IO_REGIONS = [
    # (lo, hi_exclusive, name)
    (0x1F800000, 0x1F801000, "scratchpad"),
    (0x1F801000, 0x1F801040, "memctl"),
    (0x1F801040, 0x1F801060, "sio0_pad_card"),
    (0x1F801050, 0x1F801060, "sio1"),
    (0x1F801060, 0x1F801070, "memctl2"),
    (0x1F801070, 0x1F801080, "irq"),
    (0x1F801080, 0x1F801100, "dma"),
    (0x1F801100, 0x1F801130, "timers"),
    (0x1F801800, 0x1F801804, "cdrom"),
    (0x1F801810, 0x1F801818, "gpu"),
    (0x1F801820, 0x1F801828, "mdec"),
    (0x1F801C00, 0x1F801E00, "spu"),
    (0x1F802000, 0x1F803000, "expansion2"),
]


def io_region(addr: int) -> str | None:
    phys = addr & 0x1FFFFFFF
    for lo, hi, name in IO_REGIONS:
        if lo <= phys < hi:
            return name
    if 0x1F801000 <= phys < 0x1F802000:
        return "io_other"
    return None


def scan_io(img: Image, window: int = 24) -> list[dict]:
    """Find `lui rX, <io page>` and then classify how rX is used.

    Three use forms are recognised, all within `window` instructions and only
    while rX has not been redefined:
      * `lw/sw rY, imm(rX)`            -> access at (page<<16)+imm
      * `ori rX, rX, imm` (or addiu)   -> refines the tracked constant in rX
      * `jal`/`jalr`                   -> rX is an argument, address only materialised

    The scan is purely syntactic. It finds direct MMIO/scratchpad touches;
    accesses made through a pointer previously stored in a global are NOT found
    (those are reported instead by `scan_word_refs` over the IO range).
    """
    hits: list[dict] = []
    n = len(img.data) // 4
    words = [w for (w,) in struct.iter_unpack("<I", img.data[: n * 4])]
    for i in range(n):
        w = words[i]
        if OP(w) != OP_LUI:
            continue
        page = IMM(w)
        if page not in IO_HI:
            continue
        base_reg = RT(w)
        if base_reg == 0:
            continue
        cur = page << 16
        used = False
        for j in range(i + 1, min(i + 1 + window, n)):
            w2 = words[j]
            op2 = OP(w2)
            kind = LOAD_OPS.get(op2) or STORE_OPS.get(op2)
            if kind and RS(w2) == base_reg:
                ea = (cur + SIMM(w2)) & 0xFFFFFFFF
                reg = io_region(ea)
                if reg:
                    used = True
                    hits.append(
                        {
                            "lui_at": img.base + i * 4,
                            "access_at": img.base + j * 4,
                            "op": kind,
                            "addr": ea,
                            "region": reg,
                            "reg": REGNAMES[RT(w2)],
                        }
                    )
                continue
            # constant refinement keeps the tracking alive
            if op2 == OP_ORI and RS(w2) == base_reg and RT(w2) == base_reg:
                cur = (cur | IMM(w2)) & 0xFFFFFFFF
                continue
            if op2 == OP_ADDIU and RS(w2) == base_reg and RT(w2) == base_reg:
                cur = (cur + SIMM(w2)) & 0xFFFFFFFF
                continue
            # base register redefined -> stop following it
            if op2 == OP_LUI and RT(w2) == base_reg:
                break
            if op2 == OP_SPECIAL and RD(w2) == base_reg:
                break
            if op2 in (OP_ADDIU, OP_ORI) and RT(w2) == base_reg:
                break
            if kind and RT(w2) == base_reg and op2 in LOAD_OPS:
                break
            if op2 == OP_JAL:
                break
        if not used:
            reg = io_region(cur)
            hits.append(
                {
                    "lui_at": img.base + i * 4,
                    "access_at": None,
                    "op": "materialise",
                    "addr": cur,
                    "region": reg or "io_other",
                    "reg": REGNAMES[base_reg],
                }
            )
    return hits


# ---------------------------------------------------------------------------
# Probe: BIOS / kernel calls
# ---------------------------------------------------------------------------
#
# The PS1 BIOS is entered through three trampolines at 0xA0/0xB0/0xC0. libapi
# emits:
#       li   $t2, 0xA0     (addiu t2,zero,-0x60 / ori / lui+ori)
#       jr   $t2
#       li   $t1, <func#>  (delay slot)
# We therefore look for `jr rX` where rX was loaded with the constant 0xA0,
# 0xB0 or 0xC0 within a short window, and read the function number from the
# nearby `li $t1`.  Separately we count `syscall` and `break` instructions.

BIOS_VECTORS = {0xA0: "A0", 0xB0: "B0", 0xC0: "C0"}


def _const_loads(words, i, window):
    """Map reg -> immediate value for simple li forms in words[i-window:i]."""
    out = {}
    for j in range(max(0, i - window), i + 2):
        w = words[j][0]
        op = OP(w)
        if op == OP_ADDIU and RS(w) == 0:
            out[RT(w)] = SIMM(w) & 0xFFFFFFFF
        elif op == OP_ORI and RS(w) == 0:
            out[RT(w)] = IMM(w)
        elif op == OP_LUI:
            out[RT(w)] = IMM(w) << 16
    return out


def scan_bios(img: Image, window: int = 8) -> list[dict]:
    hits: list[dict] = []
    n = len(img.data) // 4
    words = list(struct.iter_unpack("<I", img.data[: n * 4]))
    for i in range(n):
        w = words[i][0]
        addr = img.base + i * 4
        if OP(w) == OP_SPECIAL and FUNCT(w) == F_SYSCALL:
            hits.append({"at": addr, "kind": "syscall", "code": (w >> 6) & 0xFFFFF})
            continue
        if OP(w) == OP_SPECIAL and FUNCT(w) == F_BREAK:
            hits.append({"at": addr, "kind": "break", "code": (w >> 6) & 0xFFFFF})
            continue
        if OP(w) == OP_SPECIAL and FUNCT(w) in (F_JR, F_JALR):
            consts = _const_loads(words, i, window)
            tgt = consts.get(RS(w))
            if tgt in BIOS_VECTORS:
                # function number is normally in $t1 (reg 9), set in the delay slot
                fn = None
                if i + 1 < n:
                    d = words[i + 1][0]
                    if OP(d) == OP_ADDIU and RS(d) == 0 and RT(d) == 9:
                        fn = SIMM(d) & 0xFF
                    elif OP(d) == OP_ORI and RS(d) == 0 and RT(d) == 9:
                        fn = IMM(d) & 0xFF
                if fn is None:
                    fn = consts.get(9)
                hits.append(
                    {
                        "at": addr,
                        "kind": "biosvec",
                        "vector": BIOS_VECTORS[tgt],
                        "func": fn,
                    }
                )
    return hits


# ---------------------------------------------------------------------------
# Probe: function boundaries
# ---------------------------------------------------------------------------
#
# Two independent signals, intersected:
#  (a) every `jal target` in the image names a function entry (call graph roots);
#  (b) a classic MIPS prologue `addiu $sp, $sp, -N` starts a non-leaf function.
# We also collect `jr $ra` as function ends. A "function" is reported when we
# can pair an entry with a following `jr $ra`.

def scan_calls(img: Image) -> dict[int, list[int]]:
    """target vaddr -> list of call sites."""
    calls: dict[int, list[int]] = {}
    n = len(img.data) // 4
    for i, (w,) in enumerate(struct.iter_unpack("<I", img.data[: n * 4])):
        if OP(w) == OP_JAL:
            addr = img.base + i * 4
            tgt = ((addr + 4) & 0xF0000000) | (TARGET(w) << 2)
            calls.setdefault(tgt, []).append(addr)
    return calls


def scan_prologues(img: Image) -> list[int]:
    out = []
    n = len(img.data) // 4
    for i, (w,) in enumerate(struct.iter_unpack("<I", img.data[: n * 4])):
        # addiu $sp, $sp, -N
        if OP(w) == OP_ADDIU and RS(w) == 29 and RT(w) == 29 and SIMM(w) < 0:
            out.append(img.base + i * 4)
    return out


def scan_returns(img: Image) -> list[int]:
    out = []
    n = len(img.data) // 4
    for i, (w,) in enumerate(struct.iter_unpack("<I", img.data[: n * 4])):
        if OP(w) == OP_SPECIAL and FUNCT(w) == F_JR and RS(w) == 31:
            out.append(img.base + i * 4)
    return out


def scan_funcs(img: Image) -> list[dict]:
    calls = scan_calls(img)
    prologues = set(scan_prologues(img))
    rets = sorted(scan_returns(img))
    entries = sorted(set(a for a in calls if img.contains(a)) | prologues)
    funcs = []
    import bisect

    for idx, e in enumerate(entries):
        nxt = entries[idx + 1] if idx + 1 < len(entries) else img.base + len(img.data)
        k = bisect.bisect_left(rets, e)
        end = None
        while k < len(rets) and rets[k] < nxt:
            end = rets[k] + 8  # jr $ra + delay slot
            k += 1
        funcs.append(
            {
                "addr": e,
                "end": end,
                "size": (end - e) if end else None,
                "callers": len(calls.get(e, [])),
                "has_prologue": e in prologues,
            }
        )
    return funcs


# ---------------------------------------------------------------------------
# Probe: strings
# ---------------------------------------------------------------------------

PRINTABLE = set(range(0x20, 0x7F)) | {0x09}


def scan_strings(img: Image, minlen: int = 4):
    data = img.data
    out = []
    start = None
    for i, b in enumerate(data):
        if b in PRINTABLE:
            if start is None:
                start = i
        else:
            if start is not None and i - start >= minlen:
                out.append((img.base + start, data[start:i].decode("ascii")))
            start = None
    if start is not None and len(data) - start >= minlen:
        out.append((img.base + start, data[start:].decode("ascii")))
    return out


# ---------------------------------------------------------------------------
# Probe: xrefs to an address
# ---------------------------------------------------------------------------
#
# Finds three reference forms:
#  - a full 32-bit word equal to the address (pointer table / data pointer);
#  - `jal`/`j` whose computed target equals the address;
#  - a lui/addiu or lui/ori pair that materialises the address.

#  - a gp-relative load/store, gp being set by crt0 (see GP below).

# crt0 at the EXE entry point does `lui gp,0x8006 / addiu gp,gp,-21364`.
# Established by disassembling 0x800403A8..0x800403AC.
GP = 0x8005AC8C


def scan_xrefs(img: Image, addr: int, window: int = 16, gp: int | None = GP) -> list[dict]:
    out = []
    n = len(img.data) // 4
    words = [w for (w,) in struct.iter_unpack("<I", img.data[: n * 4])]
    for i, w in enumerate(words):
        at = img.base + i * 4
        if gp is not None:
            mem = LOAD_OPS.get(OP(w)) or STORE_OPS.get(OP(w))
            if mem and RS(w) == 28 and (gp + SIMM(w)) & 0xFFFFFFFF == addr:
                out.append({"at": at, "kind": "gp+" + mem})
                continue
        if w == addr:
            out.append({"at": at, "kind": "word"})
            continue
        op = OP(w)
        if op in (OP_J, OP_JAL):
            tgt = ((at + 4) & 0xF0000000) | (TARGET(w) << 2)
            if tgt == addr:
                out.append({"at": at, "kind": "jal" if op == OP_JAL else "j"})
                continue
        if op == OP_LUI:
            hi = IMM(w) << 16
            reg = RT(w)
            for j in range(i + 1, min(i + 1 + window, n)):
                w2 = words[j]
                op2 = OP(w2)
                if op2 == OP_ADDIU and RS(w2) == reg:
                    if (hi + SIMM(w2)) & 0xFFFFFFFF == addr:
                        out.append({"at": at, "kind": "lui+addiu", "second": img.base + j * 4})
                        break
                if op2 == OP_ORI and RS(w2) == reg:
                    if (hi | IMM(w2)) == addr:
                        out.append({"at": at, "kind": "lui+ori", "second": img.base + j * 4})
                        break
                mem = LOAD_OPS.get(op2) or STORE_OPS.get(op2)
                if mem and RS(w2) == reg:
                    if (hi + SIMM(w2)) & 0xFFFFFFFF == addr:
                        out.append({"at": at, "kind": "lui+" + mem, "second": img.base + j * 4})
                        break
                if op2 == OP_LUI and RT(w2) == reg:
                    break
    return out


def scan_word_refs(img: Image, lo: int, hi: int) -> list[tuple[int, int]]:
    """All aligned words whose value lies in [lo, hi)."""
    return [(a, w) for a, w in img.words() if lo <= w < hi]


# ---------------------------------------------------------------------------
# Probe: indirect hardware access (PsyQ style)
# ---------------------------------------------------------------------------
#
# PsyQ libgpu/libspu/libcd keep the MMIO addresses as CONSTANTS in their data
# segment and dereference them through a pointer:
#       lui  rX, hi(&table)  /  lw rY, lo(&table)(rX)   ; rY = 0x1F801810
#       sw   rZ, 0(rY)
# A purely syntactic lui-scan therefore reports zero MMIO sites. This probe
# closes the gap in two steps:
#   1. find every aligned DATA word whose value is an MMIO address (the table);
#   2. xref the ADDRESS OF that word to find the code that loads it.
# The reported site is the load of the pointer, not the dereference, so it is a
# lower bound on the true access count - but it is exact about WHICH code
# touches hardware, which is what the interpreter scoping question needs.

IO_LO, IO_HI_ADDR = 0x1F801000, 0x1F802000


def scan_hw_indirect(img: Image) -> list[dict]:
    out = []
    for holder, val in img.words():
        phys = val & 0x1FFFFFFF
        if not (IO_LO <= phys < IO_HI_ADDR):
            continue
        refs = scan_xrefs(img, holder)
        out.append(
            {
                "holder": holder,
                "value": val,
                "region": io_region(val) or "io_other",
                "refs": [r["at"] for r in refs],
            }
        )
    return out


# ---------------------------------------------------------------------------
# Probe: region map (code vs data), GT2-style chunk classification
# ---------------------------------------------------------------------------
#
# Per fixed-size chunk compute: zero%, printable-ASCII%, and the fraction of
# words that are not a plausible R3000 encoding. Low implausible% with returns
# and calls => code; high zero% => padding; high ASCII% => strings; else data.

PLAUSIBLE_OPS = set(
    [0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C,
     0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
     0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x28, 0x29, 0x2A, 0x2B, 0x2E,
     0x30, 0x32, 0x38, 0x3A]
)
PLAUSIBLE_SPECIAL = set(
    [0x00, 0x02, 0x03, 0x04, 0x06, 0x07, 0x08, 0x09, 0x0C, 0x0D, 0x10, 0x11,
     0x12, 0x13, 0x18, 0x19, 0x1A, 0x1B, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25,
     0x26, 0x27, 0x2A, 0x2B]
)


def plausible(w: int) -> bool:
    op = OP(w)
    if op == 0x00:
        return FUNCT(w) in PLAUSIBLE_SPECIAL
    if op == 0x01:
        return RT(w) in (0x00, 0x01, 0x10, 0x11)
    return op in PLAUSIBLE_OPS


def region_map(img: Image, chunk: int = 0x400) -> list[dict]:
    out = []
    for off in range(0, len(img.data), chunk):
        blk = img.data[off : off + chunk]
        n = len(blk) // 4
        if n == 0:
            continue
        ws = [w for (w,) in struct.iter_unpack("<I", blk[: n * 4])]
        zero = sum(1 for w in ws if w == 0)
        imp = sum(1 for w in ws if not plausible(w))
        rets = sum(1 for w in ws if OP(w) == OP_SPECIAL and FUNCT(w) == F_JR and RS(w) == 31)
        calls = sum(1 for w in ws if OP(w) == OP_JAL)
        asc = sum(1 for b in blk if b in PRINTABLE) / max(1, len(blk))
        if zero == n:
            kind = "zero"
        elif asc > 0.80:
            kind = "text"
        elif imp / n < 0.03 and (rets or calls >= 2):
            kind = "code"
        elif imp / n < 0.10:
            kind = "code+data"
        else:
            kind = "data"
        out.append(
            {
                "addr": img.base + off,
                "size": len(blk),
                "kind": kind,
                "zero_pct": round(100 * zero / n),
                "imp_pct": round(100 * imp / n),
                "ascii_pct": round(100 * asc),
                "rets": rets,
                "calls": calls,
            }
        )
    return out


def region_runs(img: Image, chunk: int = 0x400) -> list[dict]:
    m = region_map(img, chunk)
    runs = []
    for r in m:
        if runs and runs[-1]["kind"] == r["kind"]:
            runs[-1]["end"] = r["addr"] + r["size"]
        else:
            runs.append({"kind": r["kind"], "addr": r["addr"], "end": r["addr"] + r["size"]})
    for r in runs:
        r["size"] = r["end"] - r["addr"]
    return runs


# ---------------------------------------------------------------------------
# Probe: overlay pointer-table head / load address
# ---------------------------------------------------------------------------
#
# Each overlay file begins with a small word then a run of 0x800xxxxx pointers
# interleaved with data. The overlay is linked to a FIXED address (absolute
# pointers, no relocation records), so the load address L satisfies:
#     for every embedded pointer p that points inside the overlay's own image,
#     0 <= p - L < filesize
# Taking the minimum embedded pointer P_min gives an upper bound L <= P_min,
# and the maximum P_max gives a lower bound L > P_max - filesize.
# We report that bracket; the exact value is pinned from the EXE's loader code.

def overlay_ptr_stats(img: Image) -> dict:
    ptrs = [w for _, w in img.words() if 0x80000000 <= w < 0x80200000]
    ptrs_sorted = sorted(set(ptrs))
    size = len(img.data)
    return {
        "file": img.name,
        "size": size,
        "sha1": img.sha1,
        "id_word": struct.unpack_from("<I", img.data, 0)[0],
        "kseg0_ptr_count": len(ptrs),
        "ptr_min": ptrs_sorted[0] if ptrs_sorted else None,
        "ptr_max": ptrs_sorted[-1] if ptrs_sorted else None,
        "load_addr_upper_bound": ptrs_sorted[0] if ptrs_sorted else None,
        "load_addr_lower_bound": (ptrs_sorted[-1] - size + 1) if ptrs_sorted else None,
    }


def overlay_head_table(img: Image, count: int = 64) -> list[tuple[int, int]]:
    out = []
    for i in range(count):
        (w,) = struct.unpack_from("<I", img.data, i * 4)
        out.append((i * 4, w))
    return out


# ---------------------------------------------------------------------------
# objdump wrapper
# ---------------------------------------------------------------------------


def disassemble(img: Image, addr: int, count: int = 64) -> str:
    start = img.file_off + (addr - img.base)
    stop = start + count * 4
    cmd = [
        OBJDUMP, "-D", "-b", "binary", "-m", "mips:3000", "-EL",
        f"--adjust-vma=0x{img.base - img.file_off:08x}",
        f"--start-address=0x{addr:08x}",
        f"--stop-address=0x{addr + count * 4:08x}",
        img.path,
    ]
    return subprocess.run(cmd, capture_output=True, text=True).stdout


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def cmd_header(args):
    h = exe_header()
    for k, v in h.items():
        print(f"{k:12} {v if not isinstance(v, int) else f'0x{v:08X} ({v})'}")


def cmd_strings(args):
    img = pick_image(args)
    rx = re.compile(args.filter, re.I) if args.filter else None
    for a, s in scan_strings(img, args.min):
        if rx and not rx.search(s):
            continue
        print(f"0x{a:08X}  {s!r}")


def cmd_io(args):
    img = pick_image(args)
    hits = scan_io(img)
    by_region: dict[str, int] = {}
    by_addr: dict[int, int] = {}
    for h in hits:
        by_region[h["region"]] = by_region.get(h["region"], 0) + 1
        by_addr[h["addr"]] = by_addr.get(h["addr"], 0) + 1
    print(f"# {img.name} sha1={img.sha1}")
    print(f"# total direct MMIO accesses: {len(hits)}")
    for r, c in sorted(by_region.items(), key=lambda kv: -kv[1]):
        print(f"  {r:16} {c}")
    print("# distinct registers touched:")
    for a, c in sorted(by_addr.items()):
        print(f"  0x{a:08X}  {io_region(a):16} x{c}")
    if args.verbose:
        print("# sites:")
        for h in hits:
            print(f"  0x{h['access_at']:08X} {h['op']:4} 0x{h['addr']:08X} {h['region']}")


def cmd_hw(args):
    img = pick_image(args)
    hits = scan_hw_indirect(img)
    print(f"# {img.name} sha1={img.sha1}")
    print(f"# MMIO addresses held as data words: {len(hits)}")
    by_region: dict[str, int] = {}
    sites: set[int] = set()
    for h in hits:
        by_region[h["region"]] = by_region.get(h["region"], 0) + 1
        sites.update(h["refs"])
    for r, c in sorted(by_region.items(), key=lambda kv: -kv[1]):
        print(f"  {r:16} {c}")
    print(f"# distinct code sites loading one of them: {len(sites)}")
    if sites:
        print(f"# site address range: 0x{min(sites):08X}..0x{max(sites):08X}")
    if args.verbose:
        for h in hits:
            refs = " ".join(f"0x{r:08X}" for r in h["refs"]) or "-"
            print(f"  0x{h['holder']:08X} = 0x{h['value']:08X} {h['region']:14} refs: {refs}")


def cmd_regions(args):
    img = pick_image(args)
    print(f"# {img.name} sha1={img.sha1} chunk=0x{args.chunk:X}")
    tot: dict[str, int] = {}
    for r in region_runs(img, args.chunk):
        tot[r["kind"]] = tot.get(r["kind"], 0) + r["size"]
        print(f"  0x{r['addr']:08X}..0x{r['end']:08X}  0x{r['size']:06X}  {r['kind']}")
    print("# totals:")
    for k, v in sorted(tot.items(), key=lambda kv: -kv[1]):
        print(f"  {k:12} 0x{v:06X} ({100*v//len(img.data)}%)")


def cmd_tags(args):
    """Find where a 4-character container tag is built as an instruction constant.

    A tag like "DOD3" is compared as the little-endian word 0x33444F44, which the
    compiler materialises as `lui rX,0x3344` + `ori rX,rX,0x4F44`. This is how the
    RMD3 parser was located: the five RMD3-family tags are built at exactly five
    places in the whole game, all inside RASHCDI.
    """
    tags = args.tags.split(",") if args.tags else ["RMD3", "DOD3", "DPD3", "BBD3", "DMD3", "LECT"]
    imgs = {EXE_NAME: load_exe()}
    for nm in OVERLAYS:
        imgs[nm] = load_overlay(nm, OVERLAY_BASE)
    for nm, img in imgs.items():
        n = len(img.data) // 4
        ws = [w for (w,) in struct.iter_unpack("<I", img.data[: n * 4])]
        for tag in tags:
            val = struct.unpack("<I", tag.encode()[:4])[0]
            hi, lo = (val >> 16) & 0xFFFF, val & 0xFFFF
            hits = []
            for i, w in enumerate(ws):
                a = img.base + i * 4
                if w == val:
                    hits.append((a, "word"))
                if OP(w) == OP_LUI and IMM(w) == hi:
                    for j in range(i + 1, min(i + 9, n)):
                        w2 = ws[j]
                        if OP(w2) == OP_ORI and RS(w2) == RT(w) and IMM(w2) == lo:
                            hits.append((a, "lui+ori"))
                            break
                        if OP(w2) == OP_ADDIU and RS(w2) == RT(w) and \
                                ((hi << 16) + SIMM(w2)) & 0xFFFFFFFF == val:
                            hits.append((a, "lui+addiu"))
                            break
            if hits:
                print(f"{nm:14} {tag} (0x{val:08X}): " +
                      " ".join(f"0x{a:08X}/{k}" for a, k in hits))


def cmd_basevote(args):
    """Load-address voting (independent of the loader constant).

    A MIPS `jal` encodes an absolute target, so an overlay's internal call targets are
    readable without knowing where it loads. Each (call target, prologue offset) pair
    votes for base = target - offset; the true base collects one vote per internal call.
    """
    for name in [args.overlay] if args.overlay else OVERLAYS:
        img = load_overlay(name, 0)
        n = len(img.data) // 4
        ws = [w for (w,) in struct.iter_unpack("<I", img.data[: n * 4])]
        tgts, pro = set(), []
        for i, w in enumerate(ws):
            if OP(w) == OP_JAL:
                tgts.add(0x80000000 | (TARGET(w) << 2))
            if OP(w) == OP_ADDIU and RS(w) == 29 and RT(w) == 29 and SIMM(w) < 0:
                pro.append(i * 4)
        votes: dict[int, int] = {}
        for t in tgts:
            for off in pro:
                b = t - off
                if b % 4 == 0 and 0x80010000 <= b < 0x80100000:
                    votes[b] = votes.get(b, 0) + 1
        top = sorted(votes.items(), key=lambda kv: -kv[1])[:5]
        print(f"{name:14} size=0x{len(img.data):06X} jal-targets={len(tgts)} prologues={len(pro)}")
        for b, c in top:
            print(f"     base=0x{b:08X} votes={c}")


def cmd_smc(args):
    """Self-modifying-code scan.

    `lui rX,hi` followed within `--window` instructions by a store using rX as base,
    aborting on any redefinition of rX, on `jal` and on any branch (a stale `lui`
    carried across a basic-block boundary is the dominant false positive); plus every
    gp-relative store. Reports the stores whose target lands in a code run.
    """
    img = pick_image(args)
    runs = list(CODE_RUNS.get(img.name, []))
    if img.name != EXE_NAME:
        # an overlay writing into the resident image's code would be SMC too
        runs += CODE_RUNS[EXE_NAME]
    n = len(img.data) // 4
    ws = [w for (w,) in struct.iter_unpack("<I", img.data[: n * 4])]
    found, total = [], 0
    for i, w in enumerate(ws):
        if OP(w) == OP_LUI and RT(w) != 0:
            hi, reg = IMM(w) << 16, RT(w)
            for j in range(i + 1, min(i + 1 + args.window, n)):
                w2 = ws[j]
                op = OP(w2)
                st = STORE_OPS.get(op)
                if st and RS(w2) == reg:
                    total += 1
                    tgt = (hi + SIMM(w2)) & 0xFFFFFFFF
                    if _in_runs(tgt, runs) or (args.also and _in_runs(tgt, args.also)):
                        found.append((img.base + j * 4, tgt, st))
                    break
                if op in (OP_J, OP_JAL) or op == 1 or 2 <= op <= 7:
                    break
                if op == OP_LUI and RT(w2) == reg:
                    break
                if op in (OP_ADDIU, OP_ORI) and RT(w2) == reg:
                    break
                if op == OP_SPECIAL and (RD(w2) == reg or FUNCT(w2) in (F_JR, F_JALR)):
                    break
                if LOAD_OPS.get(op) and RT(w2) == reg:
                    break
        elif STORE_OPS.get(OP(w)) and RS(w) == 28:
            total += 1
            tgt = (GP + SIMM(w)) & 0xFFFFFFFF
            if _in_runs(tgt, runs):
                found.append((img.base + i * 4, tgt, STORE_OPS[OP(w)]))
    print(f"# {img.name} sha1={img.sha1}")
    print(f"# stores with a statically resolvable target: {total}")
    print(f"# of those landing in a code run: {len(found)}")
    for a, t, k in found:
        print(f"  0x{a:08X} {k} -> 0x{t:08X}")


def cmd_entries(args):
    for e in overlay_entry_points(load_exe()):
        print(f"{e['target']}  owner={e['owner'] or '?':14} sites={len(e['call_sites'])} "
              f"{' '.join(e['call_sites'][:4])}  scores={e['scores']}")


def cmd_ovlcalls(args):
    """Overlay -> resident-EXE calls, and how many land in the PsyQ zone."""
    for name in [args.overlay] if args.overlay else OVERLAYS:
        img = load_overlay(name, OVERLAY_BASE)
        runs = CODE_RUNS[name]
        n = len(img.data) // 4
        t: dict[int, int] = {}
        for i, (w,) in enumerate(struct.iter_unpack("<I", img.data[: n * 4])):
            at = img.base + i * 4
            if not _in_runs(at, runs) or OP(w) != OP_JAL:
                continue
            tgt = ((at + 4) & 0xF0000000) | (TARGET(w) << 2)
            if tgt < OVERLAY_BASE:
                t[tgt] = t.get(tgt, 0) + 1
        psyq = [k for k in t if 0x80040000 <= k < 0x80052400]
        print(f"{name:14} -> resident: {len(t):4} distinct, {sum(t.values()):5} sites; "
              f"PsyQ zone: {len(psyq):3} distinct, {sum(t[k] for k in psyq):4} sites")


def cmd_bios(args):
    img = pick_image(args)
    hits = scan_bios(img)
    kinds: dict[str, int] = {}
    for h in hits:
        k = h["kind"] if h["kind"] != "biosvec" else "vec" + h["vector"]
        kinds[k] = kinds.get(k, 0) + 1
    print(f"# {img.name} sha1={img.sha1}")
    print(f"# total BIOS/kernel entries: {len(hits)}")
    for k, c in sorted(kinds.items()):
        print(f"  {k:10} {c}")
    funcs: dict[str, int] = {}
    for h in hits:
        if h["kind"] == "biosvec":
            key = f"{h['vector']}({h['func']:#04x})" if h["func"] is not None else f"{h['vector']}(?)"
            funcs[key] = funcs.get(key, 0) + 1
    print("# distinct BIOS functions:")
    for k, c in sorted(funcs.items()):
        print(f"  {k:12} x{c}")
    if args.verbose:
        for h in hits:
            print(f"  0x{h['at']:08X} {h}")


def cmd_funcs(args):
    img = pick_image(args)
    fs = scan_funcs(img)
    sized = [f for f in fs if f["size"]]
    print(f"# {img.name} sha1={img.sha1}")
    print(f"# candidate entries: {len(fs)}  with end found: {len(sized)}")
    if sized:
        tot = sum(f["size"] for f in sized)
        print(f"# covered bytes: 0x{tot:X} of 0x{len(img.data):X}")
    print("# top by caller count:")
    for f in sorted(fs, key=lambda f: -f["callers"])[: args.top]:
        print(f"  0x{f['addr']:08X} size={f['size']} callers={f['callers']} prologue={f['has_prologue']}")


def cmd_xref(args):
    img = pick_image(args)
    for h in scan_xrefs(img, args.addr):
        print(f"0x{h['at']:08X}  {h['kind']}")


def cmd_ptrtable(args):
    img = load_overlay(args.overlay)
    st = overlay_ptr_stats(img)
    for k, v in st.items():
        print(f"{k:24} {v if not isinstance(v, int) else f'0x{v:08X} ({v})'}")
    print("# head words:")
    for o, w in overlay_head_table(img, args.count):
        print(f"  +0x{o:04X}  0x{w:08X}")


def cmd_loadaddr(args):
    for name in ([args.overlay] if args.overlay else OVERLAYS):
        img = load_overlay(name)
        st = overlay_ptr_stats(img)
        print(
            f"{name:14} size=0x{st['size']:06X} id={st['id_word']} "
            f"ptrs={st['kseg0_ptr_count']:6} "
            f"min=0x{st['ptr_min']:08X} max=0x{st['ptr_max']:08X} "
            f"=> L in (0x{st['load_addr_lower_bound']:08X} .. 0x{st['load_addr_upper_bound']:08X}]"
        )


def cmd_dis(args):
    img = pick_image(args)
    print(disassemble(img, args.addr, args.count))


def pick_image(args) -> Image:
    name = getattr(args, "file", None)
    if not name or name.upper().startswith("SLUS"):
        return load_exe()
    base = getattr(args, "base", None)
    return load_overlay(name, base)


def build_parser():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    def common(sp):
        sp.add_argument("--file", default=EXE_NAME, help="image file name inside work/disc_us")
        sp.add_argument("--base", type=lambda s: int(s, 0), default=None, help="load address for an overlay")
        sp.add_argument("-v", "--verbose", action="store_true")
        return sp

    sub.add_parser("header").set_defaults(func=cmd_header)

    sp = common(sub.add_parser("strings"))
    sp.add_argument("--min", type=int, default=4)
    sp.add_argument("--filter", default=None)
    sp.set_defaults(func=cmd_strings)

    common(sub.add_parser("io")).set_defaults(func=cmd_io)
    common(sub.add_parser("hw")).set_defaults(func=cmd_hw)

    sp = common(sub.add_parser("regions"))
    sp.add_argument("--chunk", type=lambda s: int(s, 0), default=0x400)
    sp.set_defaults(func=cmd_regions)
    common(sub.add_parser("bios")).set_defaults(func=cmd_bios)

    sp = common(sub.add_parser("funcs"))
    sp.add_argument("--top", type=int, default=30)
    sp.set_defaults(func=cmd_funcs)

    sp = common(sub.add_parser("xref"))
    sp.add_argument("--addr", type=lambda s: int(s, 0), required=True)
    sp.set_defaults(func=cmd_xref)

    sp = sub.add_parser("ptrtable")
    sp.add_argument("overlay")
    sp.add_argument("--count", type=int, default=48)
    sp.set_defaults(func=cmd_ptrtable)

    sp = sub.add_parser("loadaddr")
    sp.add_argument("overlay", nargs="?")
    sp.set_defaults(func=cmd_loadaddr)

    sp = sub.add_parser("tags")
    sp.add_argument("--tags", default=None, help="comma-separated 4-char tags")
    sp.set_defaults(func=cmd_tags)

    sp = sub.add_parser("basevote")
    sp.add_argument("overlay", nargs="?")
    sp.set_defaults(func=cmd_basevote)

    sp = common(sub.add_parser("smc"))
    sp.add_argument("--window", type=int, default=6)
    sp.set_defaults(func=cmd_smc, also=None)

    sub.add_parser("entries").set_defaults(func=cmd_entries)

    sp = sub.add_parser("ovlcalls")
    sp.add_argument("overlay", nargs="?")
    sp.set_defaults(func=cmd_ovlcalls)

    sp = common(sub.add_parser("dis"))
    sp.add_argument("--addr", type=lambda s: int(s, 0), required=True)
    sp.add_argument("--count", type=int, default=64)
    sp.set_defaults(func=cmd_dis)

    sp = sub.add_parser("emit-json")
    sp.add_argument("--out", default=os.path.join(ROOT, "db", "exe.json"))
    sp.set_defaults(func=lambda a: cmd_emit_json(a))

    return p


# ---------------------------------------------------------------------------
# Machine-readable fact emission (db\exe.json)
# ---------------------------------------------------------------------------
#
# Only facts this file can re-derive from the bytes go in automatically. Facts
# that took a human reading of a disassembly (function roles, the state machine,
# the overlay ABI) are carried in HAND_FACTS below, each with its own provenance
# string naming the addresses that were read.

OVERLAY_BASE = 0x8005B5E8            # proven: the overlays' load address
BSS_START = 0x8005B1F0
BSS_END = 0x800DC0C8

# code runs, from `exe.py regions --chunk 0x400` (EXE) / `--chunk 0x1000` (overlays)
CODE_RUNS = {
    # 0x80055C00..0x80056C00 and 0x80059800..0x8005A800 look like code to the chunk
    # classifier (0-1% implausible words) but contain ZERO `jr $ra` and zero stack
    # prologues while ~46% of their words fall in the 0x0C000000..0x0FFFFFFF band that
    # decodes as `jal`. They are numeric tables, not code, and are excluded here.
    # 0x80010000..0x80010028 is data (a 0xFFFFFFFF marker at +0x10, zero elsewhere);
    # hand-written fixed-point assembly starts at 0x80010028.
    EXE_NAME: [(0x80010028, 0x80010400), (0x80011800, 0x80052400)],
    "RASHCDF.BIN": [(0x8005D5E8, 0x800805E8)],
    "RASHCDG.BIN": [(0x8005B5E8, 0x800CB5E8)],
    "RASHCDI.BIN": [(0x8005C5E8, 0x8006B5E8)],
}


def overlay_entry_points(exe_img: Image) -> list[dict]:
    """Every `jal` from resident EXE code that lands in the overlay window.

    This is the resident -> overlay ABI. There is no dispatch table: the calls
    are plain absolute `jal`s, so each address is only meaningful while the
    matching overlay is resident. Attribution is by a plausibility test: does
    the target look like a function entry in that overlay's image?
    """
    sizes = {"RASHCDI.BIN": 0x1355C, "RASHCDF.BIN": 0x45520, "RASHCDG.BIN": 0x72088}
    ovl = {n: load_overlay(n, OVERLAY_BASE) for n in sizes}
    runs = CODE_RUNS[EXE_NAME]
    n = len(exe_img.data) // 4
    words = [w for (w,) in struct.iter_unpack("<I", exe_img.data[: n * 4])]
    tgt: dict[int, list[int]] = {}
    for i, w in enumerate(words):
        at = exe_img.base + i * 4
        if not _in_runs(at, runs) or OP(w) != OP_JAL:
            continue
        t = ((at + 4) & 0xF0000000) | (TARGET(w) << 2)
        if t >= OVERLAY_BASE:
            tgt.setdefault(t, []).append(at)

    def entry_score(img: Image, a: int):
        if not img.contains(a) or not img.contains(a + 0x3C):
            return None
        ws = [struct.unpack_from("<I", img.data, img.off(a) + 4 * k)[0] for k in range(16)]
        if all(w == 0 for w in ws):
            return 0
        pro = 1 if (OP(ws[0]) == OP_ADDIU and RS(ws[0]) == 29 and RT(ws[0]) == 29 and SIMM(ws[0]) < 0) else 0
        return round(100 * (0.6 * sum(1 for w in ws if plausible(w)) / 16 + 0.4 * pro))

    out = []
    for t in sorted(tgt):
        sc = {nm: entry_score(ovl[nm], t) for nm in sizes}
        best = max([v for v in sc.values() if v is not None], default=None)
        owners = [nm for nm, v in sc.items() if v is not None and v == best]
        out.append(
            {
                "target": _hx(t),
                "call_sites": [_hx(a) for a in tgt[t]],
                "scores": {nm: v for nm, v in sc.items()},
                "owner": owners[0] if len(owners) == 1 else None,
                "owner_candidates": owners,
            }
        )
    return out


def _in_runs(addr, runs):
    return any(lo <= addr < hi for lo, hi in runs)


def _hx(v):
    return None if v is None else f"0x{v:08X}"


HAND_FACTS = {
    EXE_NAME: {
        "functions": [
            (0x8004032C, "crt0_entry", "identified",
             "PS-X EXE header pc0. Disassembled 0x8004032C..0x800403C8: zero-fills "
             "0x8005B1F0..0x800DC0C8, sets sp from *0x8005AC98-8, sets gp=0x8005AC8C, "
             "stores heap base/size to 0x800548D4/0x800548D8, then jal 0x80012224, then break."),
            (0x80012224, "main", "identified",
             "Sole jal target of crt0 at 0x800403C0. Disassembled 0x80012224..0x80012520: "
             "top-level state machine over the byte at offset 0 of *0x8005B2F8."),
            (0x800118A0, "load_overlay", "identified",
             "Disassembled 0x800118A0..0x800119BC. a0 = overlay bit (2/4/8/16), a1 = destination. "
             "Selects one of the four name strings at 0x8001075C/68/74/80, formats it with the "
             "'%s' at 0x8005AC94, calls the file reader at 0x80014680, then FlushCache "
             "(0x80043D44) between interrupt disable/enable (0x80043DD4/0x80043DF4), then "
             "*0x8005ACA8 |= a0."),
            (0x800119C0, "race_frame_step", "disassembled",
             "jal from main at 0x80012348, immediately after the RASHCDG load. Disassembled "
             "0x800119C0..0x80011A90: reads *0x800DE4D0, calls RASHCDG entries 0x800C4550, "
             "0x8008B99C/0x8008BA18/0x80093F94. Role is inferred from the call context."),
            (0x80014680, "file_read_device_to_buf", "identified",
             "Disassembled 0x80014680..0x8001473C. Signature (name, device, buf, &size) taken "
             "from the diagnostic format string at 0x80010820, which it passes to the printf at "
             "0x80044894 with exactly those three values."),
            (0x80023714, "build_race_stp_name", "identified",
             "Disassembled 0x80023714..0x80023798: sprintf (0x80043FD4) of the format string at "
             "0x80010BC8 with *(*0x8005B2F8 + 0x30) and *(*0x8005B2F8 + 0x40)."),
            (0x80043D44, "bios_FlushCache", "verified",
             "Disassembled: li t2,0xA0 / jr t2 / li t1,0x44 - the standard A0(0x44) thunk shape."),
            (0x80043DA8, "bios_EnterCriticalSection", "verified",
             "Disassembled 0x80043DA4: li a0,1 followed by syscall."),
            (0x80043DD4, "irq_disable", "verified",
             "Disassembled: mfc0 a0,SR / and a0,~0x401 / mtc0 a0,SR / jr ra."),
            (0x80043DF4, "irq_enable", "verified",
             "Disassembled: mfc0 a0,SR / ori a0,0x401 / mtc0 a0,SR / jr ra."),
            (0x80043FD4, "sprintf", "identified",
             "Called with (buffer, format, args...) at 0x80011934, 0x80023738 and 173 other "
             "sites; the second argument is always a printf format string."),
            (0x80044894, "printf", "identified",
             "Called with a diagnostic format string as a0 at 0x80011968 and 0x800146C8."),
        ],
        "globals": [
            (0x8005ACA8, "overlay_resident_mask", "identified",
             "Written at 0x80011924 (= 1) and 0x800119A0 (|= overlay bit) inside load_overlay; "
             "read at 0x80011990, 0x80019990, 0x8002CC28. File image holds 1."),
            (0x80010E34, "overlay_load_address", "verified",
             "Word in the EXE image = 0x8005B5E8. Read at 0x8001227C/0x800122C8/0x800122E8/"
             "0x80012308/0x80012490 and passed as load_overlay's a1 at every one of the five "
             "call sites."),
            (0x8005B2F8, "game_state_ptr", "identified",
             "Written once at 0x80011744; read at 46 sites. Byte at +0 is the main-loop state, "
             "byte at +2 a request flag, word at +0x30 the road/player set (1 or 2), word at "
             "+0x40 the race index."),
            (0x8005AC8C, "gp_base", "verified",
             "Set by crt0 at 0x800403A8..0x800403AC; 761 gp-relative memory ops in the EXE "
             "resolve against it."),
            (0x8002FDEC, "bind_model_instance", "identified",
             "Disassembled 0x8002FDEC..0x8002FF04. Bounds the slot with slti 50 against the model "
             "registry at 0x800CE1B0, stores the registry entry at obj+0x60, the last group's DOD3 "
             "at obj+0x00, groupCount-1 at obj+0x08/+0x0A/+0x0B, and mallocs "
             "subMeshCount(group 0)*24 into obj+0x04."),
            (0x800292A0, "gte_transform_quad", "disassembled",
             "Disassembled 0x800292A0..0x80029360: lwc2 of a 4-entry SVECTOR array, RTPS "
             "(0x0180001), RTPT (0x0280030), AVSZ4 (0x168002E), swc2 of the screen XY and OTZ. "
             "Near-identical copies at 0x80029600 and 0x800266C0."),
            (0x8004D180, "libgte_RotTransPers4", "probable",
             "GTE cluster RTPS/RTPT/RTPS/AVSZ4 at 0x8004D1C0..0x8004D250, inside the PsyQ zone; "
             "shape matches libgte RotTransPers4. Not corroborated by a string."),
            (0x80048A6C, "libgpu_LoadImage", "probable",
             "Called at RASHCDI 0x8005DF80 with a RECT built on the stack and a pixel pointer, "
             "immediately after the VRAM tile rectangle is computed. Inside the PsyQ zone."),
            (0x80052400, "device_path_table", "probable",
             "12 slots of 0x14 bytes at 0x80052400..0x800524F0, each an ASCII directory prefix. "
             "Only one direct code reference (0x8001B5FC -> slot 4); the slots are otherwise "
             "filled/consumed at runtime, so the indexing was not established statically."),
        ],
    },
    "RASHCDF.BIN": {
        "functions": [
            (0x8007FEDC, "fe_entry_a", "identified",
             "jal from the resident EXE at 0x80012288, immediately after load_overlay(16)."),
            (0x8007FF4C, "fe_entry_b", "identified",
             "jal from the resident EXE at 0x800122DC, immediately after load_overlay(16)."),
        ],
        "globals": [
            (0x8008973C, "fe_resource_name_table", "identified",
             "42 consecutive words at 0x8008973C..0x800897E0, each pointing into the string pool "
             "at 0x8005C618..0x8005C857; entries 0..10 are .wve, 11..20 screen .str, 21..41 "
             "course thumbnail .str."),
            (0x800897EC, "fe_resource_record_table", "probable",
             "23 records of 0x20 bytes at 0x800897EC..0x80089ACC whose first field is an ASCII "
             "file name (.str/.psh/.loc/.pfn). Record layout beyond the name not established."),
            (0x800810D8, "mdecout_dma_chcr_ptr", "identified",
             "Data word = 0x1F801098 (DMA1/MDECout CHCR); loaded at 0x80061298 and tested "
             "against 0x01000000 (busy bit) at 0x800612AC."),
        ],
    },
    "RASHCDG.BIN": {
        "functions": [
            (0x8009C5E4, "dod3_group_subtype", "identified",
             "Disassembled 0x8009C5E4..0x8009C650. f(groupIndex) -> "
             "(*(u16*)(registry[cur].groups[groupIndex].dod3 + 0x0E) & 0xF80) >> 7, or -1 when the "
             "index is out of range. Called from 0x8009C9F0. Independently confirms the "
             "12-byte group-descriptor stride and that field +0x00 is the DOD3 pointer."),
        ],
        "globals": [],
    },
    "RASHCDI.BIN": {
        "functions": [
            (0x80064610, "level_entry_a", "identified",
             "jal from the resident EXE at 0x800122F4, immediately after load_overlay(2)."),
            (0x80063B90, "level_entry_b", "identified",
             "jal from the resident EXE at 0x800122FC, immediately after load_overlay(2)."),
            (0x80063A20, "level_entry_c", "identified",
             "jal from the resident EXE at 0x8001249C, immediately after load_overlay(2)."),
            (0x80063FA0, "level_entry_d", "identified",
             "jal from the resident EXE at 0x800124C0 on state 5."),
            (0x8005E848, "level_entry_e", "probable",
             "jal from the resident EXE at 0x8001211C; attributed to RASHCDI by the "
             "function-entry plausibility test (100 for I, 60 for F and G)."),
            (0x8005C0C4, "rmd3_walk_chunks", "identified",
             "Disassembled 0x8005C0C4..0x8005C294. Compares the word at chunk+0 against the tag "
             "constants built by lui+ori at 0x8005C120 (RMD3), 0x8005C158 (DOD3), 0x8005C164 "
             "(DPD3) and 0x8005C144 (BBD3), and advances by the size word at chunk+4. These five "
             "lui+ori pairs are the only places in any of the four images that build an RMD3 "
             "family tag."),
            (0x8005CB9C, "rmd3_register_object", "identified",
             "Disassembled 0x8005CB9C..0x8005CC48. Finds a free slot in the 50-entry registry at "
             "0x800CE1B0 (stride 0x10, slti 50 at 0x8005CC28), stores modelId at +0x00 and the "
             "RMD3+0x0C group count at +0x04, and mallocs groupCount*12 group descriptors "
             "into +0x08."),
            (0x8005CC4C, "dod3_register_group", "identified",
             "Disassembled 0x8005CC4C..0x8005CD30. Mallocs subMeshCount*4 DPD3 pointers into "
             "groups[g]+0x04, stores the DOD3 pointer into groups[g]+0x00, and at "
             "0x8005CCBC..0x8005CD28 REWRITES the five region offsets at DOD3+0x20/24/28/2C/30 "
             "into absolute pointers in place (0 preserved as 0)."),
            (0x8005CD60, "dpd3_register_submesh", "identified",
             "Disassembled 0x8005CD60..0x8005CE58. Looks the modelId up in the registry, then "
             "stores the DPD3 pointer at groups[g].submeshes[DPD3+0x0D]."),
            (0x8005CE78, "bbd3_register", "disassembled",
             "Disassembled 0x8005CE78..0x8005CEC8; same registry lookup then a per-group store."),
            (0x8005C298, "tex_open_and_upload", "identified",
             "Disassembled 0x8005C298..0x8005C444. Opens the file through 0x80014654, checks the "
             "word at +0 against the LECT tag 0x5443454C built by lui+ori at 0x8005C2C0, and "
             "calls 0x8005DDB8."),
            (0x8005DDB8, "lect_alloc_vram_page", "identified",
             "Disassembled 0x8005DDB8..0x8005DF98. Dispatches on the byte at LECT+0x0C (1..6) "
             "through the jump table at 0x8005B6A4; the kind-1 path computes a 64x60 VRAM "
             "rectangle from the config table at 0x800533B4 fields +0x39/+0x3A, assembles the GPU "
             "tpage word at 0x8005DF44..0x8005DF6C, stores a 12-byte entry into the page table at "
             "0x800D5F70+0x18, and uploads with 0x80048A6C. "),
        ],
        "globals": [
            (0x800CE1B0, "model_registry", "identified",
             "50 slots of 0x10 bytes. Written by 0x8005CB9C/0x8005CC4C/0x8005CD60, read by the "
             "resident EXE at 0x8002FE18 and by RASHCDG at 0x8009C5F8. "),
            (0x800D5F70, "texture_page_table", "identified",
             "Runtime VRAM page table written by 0x8005DDB8. Kind-1 entries at +0x18 with the "
             "count at +0x198; kind-2 entries at +0x138 with the count at +0x19C; stride 12; "
             "entry+0x08 holds the ready-made GPU tpage word. Referenced from 10 sites in "
             "RASHCDI and from NO site in RASHCDG or the resident EXE."),
            (0x8005B6A4, "lect_kind_jump_table", "identified",
             "6-entry jump table for the LECT kind byte; bounded by sltiu 6 at 0x8005DDE8 and "
             "entered by jr at 0x8005DE10."),
            (0x800533B4, "vram_layout_config", "probable",
             "Stride 0x58, indexed by *(game_state+0x30) - 1. Only fields +0x39 (base Y) and "
             "+0x3A (bits 0-3 base page X, bit 4 = +256 Y) were read, at "
             "0x8005DEC8..0x8005DF20. The rest of the record is undecoded."),
        ],
    },
}


def _image_facts(name: str) -> dict:
    img = load_exe() if name == EXE_NAME else load_overlay(name, OVERLAY_BASE)
    runs = CODE_RUNS[name]
    inr = lambda a: _in_runs(a, runs)

    bios = [h for h in scan_bios(img) if inr(h["at"])]
    # `break` and `syscall` words are common inside data; keep only those in code runs
    io = scan_io(img)
    io_code = [h for h in io if inr(h["access_at"] or h["lui_at"])]
    mmio = [h for h in io_code if h["region"] != "scratchpad" and h["access_at"]]
    scratch = [h for h in io_code if h["region"] == "scratchpad" and h["access_at"]]
    indirect = scan_hw_indirect(img)
    indirect_sites = sorted({r for h in indirect for r in h["refs"] if inr(r)})
    funcs = [f for f in scan_funcs(img) if inr(f["addr"])]

    hand = HAND_FACTS.get(name, {"functions": [], "globals": []})

    return {
        "file": name,
        "sha1": img.sha1,
        "size": len(open(img.path, "rb").read()),
        "load_address": _hx(img.base),
        "image_size": _hx(len(img.data)),
        "code_runs": [
            {"addr": _hx(lo), "end": _hx(hi), "size": _hx(hi - lo)} for lo, hi in runs
        ],
        "code_runs_provenance":
            "tools\\scout\\exe.py regions: per-chunk share of words that are not a plausible "
            "R3000 encoding, plus jr-$ra and jal counts. Boundaries are chunk-granular estimates.",
        "regions": [
            {"addr": _hx(r["addr"]), "end": _hx(r["end"]), "kind": r["kind"]}
            for r in region_runs(img, 0x400 if name == EXE_NAME else 0x1000)
        ],
        "bios_call_sites": {
            "count": len(bios),
            "by_kind": {
                k: sum(1 for h in bios if (h["kind"] if h["kind"] != "biosvec" else "vec" + h["vector"]) == k)
                for k in sorted({(h["kind"] if h["kind"] != "biosvec" else "vec" + h["vector"]) for h in bios})
            },
            "sites": [
                {
                    "addr": _hx(h["at"]),
                    "kind": h["kind"],
                    "vector": h.get("vector"),
                    "func": (None if h.get("func") is None else f"0x{h['func']:02X}"),
                }
                for h in bios
            ],
            "provenance":
                "tools\\scout\\exe.py bios: `jr rX` where rX was loaded with 0xA0/0xB0/0xC0 "
                "within 8 instructions, function number read from the `li $t1` in the delay "
                "slot; plus syscall/break words. Restricted to the code runs above, because "
                "both encodings occur frequently inside data.",
        },
        "io_sites": {
            "direct_mmio": [
                {"addr": _hx(h["access_at"]), "op": h["op"], "target": _hx(h["addr"]), "region": h["region"]}
                for h in mmio
            ],
            "direct_mmio_count": len(mmio),
            "scratchpad_count": len(scratch),
            "indirect_pointer_holders": [
                {"holder": _hx(h["holder"]), "target": _hx(h["value"]), "region": h["region"],
                 "code_refs": [_hx(r) for r in h["refs"] if inr(r)]}
                for h in indirect if any(inr(r) for r in h["refs"])
            ],
            "indirect_site_count": len(indirect_sites),
            "indirect_site_range": [_hx(min(indirect_sites)), _hx(max(indirect_sites))] if indirect_sites else None,
            "provenance":
                "tools\\scout\\exe.py io (lui of an I/O page followed by a load/store on that "
                "register) and tools\\scout\\exe.py hw (aligned data words whose value is an "
                "MMIO address, cross-referenced back to the code that loads them, including "
                "gp-relative loads with gp = 0x8005AC8C). The indirect count is a LOWER bound: "
                "it counts pointer loads, not dereferences.",
        },
        "candidate_functions": {
            "count": len(funcs),
            "with_prologue": sum(1 for f in funcs if f["has_prologue"]),
            "provenance":
                "tools\\scout\\exe.py funcs: union of in-image `jal` targets and "
                "`addiu $sp,$sp,-N` prologues, paired with the next `jr $ra`. Heuristic: leaf "
                "functions without a prologue that are never called by `jal` are missed, and "
                "`jal` targets inside data are false positives.",
        },
        "facts": (
            [
                {"address": _hx(a), "name": n, "kind": "function", "status": s, "evidence": e}
                for a, n, s, e in hand["functions"]
            ]
            + [
                {"address": _hx(a), "name": n, "kind": "data", "status": s, "evidence": e}
                for a, n, s, e in hand["globals"]
            ]
        ),
    }


def cmd_emit_json(args):
    exe_hdr = exe_header()
    doc = {
        "schema": 1,
        "generator": "tools\\scout\\exe.py emit-json",
        "game": "Road Rash: Jailbreak (USA), SLUS_01053",
        "source": "work\\disc_us (an extract of the player's own disc); no bytes are copied here",
        "status_vocabulary": {
            "verified": "reproduced from the bytes by more than one independent probe",
            "identified": "role established by reading our own disassembly",
            "disassembled": "instructions read, role inferred from call context only",
            "probable": "consistent with the bytes but not proven; see the doc",
            "heuristic": "output of a scan whose false-positive mode is documented",
            "unknown": "explicitly not established; the evidence field says what was tried",
        },
        "images": {},
        "memory_map": {
            "exe_text": {"addr": _hx(exe_hdr["text_addr"]),
                         "end": _hx(exe_hdr["text_addr"] + exe_hdr["text_size"]),
                         "size": _hx(exe_hdr["text_size"]),
                         "provenance": "PS-X EXE header words at file offsets 0x18/0x1C"},
            "bss": {"addr": _hx(BSS_START), "end": _hx(BSS_END),
                    "provenance": "crt0 zero-fill loop disassembled at 0x8004032C..0x8004034C"},
            "overlay_window": {"addr": _hx(OVERLAY_BASE),
                               "end": _hx(OVERLAY_BASE + 0x72088),
                               "provenance":
                                   "constant at 0x80010E34, passed as load_overlay's a1 at all "
                                   "five call sites; independently confirmed by call-target "
                                   "voting inside each overlay (see the doc)"},
            "heap": {"addr": _hx(BSS_END), "size": _hx(0x0011BF30),
                     "provenance":
                         "crt0 0x80040368..0x8004039C: base = end of bss, size = sp - 0x2000 - base"},
            "sp_at_main": {"addr": _hx(0x801FFFF8),
                           "provenance": "crt0 0x80040350..0x80040364, from *0x8005AC98 = 0x00200000"},
        },
        "overlays": {
            "load_address": _hx(OVERLAY_BASE),
            "mutually_exclusive": True,
            "mutual_exclusion_evidence":
                "All three are loaded to the same address (0x80010E34). load_overlay resets "
                "*0x8005ACA8 to 1 at 0x80011924 before loading ids 2, 4 and 16, then ORs the id "
                "in at 0x800119A0.",
            "selector_ids": {"2": "RASHCDI.BIN", "4": "RASHCDG.BIN", "8": "rashcdr.bin (absent)",
                             "16": "RASHCDF.BIN"},
            "selector_provenance":
                "Disassembly of the comparison chain at 0x800118B4..0x8001191C against the four "
                "name strings at 0x8001075C, 0x80010768, 0x80010774, 0x80010780.",
            "relocation": "none",
            "relocation_evidence":
                "load_overlay reads the file straight to the fixed address and then calls only "
                "FlushCache (0x80043D44) between irq_disable/irq_enable; there is no relocation "
                "pass and no relocation table in any overlay file.",
        },
    }
    doc["rmd3_draw"] = {
        "note":
            "Findings from the RMD3 draw path. "
            "Overlay addresses assume base 0x8005B5E8 and the overlay named in `image`.",
        "dod3_flags": [
            {
                "field": "bit 30 of DOD3+0x0C",
                "meaning": "group is stored in world units; clear = reduced unit ~1/16",
                "status": "established",
                "evidence":
                    "Re-walked all 112 .GEO files (1792 groups). Classifying groups by radius "
                    "(DOD3+0x10) relative to the largest group of the same object, bit 30 is set "
                    "in 550 of 556 large-unit groups and in 5 of 1197 small-unit groups; no other "
                    "bit separates them. Over the 495 objects containing both kinds, "
                    "radius(set)/radius(clear) has n=1003, median 16.58, mean 17.10, range "
                    "12.82..21.17, sd 1.44 - so the factor is an engine constant with authoring "
                    "slack, not a per-model field. The code applying it was NOT found.",
            },
            {
                "field": "(DOD3+0x0E & 0x78) >> 3",
                "meaning": "object class code, values 1..6, constant across an object's groups",
                "status": "established",
                "evidence":
                    "Read by RASHCDI 0x8005C408 (argument to 0x8005BD80 with the model slot), "
                    "0x8005C42C, 0x8005CB20, 0x8005CB44; 7 sites in the EXE and 8 in RASHCDG. "
                    "Data: 1410 of 1792 groups have value 3, and the value never varies between "
                    "the groups of one object.",
            },
            {
                "field": "(DOD3+0x0E & 0xF80) >> 7",
                "meaning": "prop sub-type code, values 0..5; NOT a LOD or scale selector",
                "status": "established",
                "evidence":
                    "Read by RASHCDG 0x8009C630 (accessor 0x8009C5E4) and 8 other overlay sites "
                    "plus EXE 0x800207FC. Data: 0 in 1570 of 1792 groups, non-zero almost only in "
                    "HAZARD*.GEO, and cross-tabulating against the group index shows no "
                    "correlation, which rules out the LOD-scale reading.",
            },
        ],
        "dod3_scale_0x14": {
            "status": "unknown",
            "evidence":
                "No reader found in any of the four images. Scan: 32-instruction windows holding "
                "lw rX,0x14(rY) and lw rZ,0x24(rY) on the same non-sp, non-gp base - 4 candidates "
                "in the EXE, 3 in RASHCDG, 2 in RASHCDI, 0 in RASHCDF, all inspected and all "
                "unrelated structures. Value is 4096 in 1786 groups, 3932 in 4 and 4915 in 2 (all "
                "six in PED01A.GEO). The 12.12 reading is neither confirmed nor refuted; a read "
                "through a struct-held pointer would not be seen by this scan.",
        },
        "texture_page": {
            "status": "established",
            "image": "RASHCDI.BIN",
            "allocator": "0x8005DDB8",
            "page_table": "0x800D5F70",
            "entry_stride": 12,
            "kind1": {"entries": "0x800D5F88", "count_at": "0x800D6108", "tile_w": 64, "tile_h": 60},
            "kind2": {"entries": "0x800D60A8", "count_at": "0x800D610C", "tile_w": 32, "tile_h": 64},
            "rect_formula":
                "x = ((cfg[0x3A] & 0x0F) << 6) + ((n << 4) & 0x3C0); "
                "y = cfg[0x39] + ((cfg[0x3A] & 0x10) << 4) + (n & 3) * 60; "
                "cfg = 0x800533B4 + 0x58 * (*(game_state+0x30) - 1)",
            "tpage_formula":
                "tpage = ((y & 0x100) >> 4) | ((x & 0x3FF) >> 6) | 0x80 | ((y & 0x200) << 2); "
                "bit 7 set = 8-bit CLUT colour mode",
            "evidence":
                "Disassembled 0x8005DDB8..0x8005DF98. Rect built at 0x8005DEC8..0x8005DF20, tpage "
                "word assembled at 0x8005DF44..0x8005DF6C and stored at entry+0x08 by the sh at "
                "0x8005DF6C, upload by jal 0x80048A6C at 0x8005DF80.",
        },
        "prim_tpage_lookup": {
            "status": "unknown",
            "evidence":
                "The page table 0x800D5F70 is referenced from 10 sites in RASHCDI (0x8005DAEC, "
                "0x8005DE18, 0x8005DE58, 0x8005E090, 0x8005E0D0, 0x8005E314, 0x8005E6D8, "
                "0x8005E854, 0x8005E8E0, 0x8005E900) and from no site in RASHCDG or the resident "
                "EXE. A search for an in-place patch of the .GEO primitives (a loop with "
                "addiu rX,rX,20 near an sh rZ,6(rY)) found no convincing candidate in any image.",
        },
        "prim_draw_loop": {
            "status": "unknown",
            "evidence":
                "No image contains a 48-instruction window with lw at {0,4,8} plus lhu at "
                "{12,14,16,18} on one base register, and RASHCDG contains no lw +0x18 together "
                "with addiu +0x1C on the same base. A linear same-base scan cannot follow a "
                "scheduled loop whose base register changes between loads; this needs data-flow "
                "analysis or an interpreter watchpoint.",
        },
    }
    doc["overlays"]["entry_points"] = overlay_entry_points(load_exe())
    doc["overlays"]["entry_points_provenance"] = (
        "tools\\scout\\exe.py emit-json / overlay_entry_points(): every `jal` from resident EXE "
        "code whose target is >= 0x8005B5E8. `owner` is the overlay in which the target scores "
        "highest on a function-entry plausibility test (prologue + share of decodable words over "
        "16 instructions); a null owner means the test could not separate the candidates, which "
        "happens for leaf functions with no stack prologue."
    )
    for name in [EXE_NAME] + OVERLAYS:
        doc["images"][name] = _image_facts(name)

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=2)
    print(f"wrote {args.out}")


def main(argv=None):
    args = build_parser().parse_args(argv)
    args.func(args)


if __name__ == "__main__":
    main()
