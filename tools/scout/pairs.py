"""Scout probe for the CONTACT PAIRS of Road Rash: Jailbreak (USA, SLUS_01053).

The subject is the pair half of the collision pass `RASHCDG 0x800A4774` (the bike-against-bike
enumerator, the kind loop and its partner dispatch), the bike pair `0x800AB7A0` and its classifier
`0x800ABE78`, the box mover `0x800A8DF0`, the heading re-derivation `0x800A8FE8` and the camera-box
gate `0x800A8C78` (modelled in this file), and the responses, the partner resolvers, the deferred
reactions and the road-wall collision `0x800B3AD0` (modelled in pairs_models.py).

This probe re-derives their behaviour independently of the C++ tree:

  * SKELETON: both image hashes, the `jal` word at every call site the transcription names, the three
    partner jump tables, the literal words the arithmetic rests on;
  * LIVE: the transcriptions are run as MODELS against the real functions executing in our
    interpreter. Snapshot copies are made under work\\pairs\\probe whose only difference from the original
    savestate is DATA - two entities placed in contact by editing positions (bikes) or road-slice
    fields (the traffic car). The original code is never changed. For every call of a modelled
    function the RAM is rebuilt from the snapshot plus every watched write before the call, the model
    runs, and it is compared with the interpreter on every write it makes outside its own stack
    frame (in order), on the exact sequence of calls it makes (target and argument registers) and
    on its return value. Callees are not modelled: their recorded writes and return value are
    replayed (the same oracle seam the bench uses);
  * CONTACT: facts about what a contact does, read from the same runs (the box displacement equals
    the logged impulse, the contact record, the flag bits, the integrated speed jump from a car).

Reads ONLY work\\disc_us and work\\oracle\\state; writes ONLY under work\\pairs\\probe (snapshot copies,
trace outputs). Uses <build>\\rrverify.exe read-only. Never copies game bytes into the repo.

    python tools\\scout\\pairs.py info            # skeleton only
    python tools\\scout\\pairs.py live [--cover]  # the model-vs-interpreter runs (+ model lines never run)
    python tools\\scout\\pairs.py verify          # pairs: <n> checks, <m> failures
    python tools\\scout\\pairs.py verify --mutate # each mutated claim must fail its own check
"""

from __future__ import annotations

import argparse
import bisect
import csv
import os
import re
import shutil
import struct
import subprocess
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import exe as E  # noqa: E402

ROOT = E.ROOT
STATE_DIR = os.path.join(ROOT, "work", "oracle", "state")
WORK = os.path.join(ROOT, "work", "pairs")
# RRJB_PROBE_BUILD names the build directory that holds rrverify.exe (default below).
RRVERIFY = os.path.join(ROOT, os.environ.get("RRJB_PROBE_BUILD") or "build_m0", "rrverify.exe")
OVL_BASE = 0x8005B5E8
SHA1 = {"SLUS": "67ed165a2c517d4e6106fb0dfa324d66dd9a76f1",
        "G": "cfe43a7786759f2cb9c57751cf99e84d1074782c"}

MUTATE = {"on": False}

_ROWS = {}


def bench_rows():
    """{entry: row name} of every acceptance row of the bench, as `rrverify phys --list` prints its own row table
    (the rows' own entry field, whatever helper built them); spliced regions and the diagnostic
    `sound_emitter_is_invisible` are not ports of a whole function and are left out. An empty table raises."""
    if not _ROWS:
        r = subprocess.run([RRVERIFY, "phys", "--list"], capture_output=True, text=True, cwd=ROOT)
        for m in re.finditer(r"^\s+(\w+)\s+0x([0-9A-Fa-f]{8})\s\s(.*)$", r.stdout, re.M):
            name, entry, sig = m.group(1), int(m.group(2), 16), m.group(3)
            if "spliced" in sig or name == "sound_emitter_is_invisible":
                continue
            _ROWS.setdefault(entry, name)
        if not _ROWS:
            raise RuntimeError(f"{RRVERIFY} phys --list printed no rows (exit {r.returncode})")
    return dict(_ROWS)


def u32(v):
    return v & 0xFFFFFFFF


def s32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


def iabs(v):
    """(v + (v >> 31)) ^ (v >> 31): abs(INT_MIN) stays negative"""
    v = s32(v)
    sg = -1 if v < 0 else 0
    return s32((v + sg) ^ sg)


# ---------------------------------------------------------------------------
# images
# ---------------------------------------------------------------------------

_IM = {}


def images():
    if not _IM:
        _IM["SLUS"] = E.load_exe()
        _IM["G"] = E.load_overlay("RASHCDG.BIN", OVL_BASE)
    return _IM


def word_at(a):
    im = images()
    return im["G"].word(a) if im["G"].contains(a) else im["SLUS"].word(a)


def jal_target(site):
    w = word_at(site)
    if (w >> 26) != 3:
        return None
    return ((site + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)


_TG = {}


def call_sites(target):
    """every `jal target` in RASHCDG"""
    if not _TG:
        g = images()["G"]
        for a, w in g.words():
            if (w >> 26) == 3:
                _TG.setdefault(((a + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2), []).append(a)
    return _TG.get(target, [])


def starts():
    call_sites(0)
    return sorted(t for t in _TG if OVL_BASE <= t < 0x800CE000)


def extent(f):
    s = starts()
    return f, s[s.index(f) + 1]


def jals_in(lo, hi):
    return [a for a in range(lo, hi, 4) if (word_at(a) >> 26) == 3]


# ---------------------------------------------------------------------------
# the model-vs-interpreter harness
# ---------------------------------------------------------------------------

REG = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
       "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]


class Mismatch(Exception):
    pass


def put(mem, a, n, v):
    o = a & 0x1FFFFF
    if n == 4:
        struct.pack_into("<I", mem, o, v & 0xFFFFFFFF)
    elif n == 2:
        struct.pack_into("<H", mem, o, v & 0xFFFF)
    else:
        mem[o] = v & 0xFF


def _load(mem, op, ad):
    ad &= 0x1FFFFF
    return {0x23: lambda: struct.unpack_from("<I", mem, ad)[0],
            0x21: lambda: struct.unpack_from("<h", mem, ad)[0],
            0x25: lambda: struct.unpack_from("<H", mem, ad)[0],
            0x20: lambda: struct.unpack_from("<b", mem, ad)[0],
            0x24: lambda: mem[ad]}[op]() & 0xFFFFFFFF


def callee_args(site, args, post, mem):
    """calls.bin records a0..a3 when the jal executes: before its delay slot, and before a load issued by
    the instruction in front of it has landed (the R3000 load delay). Re-execute both when they write an
    argument register, from the callee-saved registers of the post-call probe and the recorded
    a-registers. An argument that depends on a caller-saved temporary comes back as None."""
    out = list(args)

    def rv(r):
        n = REG[r]
        if n == "zero":
            return 0
        if n in ("a0", "a1", "a2", "a3"):
            return out[r - 4]
        if n.startswith("s") or n in ("sp", "fp", "gp"):
            return post[n]
        raise KeyError(n)
    wp = word_at(site - 4)
    op, rs, rt = wp >> 26, (wp >> 21) & 31, (wp >> 16) & 31
    if op in (0x20, 0x21, 0x23, 0x24, 0x25) and 4 <= rt <= 7:
        imm = s32((wp & 0xFFFF) << 16) >> 16
        try:
            out[rt - 4] = _load(mem, op, rv(rs) + imm)
        except (KeyError, TypeError):
            out[rt - 4] = None
    w = word_at(site + 4)
    op, rs, rt, rd = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
    imm, simm, sh, fn = w & 0xFFFF, s32((w & 0xFFFF) << 16) >> 16, (w >> 6) & 31, w & 63
    dest, val = None, None
    try:
        if op == 0:
            dest = rd
            val = {0x21: lambda: rv(rs) + rv(rt), 0x20: lambda: rv(rs) + rv(rt),
                   0x23: lambda: rv(rs) - rv(rt), 0x22: lambda: rv(rs) - rv(rt),
                   0x25: lambda: rv(rs) | rv(rt), 0x24: lambda: rv(rs) & rv(rt),
                   0x00: lambda: rv(rt) << sh, 0x02: lambda: u32(rv(rt)) >> sh,
                   0x03: lambda: s32(rv(rt)) >> sh,
                   0x26: lambda: rv(rs) ^ rv(rt), 0x27: lambda: ~(rv(rs) | rv(rt)),
                   0x2A: lambda: int(s32(rv(rs)) < s32(rv(rt))),
                   0x2B: lambda: int(u32(rv(rs)) < u32(rv(rt)))}.get(fn, lambda: None)()
            if val is None:
                dest = None
        elif op in (0x08, 0x09):
            dest, val = rt, rv(rs) + simm
        elif op == 0x0D:
            dest, val = rt, rv(rs) | imm
        elif op == 0x0C:
            dest, val = rt, rv(rs) & imm
        elif op == 0x0E:
            dest, val = rt, rv(rs) ^ imm
        elif op == 0x0A:                               # slti
            dest, val = rt, int(s32(rv(rs)) < simm)
        elif op == 0x0B:                               # sltiu: the immediate is sign-extended, then compared unsigned
            dest, val = rt, int(u32(rv(rs)) < u32(simm))
        elif op == 0x0F:
            dest, val = rt, imm << 16
        elif op in (0x20, 0x21, 0x23, 0x24, 0x25):
            dest, val = rt, _load(mem, op, rv(rs) + simm)
    except (KeyError, TypeError):
        dest = rd if op == 0 else rt
        val = None
    if dest is None:
        # an instruction not modelled above that still writes a GPR: its target is unknown, not the pre-slot value
        if op == 0 and fn not in (0x08, 0x0C, 0x0D, 0x11, 0x13, 0x18, 0x19, 0x1A, 0x1B):
            dest = rd
        elif 0x08 <= op <= 0x0F or 0x20 <= op <= 0x26 or (op == 0x12 and rs in (0, 2)):
            dest = rt
    if dest is not None and 4 <= dest <= 7:
        out[dest - 4] = None if val is None else u32(val)
    return out


class Trace:
    def __init__(self, outdir, ram):
        self.ram0 = bytes(ram)
        self.calls = []
        b = open(os.path.join(outdir, "calls.bin"), "rb").read()
        for i in range(0, len(b), 40):
            seq, frm, tgt, a0, a1, a2, a3, sp = struct.unpack_from("<QIIIIIII", b, i)
            self.calls.append((seq, frm, tgt, (a0, a1, a2, a3), sp))
        self.cseq = [c[0] for c in self.calls]
        self.probes = defaultdict(list)
        p = os.path.join(outdir, "probes.csv")
        if os.path.exists(p):
            for r in csv.DictReader(open(p)):
                self.probes[int(r["pc"], 16)].append(
                    (int(r["seq"]), {k: int(v, 16) for k, v in r.items() if k not in ("seq", "name", "pc")}))
        self.writes = []
        for r in csv.DictReader(open(os.path.join(outdir, "watch.csv"))):
            if r["kind"] != "write":
                continue
            n = int(r["size"])
            self.writes.append((int(r["seq"]), int(r["pc"], 16), int(r["address"], 16), n,
                                int(r["value"], 16) & ((1 << (8 * n)) - 1)))
        self.writes.sort()
        self.wseq = [w[0] for w in self.writes]

    def post(self, pc, seq, sp):
        for pseq, regs in self.probes.get(pc, []):
            if pseq > seq and regs["sp"] == sp:
                return pseq, regs
        return None


class Machine:
    """What a model sees: the replayed reads, writes, calls and return value described in the module docstring."""

    def __init__(self, tr, mem, seq, ex, regs, lo, hi, frame):
        self.tr, self.mem = tr, mem
        self.entry_sp = regs["sp"]
        self.sp = u32(self.entry_sp - frame)
        self.regs = regs
        i0, i1 = bisect.bisect_right(tr.cseq, seq), bisect.bisect_left(tr.cseq, ex)
        self.gcalls = [c for c in tr.calls[i0:i1] if lo <= c[1] < hi and c[4] == self.sp]
        self.ci = 0
        self.mwrites = []
        self.windows = []
        self.notes = []
        self.unchecked = 0
        self.wi = bisect.bisect_right(tr.wseq, seq)

    def _o(self, a):
        return a & 0x1FFFFF

    def lw(self, a):
        if a & 3:
            raise Mismatch(f"unaligned lw {a:#x}")
        return struct.unpack_from("<I", self.mem, self._o(a))[0]

    def lws(self, a):
        return s32(self.lw(a))

    def lhs(self, a):
        return struct.unpack_from("<h", self.mem, self._o(a))[0]

    def lhu(self, a):
        return struct.unpack_from("<H", self.mem, self._o(a))[0]

    def lbu(self, a):
        return self.mem[self._o(a)]

    def lh(self, a):
        return u32(self.lhs(a))

    def lb(self, a):
        return u32(self.lbs(a))

    def lbs(self, a):
        return struct.unpack_from("<b", self.mem, self._o(a))[0]

    def _w(self, a, n, v):
        a = u32(a)
        v &= (1 << (8 * n)) - 1
        self.mwrites.append((a, n, v))
        put(self.mem, a, n, v)

    def sw(self, a, v):
        self._w(a, 4, v)

    def sh(self, a, v):
        self._w(a, 2, v)

    def sb(self, a, v):
        self._w(a, 1, v)

    def arg(self, n):
        return self.regs[("a0", "a1", "a2", "a3")[n]] if n < 4 else self.lw(self.entry_sp + 4 * n)

    def call(self, target, a0=None, a1=None, a2=None, a3=None, stack=()):
        if self.ci >= len(self.gcalls):
            raise Mismatch(f"model calls {target:#x}; the guest made only {len(self.gcalls)} calls")
        seq, frm, tgt, args, sp = self.gcalls[self.ci]
        self.ci += 1
        if tgt != target:
            raise Mismatch(f"call #{self.ci}: model {target:#x}, guest {tgt:#x} from {frm:#x}")
        post = self.tr.post(frm + 8, seq, sp)
        if post is None:
            raise Mismatch(f"no post-call probe at {frm + 8:#x}")
        got = callee_args(frm, args, post[1], self.mem)
        for k, (mv, gv) in enumerate(zip((a0, a1, a2, a3), got)):
            if mv is not None and gv is not None and u32(mv) != gv:
                raise Mismatch(f"call #{self.ci} {target:#x} from {frm:#x}: a{k} model {u32(mv):#x} guest {gv:#x}")
            if mv is not None and gv is None:
                self.unchecked += 1
        for k, v in enumerate(stack):
            if v is not None and self.lw(self.sp + 16 + 4 * k) != u32(v):
                raise Mismatch(f"call #{self.ci} {target:#x}: stack arg {k}")
        W = self.tr.writes
        while self.wi < len(W) and W[self.wi][0] < post[0]:
            sq, pc, a, n, v = W[self.wi]
            if sq > seq:
                put(self.mem, a, n, v)
            self.wi += 1
        self.windows.append((seq, post[0], frm + 4))
        return post[1]["v0"]


def run_model(tr, model, entry, frame, watched, cover=None):
    """every call of `entry` in the trace: (seq, ok, why)"""
    lo, hi = extent(entry)
    rets = [c + 8 for c in call_sites(entry)]
    if entry == 0x800A4774:
        rets.append(0x80011B28)          # the SLUS prime-pass caller
    out = []
    base = bytearray(tr.ram0)
    bi = 0
    W = tr.writes
    for seq, regs in tr.probes.get(entry, []):
        ex = None
        for rs in rets:
            p = tr.post(rs, seq, regs["sp"])
            if p and (ex is None or p[0] < ex[0]):
                ex = p
        if ex is None:
            out.append((seq, None, "truncated: the trace ends inside the call"))
            continue
        while bi < len(W) and W[bi][0] < seq:
            put(base, W[bi][2], W[bi][3], W[bi][4])
            bi += 1
        m = Machine(tr, bytearray(base), seq, ex[0], regs, lo, hi, frame)
        m.cover = cover if cover is not None else set()
        try:
            v0 = model(m, regs["a0"], regs["a1"], regs["a2"], regs["a3"])
            i0, i1 = bisect.bisect_right(tr.wseq, seq), bisect.bisect_left(tr.wseq, ex[0])
            g = [(a, n, v) for sq, pc, a, n, v in W[i0:i1]
                 if lo <= pc < hi and not any(s0 < sq < s1 and pc != d for s0, s1, d in m.windows)
                 and not (m.sp <= a < regs["sp"])]
            mw = [w for w in m.mwrites if not (m.sp <= w[0] < regs["sp"]) and watched(w[0], w[1])]
            if m.ci != len(m.gcalls):
                c = m.gcalls[m.ci]
                raise Mismatch(f"model made {m.ci} calls, guest {len(m.gcalls)} (next {c[2]:#x} from {c[1]:#x})")
            if mw != g:
                i = next((i for i, (x, y) in enumerate(zip(mw, g)) if x != y), min(len(mw), len(g)))
                raise Mismatch(f"write #{i}: model {[(hex(a), n, hex(v)) for a, n, v in mw[i:i + 2]]} "
                               f"guest {[(hex(a), n, hex(v)) for a, n, v in g[i:i + 2]]} ({len(mw)} vs {len(g)})")
            if v0 is not None and u32(v0) != ex[1]["v0"]:
                raise Mismatch(f"v0 model {u32(v0):#x} guest {ex[1]['v0']:#x}")
            out.append((seq, True, f"{len(g)} writes, {m.ci} calls"))
        except Mismatch as e:
            out.append((seq, False, str(e)))
    return out


# ---------------------------------------------------------------------------
# the models (every address RASHCDG)
# ---------------------------------------------------------------------------

GRID, NODES = 0x800CD0B0, 0x800CCFA8
ORGX, ORGZ = 0x800CCF98, 0x800CCFA0
POOLS, RADIUS, GS, CNT = 0x800CE4D0, 0x800CCA8C, 0x8005B2F8, 0x800CCF68
LIST, CUR, PREV, MASK = 0x800CCE48, 0x800CCF78, 0x800CCF70, 0x800CCF80


def cfg(key, orig, mutated):
    return mutated if MUTATE["on"] else orig


def cell_of(m, x, z, np_):
    """0x800A4A00..0x800A4AAC; the same code sits in front of every pair loop"""
    shift = cfg("cell", 21, 20)
    col = u32((s32(u32(x - m.lw(ORGX))) >> shift) + 11)
    row = u32((s32(u32(z - m.lw(ORGZ))) >> shift) + 11)
    if np_ != 1 and not (col < 24 and row < 24):
        col = u32((s32(u32(x - m.lw(ORGX + 4))) >> shift) + 11)          # player 2's origin
        r = (s32(u32(z - m.lw(ORGZ + 4))) >> shift) + 11
        row = u32(r + (24 if r >= 0 else 0))                            # sllv 24 by (r >> 31)
    if not col < 24 or not row < u32(24 << ((np_ - 1) & 31)):
        return None
    return u32(col + row * 24)


def cursor(m, cell, myh):
    """the node after mine in the cell's chain; the slack node 128 when my handle is absent"""
    if cell is None:
        return 128
    a0 = m.lbu(GRID + cell)
    while a0 != 128 and m.lbu(NODES + 2 * a0 + 1) != myh:
        a0 = m.lbu(NODES + 2 * a0)
    return m.lbu(NODES + 2 * a0)


def gather(m, e, np_):
    myh = m.lhu(e + 172)
    return [cursor(m, cell_of(m, m.lw(e + 196 + 12 * k), m.lw(e + 196 + 12 * k + 8), np_), myh)
            for k in range(4)]


def min_handle(m, c):
    h = [m.lbu(NODES + 2 * ci + 1) for ci in c]      # an exhausted cursor reads 0x800CD0A9
    a3 = h[0] + min(h[1] - h[0], 0)
    a2 = h[2] + min(h[3] - h[2], 0)
    return a3 + min(a2 - a3, 0)


def resolve(m, h):
    pool, slot = h >> 5, h & 31
    if pool == 6:
        v0 = u32(m.lw(0x800CD6C4) + slot * 280)
        return v0, v0 + 12
    rec = POOLS + ((h >> 1) & 0x7FF0)
    v0 = u32(m.lw(rec) + u32(m.lw(rec + 4) * slot))
    return v0, v0 + 184


def octagon(m, pt, e):
    a1 = iabs(m.lw(pt) - m.lw(e + 184))
    a0 = iabs(m.lw(pt + 8) - m.lw(e + 192))
    mn = a1 if a1 < a0 else a0
    return s32(s32(a1 + a0) - (s32(mn + (u32(mn) >> 31)) >> 1))


def advance(m, c, ph):
    for k in range(4):
        if m.lbu(NODES + 2 * c[k] + 1) == ph:
            c[k] = m.lbu(NODES + 2 * c[k])


def mid_wipeout(m, e):
    """0x800A5270: flagsC bits 5..8 (the four wipeout kinds) and (bit 21 or |+0x28C| > 0xB2B8)"""
    f = m.lw(e + 568)
    if not (f & 0x1E0):
        return 0
    return 1 if (f & 0x200000) or 0xB2B8 < iabs(m.lw(e + 652)) else 0


def model_pass(m, dt, a1, a2, a3):
    """0x800A4774 CollisionPass(dt), frame 112"""
    sp = m.sp
    np_ = lambda: m.lw(m.lw(GS) + 48)
    m.sw(sp + 32, 0)
    m.sw(0x800CCE38, dt)
    for i in range(48):
        for j in range(6):
            m.sw(GRID + 24 * i + 4 * j, 0x80808080)
    b = m.lw(0x8005B38C)
    m.sw(ORGX, m.lw(b + 184) + 0xF0000)
    m.sw(ORGZ, m.lw(b + 192) + 0xF0000)
    if not np_() < 2:
        b2 = m.lw(0x8005B21C)
        m.sw(ORGX + 4, m.lw(b2 + 184) + 0xF0000)
        m.sw(ORGZ + 4, m.lw(b2 + 192) + 0xF0000)
    done = False
    for kind in range(6, -1, -1):                                   # static volumes first, bikes last
        rec = POOLS + 16 * kind
        if m.lw(m.lw(rec + 8)) == 0:
            continue
        last, e, stride = s32(m.lw(m.lw(rec + 12))), m.lw(rec), m.lw(rec + 4)
        while last >= 0 and not done:
            pts, h = None, None
            if kind == 1 and m.lw(e + 604) < 2:
                pass
            elif kind == 6:
                h = m.lhu(e)
                if h != 0 and m.lhs(e + 148) != 0:
                    pts, npts = (e + 12, 1) if m.lw(e + 8) == 1 else (e + 24, 4)
            else:
                st, h = m.lhs(e + 320), m.lhu(e + 172)
                if st != 0 and kind != 0 and h != 0:
                    pts, npts = e + 196, 4
                elif st != 0 and kind == 0:
                    m.sw(e + 568, m.lw(e + 568) & 0xFDFFFFFF)
                    fl = 1 if (m.lw(e + 616) != 0 or m.lw(e + 676) != 0) else 0
                    m.sw(e + 552, 0)
                    m.sw(e + 568, m.lw(e + 568) | (fl << 24))
                    pts, npts = e + 196, 4
            if pts is not None:
                for k in range(npts):
                    cell = cell_of(m, m.lw(pts + 12 * k), m.lw(pts + 12 * k + 8), np_())
                    if cell is None:
                        continue
                    head = m.lbu(GRID + cell)
                    if head != 128 and m.lbu(NODES + 2 * head + 1) == (h & 0xFF):
                        continue
                    n = m.lw(sp + 32)
                    m.sb(NODES + 2 * n, head)
                    m.sb(NODES + 2 * n + 1, h)
                    m.sb(GRID + cell, m.lbu(sp + 32))
                    m.sw(sp + 32, n + 1)
                    if n + 1 == 128:
                        done = True
                        break
            e = u32(e + stride)
            last -= 1
        if done:
            break
    if m.lw(sp + 32) == 0:
        return None
    m.sw(CNT, 0)
    m.call(0x8001E100, LIST, 0, 288)
    t0, t1 = m.lw(CUR), m.lw(CUR + 4)
    m.sw(CUR, 1)
    m.sw(MASK, 0)
    m.sw(0x800CCF90, 0)
    m.sw(CUR + 4, 0)
    m.sw(PREV, t0)
    m.sw(PREV + 4, t1)
    m.call(0x8001E100, 0x800CCF88, 0, 8)
    # bike against bike, 0x800A4BCC..0x800A54A4
    if m.lw(m.lw(POOLS + 8)) != 0:
        last, e = s32(m.lw(m.lw(POOLS + 12))), m.lw(POOLS)
        while last >= 0:
            if m.lhs(e + 320) != 0:
                rad = s32(m.lw(RADIUS))
                m.sw(sp + 56, rad)
                c = gather(m, e, np_())
                while c != [128, 128, 128, 128]:
                    ph = min_handle(m, c)
                    part, pt = resolve(m, ph)
                    m.sw(sp + 48, part)
                    m.sw(sp + 52, ph)
                    if octagon(m, pt, e) < rad and (ph >> 5) == 0:
                        sM, sO = mid_wipeout(m, e), mid_wipeout(m, part)
                        if (not sO and sM) or m.lhu(m.lw(e + 852) + 544) == 0:
                            m.call(0x800B09C4, part, e + 172)
                        elif not sM and sO:
                            m.call(0x800B09C4, e, part + 172)
                        elif m.lhu(m.lw(part + 852) + 544) == 0:
                            m.call(0x800B09C4, e, part + 172)
                        else:
                            m.call(0x800AB7A0, e, part, 0)
                    advance(m, c, m.lw(sp + 52))
            e = u32(e + m.lw(POOLS + 4))
            last -= 1
    # the kind loop, 0x800A54A8..0x800A777C
    for kind in range(5):
        rec = POOLS + 16 * kind
        if m.lw(m.lw(rec + 8)) == 0:
            continue
        last, e = s32(m.lw(m.lw(rec + 12))), m.lw(rec)
        while last >= 0:
            run = m.lhs(e + 320) != 0
            if run and kind != 0 and m.lhu(e + 172) == 0:
                run = False
            if run and kind == 1 and m.lw(e + 604) < 2:
                run = False
            if run:
                off = 1 if (m.lw(e + 388) & 1) or (m.lw(e + 372) != 0 and m.lhs(e + 392) != 4) else 0
                rad = s32(u32(m.lw(RADIUS + 4 * kind) + (u32(-off) & 0xA0000)))
                m.sw(sp + 56, rad)
                if kind == 4 and m.lw(e + 480) == 0:
                    run = False
            if run:
                c = gather(m, e, np_())
                while c != [128, 128, 128, 128]:
                    ph = min_handle(m, c)
                    part, pt = resolve(m, ph)
                    m.sw(sp + 48, part)
                    m.sw(sp + 52, ph)
                    if octagon(m, pt, e) < rad:
                        dispatch(m, kind, e, part, ph >> 5)
                    advance(m, c, m.lw(sp + 52))
                tail(m, e, kind)
            e = u32(e + m.lw(rec + 4))
            last -= 1
        if kind == 0 and m.lw(CNT) != 0:
            m.call(0x800A77B0)
    return None


def dispatch(m, kind, e, part, pool):
    if kind == 0:                                                   # table 0x8005B910
        if pool in (1, 2):
            if u32(m.lhu(part + 544) - 60) >= 4:
                m.call(0x800AD04C, e, part)
        elif pool == 3:
            m.call(0x800AC5BC, e, part)
        elif pool == 4:
            if (m.lhu(m.lw(part) + 14) & 2) and not (m.lw(part + 592) & 0x1A06):
                m.call(0x800AE794, e, part + 172)
            else:
                m.call(0x800B09C4, e, part + 172)
        elif pool == 5:
            if s32(m.lw(part + 304)) > 0x10000 and s32(m.lw(part + 308)) > 0x10000:
                m.call(0x800B0D8C, e, part + 172)
            else:
                m.call(0x800B09C4, e, part + 172)
        elif pool == 6:
            if m.lw(part + 8) != 0:
                m.call(0x800AE794, e, part)
            elif s32(m.lw(part + 132)) > 0x10000 and s32(m.lw(part + 136)) > 0x10000:
                m.call(0x800B0D8C, e, part)
            else:
                m.call(0x800B09C4, e, part)
    elif kind in (1, 2):                                            # table 0x8005B930, indexed pool - 1
        if pool in (1, 2):
            m.call(0x800B2AF8, e, part)
        elif pool == 3:
            m.call(0x800B2844, e, part)
        elif pool in (4, 5):
            m.call(0x800B2B00, e, part + 172)
        elif pool == 6:
            m.call(0x800B2B00, e, part)
    elif kind == 3:
        if pool == 3:
            m.call(0x800B2D44, e, part)
        elif pool == 4:
            m.call(0x800B2D88, e, part)
    else:
        if pool == 5:
            m.call(0x800B2E64, e, part + 172)
        elif pool == 6:
            m.call(0x800B2E64, e, part)


def tail(m, e, kind):
    if kind == 0:
        crash = 1 if (m.lw(e + 568) & 0x600) else 0
        r = m.call(0x800B3AD0, e, crash)
        p = m.lw(e + 856)                              # a0 is left holding the passenger
        if p != 0 and ((r & 1) != ((r & 2) >> 1) or (r == 0 and (m.lw(e + 388) >> 20) & 0xF)):
            m.call(0x800B3AD0, p, 1 if (m.lw(e + 568) & 0x600) else 0)
        if (m.lw(e + 568) & 0x600) and not (m.lw(e + 564) & 0x800000):
            m.call(0x800B1978, e)
            return
        if not (m.lw(e + 564) & 0x400000) or (m.lw(e + 568) & 0xF):
            return
        if not m.call(0x80083F30, e, e + 172, e + 820, 4):
            return
        m.call(0x80017BA0, m.lw(e + 184), m.lw(e + 192), 17 if (m.lw(e + 564) & 0xE) else 3, 0)
        if m.lhu(e + 172) < m.lw(m.lw(GS) + 48) and m.lw(m.lw(e + 852) + 604) < 2 and m.lw(0x8005B220) == 0:
            m.sw(m.sp + 16, 1)
            m.call(0x800B658C, e, 0, m.lw(e + 480), 0x165A1C, stack=(1,))
    elif kind in (1, 2):
        m.call(0x800B3AD0, e, (m.lw(e + 552) >> 30) & 1)
        if (m.lw(e + 552) & 0xC0000000) == 0x40000000 and s32(m.call(0x8002E698, e + 522, e + 450)) > 0:
            m.call(0x800B208C, e)
    elif kind == 4:
        m.call(0x800B3AD0, e, (m.lw(e + 592) >> 1) & 1)
        if (m.lw(e + 592) & 3) == 2 and s32(m.call(0x8002E698, e + 522, e + 450)) > 0:
            for k in range(3):
                m.sh(m.sp + 24 + 2 * k, u32(-m.lhu(e + 522 + 2 * k)))
            m.call(0x800B3344, e, e + 504, m.sp + 24, 1)


def model_impulse(m, e, v, flag, a3):
    """0x800A8DF0 ApplyImpulse(e, v, flag), a leaf"""
    for k in range(3):
        m.sw(e + 184 + 4 * k, m.lw(e + 184 + 4 * k) + m.lw(v + 4 * k))
    pool = m.lhu(e + 172) >> 5
    both = pool == 0 and m.lw(e + 856) != 0 and m.lw(e + 1088) != 0
    if both:
        p = m.lw(e + 856)
        for k in range(3):
            m.sw(p + 184 + 4 * k, m.lw(p + 184 + 4 * k) + m.lw(v + 4 * k))
    for c in range(8):
        for k in range(3):
            m.sw(e + 196 + 12 * c + 4 * k, m.lw(e + 196 + 12 * c + 4 * k) + m.lw(v + 4 * k))
        if both:
            p = m.lw(e + 856)
            for k in range(3):
                m.sw(p + 196 + 12 * c + 4 * k, m.lw(p + 196 + 12 * c + 4 * k) + m.lw(v + 4 * k))
    if flag:
        if pool == 0:
            m.sw(e + 568, m.lw(e + 568) | cfg("stale", 0x01800000, 0x01000000))
        elif pool == 1:
            m.sw(e + 552, m.lw(e + 552) | 2)
        elif pool == 2:
            m.sw(e + 568, m.lw(e + 568) | 2)
        elif pool == 4:
            m.sw(e + 592, m.lw(e + 592) | 0x100)
    return None


def model_stale(m, e, a1, a2, a3):
    """0x800A8FE8 StaleHeading(e), frame 48: speed and heading re-derived from the box displacement"""
    f = m.lw(e + 568)
    if not (f & 0x01000000):
        return 0
    m.sw(e + 568, f & 0xFEFFFFFF)
    sp = m.sp
    for k in range(3):
        m.sw(sp + 16 + 4 * k, m.lw(e + 184 + 4 * k) - m.lw(e + 468 + 4 * k))
    q = s32(m.call(0x8002F0F4, sp + 16))
    if q < 132:
        return 0
    if not (m.lw(e + 568) & 0x02000000):
        h = [m.lhu(e + 450 + 2 * k) for k in range(3)]
        v, f = m.lw(e + 480), m.lw(e + 568) | 0x02000000
        for k in range(3):
            m.sh(e + 864 + 2 * k, h[k])
        m.sw(e + 860, v)
        m.sw(e + 568, f)
    s0 = s32(u32(m.call(0x8004CF74, q) << 2))
    dt = m.lws(0x800CCE38)
    if s0 > 0:
        v = m.call(0x80010028, s0, dt) if dt > 0 else u32(-s32(m.call(0x80010028, s0, u32(-dt))))
    else:
        v = u32(-s32(m.call(0x80010028, u32(-s0), dt))) if dt > 0 else m.call(0x80010028, u32(-s0), u32(-dt))
    m.sw(e + 480, cfg("speed", v, u32(-s32(v))))
    n = s0 if s0 >= 0 else -s0
    d = u32((n >> 1) + ((n - 2) >> 31))
    r = 0x80000000 // d
    m.call(0x8002EED8, r if s0 >= 0 else u32(-r), sp + 16, e + 450)
    p = m.lw(e + 856)
    if p == 0:
        return 1
    if m.lw(e + 1088) != 0:
        m.sw(e + 568, m.lw(e + 568) | 0x01000000)
        m.call(0x800A8FE8, p)
    return 1


def model_camera_box(m, e, halfw, depth, a3):
    """0x800A8C78 InCameraBox(e, halfWidth, depth), frame 56"""
    m.sw(m.entry_sp, e)
    m.sw(m.entry_sp + 4, halfw)
    m.sw(m.entry_sp + 8, depth)
    st = m.lhu(e + 320)
    if st & 8:
        return (st >> 2) & 1
    m.sw(e + 48, 0x7FFFFFFF)
    res, p = 0, 0
    while True:
        o = 1132 * p
        v = s32(m.call(0x800B6AAC, e + 184, 0x800CDA54 + o, 0x800CD950 + o))
        if v > 0x8000 and v < s32(depth):
            v = s32(m.call(0x800B6AAC, e + 184, 0x800CDA48 + o, 0x800CD950 + o))
            if iabs(v) < s32(halfw):
                res = 1
        m.sw(e + 44 + 4 * p, v >> cfg("dist", 10, 9))
        p += 1
        if not p < m.lw(m.lw(GS) + 48):
            break
    m.sh(e + 320, m.lhu(e + 320) | 8 | (res << 2))
    return res


def model_classify(m, a, b, p_side, p_ang):
    """0x800ABE78 BikePairClassify(a, b, &side, &angle, [sp+16] &along, [sp+20] &across), frame 48"""
    p_along, p_across = m.arg(4), m.arg(5)
    code = (1 if iabs(m.lw(a + 188) - m.lw(b + 188)) < m.lws(a + 312) else 0) << 2
    if code:
        d = (m.lw(a + 292) - m.lw(b + 292) + 4096) & 0xFFF
        fold = d + ((4096 - 2 * d) & (-1 if 2048 - d < 0 else 0))
        if fold < 384:
            code += 8
        m.sw(p_ang, cfg("angle", d, (4096 - d) & 0xFFF))
    if code == 12:
        x = s32(m.call(0x800B6AAC, b + 504, a + 516, a + 504))
        m.sw(p_across, x)
        f1 = (1 if 0 < x else 0) if m.lw(a + 856) else 0
        f2 = (u32(x) >> 31) if m.lw(b + 856) else 0
        side = f1 | (f2 << 1)
        m.sw(p_side, side)
        w0 = m.lws(a + 304)
        reach = s32((m.lws(a + 308) << 1) + (w0 << 2))
        half = (w0 + (1 if w0 < 0 else 0)) >> 1
        ax = iabs(m.lw(p_across))
        if side & 1:
            q = m.lws(m.lw(a + 856) + 304)
            reach, half = s32(reach + (q << 1)), s32(half + q + w0)
        elif side & 2:
            q = m.lws(m.lw(b + 856) + 304)
            reach, half = s32(reach + (q << 1)), s32(half + m.lws(b + 304) + q)
        if not ax < reach:
            return u32(-1)
        if half < ax:
            code += 1
    if code == 13:
        y = s32(m.call(0x800B6AAC, b + 504, a + 528, a + 504))
        m.sw(p_along, y)
        side = m.lw(p_side)
        q = m.lw(a + 856) if side & 1 else (m.lw(b + 856) if side & 2 else None)
        if q is not None:
            v = s32(7 * s32(m.lws(q + 308) + m.lws(b + 308)))
            thr = (v + 7) >> 3 if v < 0 else v >> 3
        else:
            v = s32(9 * s32(m.lws(a + 308) + m.lws(b + 308)))
            thr = (v + 15) >> 4 if v < 0 else v >> 4
        if iabs(m.lw(p_along)) < thr:
            code += 2
    if code == 15 and not (0xEFFFF < iabs(m.lw(a + 480) - m.lw(b + 480))):
        code = 31
    return code


def model_bike_pair(m, me, other, mode, a3):
    """0x800AB7A0 BikeVsBike(me, other, mode), frame 152"""
    sp = m.sp
    m.sw(sp + 80, 0)
    m.sw(sp + 108, 0)
    stale = m.call(0x800A8FE8, me) | u32(m.call(0x800A8FE8, other) << 1)
    arm = 0
    if m.call(0x800A8C78, me, 0x140000, 0x1C0000):
        m.sh(other + 320, m.lhu(other + 320) | 0xC)
        oh = m.lhu(other + 172)
        if oh < m.lw(m.lw(GS) + 48):
            t0 = (1 if oh == 1 else 0) & ((m.lw(CUR) >> 2) & 1)
            mh = m.lhu(me + 172)
            wa = CUR + 4 * (mh >> 4)
            m.sw(wa, m.lw(wa) | u32(u32(oh - (t0 - 1)) << ((mh & 0xF) * 2)))
            t1 = (1 if t0 == 0 else 0) if other == m.lw(0x8005B21C) else 0
            m.sw(CUR, m.lw(CUR) | (t1 << 3))
        for ent in (me, other):
            h = m.lhu(ent + 172)
            hit = (m.lw(PREV + 4 * (h >> 4)) >> ((h & 0xF) * 2)) & 3
            m.sw(MASK, m.lw(MASK) | (u32(1 << (h & 31)) if hit else 0))
        if mode == 0:
            n = m.lws(CNT)
            mh, oh2 = m.lhu(me + 172), m.lhu(other + 172)
            for i in range(max(n, 0)):
                ha, hb = m.lhu(LIST + 36 * i), m.lhu(LIST + 36 * i + 2)
                if (ha == mh and hb == oh2) or (ha == oh2 and hb == mh):
                    return 0
        m.sw(sp + 16, sp + 88)
        m.sw(sp + 20, sp + 92)
        code = m.call(0x800ABE78, me, other, sp + 80, sp + 84, stack=(sp + 88, sp + 92))
        if u32(code) == 0xFFFFFFFF:
            return 0
        arm = 2 if code == 0x1F else 1
    else:
        for bit, ent in ((1, me), (2, other)):
            if stale & bit:
                a = m.call(0x80020018, u32(m.lhs(ent + 450) << 4), u32(m.lhs(ent + 454) << 4))
                m.sw(ent + 292, a)
                m.sw(ent + 296, u32(m.lhs(0x8005624C + 4 * (a & 0xFFF) + 2) << 4))
                m.sw(ent + 300, u32(m.lhs(0x8005624C + 4 * (m.lw(ent + 292) & 0xFFF)) << 4))
    m.sw(sp + 104, 0)
    if arm == 2:
        for k, v in ((16, m.lw(sp + 88)), (20, m.lw(sp + 92)), (24, sp + 96), (28, sp + 100), (32, sp + 40)):
            m.sw(sp + k, v)
        s1 = m.call(0x800AA474, me, other, m.lw(sp + 80), m.lw(sp + 84), stack=(None, None, sp + 96, sp + 100, sp + 40))
    elif arm == 1:
        m.sw(sp + 16, sp + 40)
        m.sw(sp + 20, sp + 104)
        s1 = m.call(0x800AAD30, other, me, sp + 100, sp + 96, stack=(sp + 40, sp + 104))
        if m.lw(me + 856) != 0 and s1 == 0:
            s1 = m.call(0x800AAD30, me, other, sp + 96, sp + 100, stack=(sp + 40, sp + 104))
    else:
        m.sw(sp + 16, sp + 40)
        s1 = m.call(0x800AA140, other, me, sp + 100, sp + 96, stack=(sp + 40,))
    if m.lw(sp + 96) == 0 and m.lw(sp + 100) == 0:
        return m.lw(sp + 108)
    if m.lw(s1 + 1088) == 0:
        s1 = m.lw(s1 + 856)
    if s1 == me:
        s2, s3, s4 = other, m.lw(sp + 100), m.lw(sp + 96)
    else:
        s2, s4, s3 = me, m.lw(sp + 100), m.lw(sp + 96)
    if mode == 0:
        m.call(0x800A8DF0, s1, sp + 40, 1)
    if arm <= 0:
        m.call(0x800AC130, s1, s2, s4, s3)
        return m.lw(sp + 108)
    m.sw(sp + 108, 1)
    if s32(m.lw(sp + 104)) > 0:
        m.call(0x800B5B48, s2, s1, sp + 72)
        k = m.call(0x8001FC90, m.lw(s2 + 576), m.lw(sp + 104))
        m.call(0x8002EE50, k, sp + 72, sp + 56)
        m.call(0x800A8DF0, me, sp + 56, 1)
        m.call(0x800A8DF0, other, sp + 56, 1)
        v = m.lw(sp + 104)
        for ent in (me, other):
            w = m.lw(ent + 552)
            m.sw(ent + 552, cfg("max", (w if v < w else v), (v if v < w else w)))
    n, i = m.lws(CNT), 0
    h1, h2 = m.lhu(s1 + 172), m.lhu(s2 + 172)
    while i < n:
        ha, hb = m.lhu(LIST + 36 * i), m.lhu(LIST + 36 * i + 2)
        if (ha == h1 and hb == h2) or (ha == h2 and hb == h1):
            break
        i += 1
    if not i < 8:
        m.call(0x800AC130, s1, s2, s4, s3)
        return 1
    if mode != 0 and i < m.lws(CNT):
        return 1
    r = LIST + 36 * i
    m.sh(r, h1)
    m.sw(r + 16, m.lw(sp + 40))
    m.sw(r + 20, m.lw(sp + 44))
    m.sw(CNT, m.lw(CNT) + 1)
    m.sw(r + 24, m.lw(sp + 48))
    m.sw(r + 28, cfg("pack", s4 | u32(s3 << 16), s3 | u32(s4 << 16)))
    m.sh(r + 2, m.lhu(s2 + 172))
    if m.lw(MASK) & u32(1 << (m.lhu(s1 + 172) & 31)):
        m.call(0x800A7AB4, s1, sp + 40, m.lhu(s2 + 172))
    return m.lw(sp + 108)


def _wall_claim(model):
    """WallContact returns 0x100 | corner; --mutate claims it returns the bare corner"""
    def run(m, a0, a1, a2, a3):
        r = model(m, a0, a1, a2, a3)
        return (r & 0xFF) if (MUTATE["on"] and r is not None and r & 0x100) else r
    return run


# (entry, frame, model, name)
MODELS = [
    (0x800A4774, 112, model_pass, "CollisionPass"),
    (0x800AB7A0, 152, model_bike_pair, "BikeVsBike"),
    (0x800ABE78, 48, model_classify, "BikePairClassify"),
    (0x800A8DF0, 0, model_impulse, "ApplyImpulse"),
    (0x800A8FE8, 48, model_stale, "StaleHeading"),
    (0x800A8C78, 56, model_camera_box, "InCameraBox"),
]
import pairs_models as PM  # noqa: E402
MODELS += [(f, fr, _wall_claim(mod) if f == 0x800B3AD0 else mod, nm) for f, fr, mod, nm in PM.MODELS]


# ---------------------------------------------------------------------------
# the live runs: snapshot copies that differ from the savestate by DATA only
# ---------------------------------------------------------------------------

PLAYER = 0x801B65D4                  # pool 0 slot 0, the player's bike (quick and rr-race)
BIKE14, BIKE15 = 0x801BA1C4, 0x801BA60C
CAR = 0x800CF660                     # pool 3 slot 0, the traffic car of rr-race
BOX_FIELDS = [0x1D4, 0x0B8, 0x1F8, 0x310] + [0x0C4 + 12 * k for k in range(8)]
WATCHES = [(0x801B3000, 0xB400, "ents"), (0x800CC000, 0xA000, "glob"), (0x801FE000, 0x2000, "stack"),
           (0x8005B000, 0x1000, "sglob")]


def watched(a, n):
    return any(lo <= a and a + n <= lo + ln for lo, ln, _ in WATCHES)


def rd(ram, a, fmt="<i"):
    return struct.unpack_from(fmt, ram, a & 0x1FFFFF)[0]


def move_box(ram, e, to_x, to_z):
    """DATA edit: translate entity e so that its box centre +0xB8/+0xC0 lands on (to_x, to_z), by adding
    the same (dx, dz) to every field that holds its position (BOX_FIELDS)."""
    dx, dz = to_x - rd(ram, e + 0xB8), to_z - rd(ram, e + 0xC0)
    return [(e + f, dx, 0) for f in BOX_FIELDS] + [(e + f + 8, dz, 0) for f in BOX_FIELDS]


def car_on_player(ram, along, lateral):
    """DATA edit: put the traffic car on the player's road slice. Traffic is placed from its road state
    every frame (its +0xB8 is written by MulAdd from the slice), so moving its box does not stick; its
    slice pointer +0x154, the slice-derived pointer +0x1EC, the distance inside the slice +0x15C and the
    lateral offset +0x158 are set from the player's, offset by (along, lateral)."""
    return [(CAR + 0x154, None, rd(ram, PLAYER + 0x154, "<I")), (CAR + 0x1EC, None, rd(ram, PLAYER + 0x1EC, "<I")),
            (CAR + 0x15C, None, u32(rd(ram, PLAYER + 0x15C) + along)),
            (CAR + 0x158, None, u32(rd(ram, PLAYER + 0x158) + lateral))]


BIKE6, RIDER6, VOL0 = 0x801B7F84, 0x801BC1A4, 0x801B3AC4     # quick: the crashed bike 6, its downed rider, pool-6 slot 0
FIRST_STEP = (70779, -123208)       # the player's box travel in quick before its first pass: (1.08, -1.88) in 16.16


def box_volume(ram, rec, hx, hz, hy):
    """DATA edit: rewrite pool-6 record `rec` (a class-1 pole in quick) as a class-0 box laid out the way the
    spawner 0x8009BFE4 lays one out for an identity basis (population.md 4.1): cls 0, half extents
    +0x84/+0x88/+0x8C, corners 0..3 = centre +- (hx, hz), corners 4..7 = those lowered by 0.5 + hy."""
    fx = lambda v: int(round(v * 65536))
    c = [rd(ram, rec + 12 + 4 * k) for k in range(3)]
    assert [rd(ram, rec + 0x104 + 2 * k, "<h") for k in range(9)] == [4096, 0, 0, 0, 4096, 0, 0, 0, 4096]
    sets = [(rec + 8, None, 0), (rec + 0x84, None, fx(hx)), (rec + 0x88, None, fx(hz)), (rec + 0x8C, None, fx(hy))]
    for k, (sx, sz) in enumerate([(-1, -1), (1, -1), (1, 1), (-1, 1)]):
        top = [c[0] + sx * fx(hx), c[1], c[2] + sz * fx(hz)]
        low = [top[0], top[1] - 0x8000 - fx(hy), top[2]]
        sets += [(rec + 0x18 + 12 * k + 4 * i, None, u32(top[i])) for i in range(3)]
        sets += [(rec + 0x18 + 12 * (k + 4) + 4 * i, None, u32(low[i])) for i in range(3)]
    return sets


def edits_for(name, ram):
    fx = lambda v: int(round(v * 65536))
    b15x, b15z = rd(ram, BIKE15 + 0xB8), rd(ram, BIKE15 + 0xC0)
    vx, vz = rd(ram, VOL0 + 0x0C), rd(ram, VOL0 + 0x14)
    bike = lambda k: PLAYER + 1096 * k
    # three units beside the player's first-pass spot, across his heading (0.87, 0.5)
    px, pz = rd(ram, PLAYER + 0xB8) + FIRST_STEP[0] + fx(3 * 0.87), rd(ram, PLAYER + 0xC0) + FIRST_STEP[1] + fx(3 * 0.5)
    d6 = (px - rd(ram, BIKE6 + 0xB8), pz - rd(ram, BIKE6 + 0xC0))
    return {
        # the player's bike 0.6 units to the side of AI bike 15, level with it
        "bb-side": lambda: move_box(ram, PLAYER, b15x + fx(0.6), b15z),
        # the player's bike 0.35 units to the side of bike 15: close enough for a mid-step contact
        "bb-close": lambda: move_box(ram, PLAYER, b15x + fx(0.35), b15z),
        # the player's bike 1.6 units behind AI bike 15 (both head -Z)
        "bb-rear": lambda: move_box(ram, PLAYER, b15x, b15z + fx(1.6)),
        # AI bike 14 half a unit off AI bike 15 in X and Z: no player involved
        "bb-ai": lambda: move_box(ram, BIKE14, b15x + fx(0.5), b15z - fx(0.5)),
        # the traffic car on the player's own spot / 1.6 to his side / 3.5 ahead of him
        "tr-over0": lambda: car_on_player(ram, 0, 0),
        "tr-over-side": lambda: car_on_player(ram, 0, fx(1.6)),
        "tr-ahead": lambda: car_on_player(ram, fx(-3.5), 0),
        # the player 0.3 beside the class-1 pole of pool-6 slot 0 at his first pass
        "pole": lambda: move_box(ram, PLAYER, vx + fx(0.3) - FIRST_STEP[0], vz - FIRST_STEP[1]),
        # that record rewritten as a 2 x 2 x 3 class-0 box, the player 1.2 off its centre
        "box": lambda: move_box(ram, PLAYER, vx + fx(1.2) - FIRST_STEP[0], vz - FIRST_STEP[1])
        + box_volume(ram, VOL0, 2.0, 2.0, 3.0),
        # the crashed bike 6 (mid-wipeout) and its downed rider moved together 3 units beside the player
        "own6": lambda: [(e + f + o, d, 0) for e in (BIKE6, RIDER6) for f in BOX_FIELDS for o, d in ((0, d6[0]), (8, d6[1]))],
        # AI bike 13 onto AI bike 12, 0.4 to its side, far from the camera
        "far": lambda: move_box(ram, bike(13), rd(ram, bike(12) + 0xB8) + fx(0.4), rd(ram, bike(12) + 0xC0)),
    }[name]()


RUNS = [  # name, source savestate, frames
    ("bb-side", "quick", 20), ("bb-close", "quick", 20), ("bb-rear", "quick", 20), ("bb-ai", "quick", 20),
    ("tr-over0", "rr-race", 20), ("tr-over-side", "rr-race", 20), ("tr-ahead", "rr-race", 20),
    ("pole", "quick", 20), ("box", "quick", 20), ("own6", "quick", 20), ("far", "quick", 20),
    ("quick", "quick", 20), ("rr-race", "rr-race", 20),
]


def prepare(name, src):
    dst = os.path.join(WORK, "probe", "state-" + name)
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    shutil.copytree(os.path.join(STATE_DIR, src), dst)
    p = os.path.join(dst, "ram.bin")
    ram = bytearray(open(p, "rb").read())
    edits = [] if name == src else edits_for(name, bytes(ram))
    for a, delta, value in edits:
        o = a & 0x1FFFFF
        v = u32(struct.unpack_from("<I", ram, o)[0] + delta) if delta is not None else value
        struct.pack_into("<I", ram, o, v)
    open(p, "wb").write(ram)
    return dst, bytes(ram), edits


def probes_for(funcs):
    ps = set()
    for f in funcs:
        lo, hi = extent(f)
        ps.add(f)
        ps.update(a + 8 for a in jals_in(lo, hi))
        ps.update(c + 8 for c in call_sites(f))
    ps.update((0x8008AC58, 0x80011B28))                    # after the pass, both callers
    return sorted(ps)


def run_trace(name, src, frames, funcs):
    state, ram, edits = prepare(name, src)
    out = os.path.join(WORK, "probe", "tr-" + name)
    cmd = [RRVERIFY, "trace", "--state", state, "--frames", str(frames), "--no-cop2", "--no-gpu",
           "--pad", "0xFFFF", "--calls", "--out", out]
    for a, n, w in WATCHES:
        cmd += ["--watch", f"{a:#x}:{n}:{w}"]
    for p in probes_for(funcs):
        cmd += ["--probe", f"{p:#x}:p{p:x}"]
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
    stop = [ln.split(None, 1)[1].strip() for ln in r.stdout.splitlines() if ln.startswith(("stopped", "buffer swaps"))]
    return Trace(out, ram), stop, edits


def snapshots(tr, seqs):
    """RAM (snapshot + watched writes) just before each seq, for a sorted list of seqs"""
    out, mem, i, W = {}, bytearray(tr.ram0), 0, tr.writes
    for s in sorted(seqs):
        while i < len(W) and W[i][0] < s:
            put(mem, W[i][2], W[i][3], W[i][4])
            i += 1
        out[s] = bytes(mem)
    return out


def passes(tr):
    """[(entry seq, exit seq)] of every collision pass in the trace"""
    ent = [s for s, _ in tr.probes.get(0x800A4774, [])]
    ex = [s for s, _ in tr.probes.get(0x8008AC58, [])]
    return [(a, min((b for b in ex if b > a), default=None)) for a in ent]


def contacts(ram):
    n = rd(ram, CNT)
    return [(rd(ram, LIST + 36 * i, "<H"), rd(ram, LIST + 36 * i + 2, "<H"),
             tuple(rd(ram, LIST + 36 * i + 16 + 4 * k) for k in range(3)), rd(ram, LIST + 36 * i + 28, "<I"))
            for i in range(max(0, min(n, 8)))]


# ---------------------------------------------------------------------------
# checks
# ---------------------------------------------------------------------------

class Bench:
    def __init__(self):
        self.n = 0
        self.fail = 0

    def check(self, ok, what):
        self.n += 1
        if not ok:
            self.fail += 1
            print("  FAIL", what)
        return ok


# (site, callee): every call the transcriptions name
SKELETON = [
    (0x800A4B74, 0x8001E100), (0x800A4BC4, 0x8001E100), (0x800A5394, 0x800AB7A0), (0x800A533C, 0x800B09C4),
    (0x800A5384, 0x800B09C4), (0x800A5C48, 0x800AD04C), (0x800A5C5C, 0x800AC5BC), (0x800A5D5C, 0x800AE794),
    (0x800A5CDC, 0x800B0D8C), (0x800A5CEC, 0x800B09C4), (0x800A5D3C, 0x800B0D8C), (0x800A5D4C, 0x800B09C4),
    (0x800A5E68, 0x800B3AD0), (0x800A5EC0, 0x800B3AD0), (0x800A5EF0, 0x800B1978), (0x800A5F30, 0x80083F30),
    (0x800A5F60, 0x80017BA0), (0x800A5FC0, 0x800B658C), (0x800A6634, 0x800B2AF8), (0x800A6648, 0x800B2844),
    (0x800A6670, 0x800B2B00), (0x800A677C, 0x800B3AD0), (0x800A679C, 0x8002E698), (0x800A67AC, 0x800B208C),
    (0x800A6E3C, 0x800B2D44), (0x800A6E50, 0x800B2D88), (0x800A75C0, 0x800B2E64), (0x800A76CC, 0x800B3AD0),
    (0x800A76E8, 0x8002E698), (0x800A7724, 0x800B3344), (0x800A7760, 0x800A77B0),
    (0x800AB7DC, 0x800A8FE8), (0x800AB7E8, 0x800A8FE8), (0x800AB800, 0x800A8C78), (0x800AB9F8, 0x800ABE78),
    (0x800ABA34, 0x80020018), (0x800ABA9C, 0x80020018), (0x800ABB30, 0x800AA474), (0x800ABB70, 0x800AAD30),
    (0x800ABBA0, 0x800AAD30), (0x800ABBC0, 0x800AA140), (0x800ABC30, 0x800A8DF0), (0x800ABC5C, 0x800B5B48),
    (0x800ABC6C, 0x8001FC90), (0x800ABC80, 0x8002EE50), (0x800ABC90, 0x800A8DF0), (0x800ABCA0, 0x800A8DF0),
    (0x800ABD70, 0x800AC130), (0x800ABE20, 0x800A7AB4), (0x800ABE3C, 0x800AC130),
    (0x800ABF34, 0x800B6AAC), (0x800AC020, 0x800B6AAC), (0x800A8D10, 0x800B6AAC), (0x800A8D44, 0x800B6AAC),
    (0x800A9050, 0x8002F0F4), (0x800A90A8, 0x8004CF74), (0x800A9150, 0x8002EED8), (0x800A9184, 0x800A8FE8),
    (0x800A7B98, 0x800AB7A0),
]
# the three partner tables, read out of the overlay
TABLES = {
    0x8005B8F8: [0x800A55E0, 0x800A5FD0, 0x800A5FE4, 0x800A67BC, 0x800A6F58],          # by the mover's kind
    0x8005B910: [0x800A5D64, 0x800A5C28, 0x800A5C28, 0x800A5C58, 0x800A5C6C, 0x800A5CAC, 0x800A5CFC],
    0x8005B930: [0x800A6630, 0x800A6630, 0x800A6644, 0x800A6658, 0x800A6658, 0x800A6668],  # pool - 1
}
WORDS = [
    (0x800A5250, 0x0058102A, "slt v0,v0,t8 - octagon < radius, signed"),
    (0x800A55A0, 0x3C03000A, "lui v1,0xa - the 10.0 off-tarmac radius bonus"),
    (0x800A52A8, 0x3403B2B8, "li v1,0xb2b8 - the |+0x28C| threshold of the mid-wipeout test"),
    (0x800AB7FC, 0x3C050014, "lui a1,0x14 - the camera box half width 20.0"),
    (0x800AB804, 0x3C06001C, "lui a2,0x1c - the camera box depth 28.0"),
    (0x800AB818, 0x3442000C, "ori v0,v0,0xc - the partner is forced into the camera box"),
    (0x800ABA00, 0x3843001F, "xori v1,v0,0x1f - classifier code 31 selects 0x800AA474"),
    (0x800ABCB4, 0x0065102B, "sltu v0,v1,a1 - +0x228 = MAX (unsigned) with the magnitude"),
    (0x800ABF0C, 0x28630180, "slti v1,v1,384 - headings within 384/4096 are parallel"),
    (0x800AC0D8, 0x3C04000E, "lui a0,0xe - 0x000EFFFF, the speed-match bound of code 31"),
    (0x800A8D1C, 0x34028000, "li v0,0x8000 - forward > 0.5 in front of the camera"),
    (0x800A905C, 0x2A020084, "slti v0,s0,132 - the displacement^2 below which the heading stays"),
    (0x800A8F6C, 0x3C030180, "lui v1,0x180 - a pushed bike's flagsC gains 0x01800000"),
    (0x800A4A14, 0x00021543, "sra v0,v0,0x15 - 32-unit cells"),
    (0x800A4A68, 0x24630000 | 11, "addiu v1,v1,11 - player 2's rows ..."),
    (0x800A4A70, 0x004D1004, "sllv v0,t5,v0 - ... += 24 << (row >> 31)"),
    (0x800A4A4C, 0x8F220004, "lw v0,4(t9) - the fallback origin is PLAYER 2's (0x800CCF9C)"),
    (0x800A4BC8, 0xAC490004, "sw t1,4(v0) - 0x800CCF74 = the old 0x800CCF7C"),
    (0x800A4BC0, 0x2442CF70, "addiu v0,v0,-12432 - 0x800CCF70"),
    (0x800A4B88, 0x24060008, "li a2,8 - 0x800CCF88 is cleared for 8 bytes"),
]


CENSUS = [0x800A4774, 0x800A77B0, 0x800A7AB4, 0x800A8BE0, 0x800A8C78, 0x800A8DF0, 0x800A8FE8, 0x800A91AC,
          0x800A9408, 0x800AA140, 0x800AA34C, 0x800AA474, 0x800AAD30, 0x800AB7A0, 0x800ABE78, 0x800AC130,
          0x800AC56C, 0x800AC5BC, 0x800AC958, 0x800AD04C, 0x800AE794, 0x800B09C4, 0x800B0D8C, 0x800B2C98,
          0x800B3AD0, 0x800B59F0, 0x800B5B48, 0x800B675C, 0x800B6BD0, 0x800B6F40, 0x800B7030, 0x800B71AC,
          0x800B74F0, 0x800A451C]


def check_skeleton(bench, verbose=True):
    im = images()
    bench.check(im["SLUS"].sha1 == SHA1["SLUS"], "SLUS sha1")
    bench.check(im["G"].sha1 == SHA1["G"], "RASHCDG sha1")
    for site, callee in SKELETON:
        want = callee
        if MUTATE["on"] and site == 0x800ABB30:
            want = 0x800AAD30                                   # mutated: "code 31 takes the shared response"
        bench.check(jal_target(site) == want, f"jal at {site:#x} -> {want:#x}")
    for base, want in TABLES.items():
        got = [word_at(base + 4 * i) for i in range(len(want))]
        if MUTATE["on"] and base == 0x8005B930:
            want = [0x800A6630, 0x800A6644] + want[2:]          # mutated: a wrong first two entries
        bench.check(got == want, f"jump table {base:#x}: {[hex(x) for x in got]}")
    for a, w, what in WORDS:
        bench.check(word_at(a) == w, f"word at {a:#x}: {what}")
    # the stated extents
    for f, size in ((0x800A4774, 12348), (0x800A8DF0, 504), (0x800AB7A0, 1752), (0x800ABE78, 696),
                    (0x800A8FE8, 452), (0x800A8C78, 376), (0x800B3AD0, 7968)):
        lo, hi = extent(f)
        bench.check(hi - lo == size, f"extent of {f:#x}: {hi - lo} bytes")
    # COP2 / jalr census over every modelled function: the one GTE use is 0x800AC958's OP
    cop2, jalr = {}, 0
    for f in CENSUS:
        lo, hi = extent(f)
        for a in range(lo, hi, 4):
            w = word_at(a)
            if (w >> 26) == 0x12 or (w >> 26) in (0x32, 0x3A):
                cop2[f] = cop2.get(f, 0) + 1
            if (w >> 26) == 0 and (w & 0x3F) == 9:
                jalr += 1
    bench.check(cop2 == {0x800AC958: 10} and jalr == 0 and word_at(0x800ACD68) == 0x4B78000C,
                f"COP2/jalr census of {len(CENSUS)} functions: {[(hex(k), v) for k, v in cop2.items()]}, jalr {jalr}")
    if verbose:
        print(f"  {len(SKELETON)} call sites, {len(TABLES)} tables, {len(WORDS)} words, {len(CENSUS)} functions censused")


_COVER = set()


def _tracer(frame, event, arg):
    if frame.f_code.co_filename == __file__:
        if event == "line":
            _COVER.add(frame.f_lineno)
        return _tracer
    return None


def uncovered(fn):
    """source lines of a model (and the helpers it names) the live runs never executed"""
    import inspect
    src, first = inspect.getsourcelines(fn)
    out = []
    for i, ln in enumerate(src):
        t = ln.strip()
        if not t or t.startswith(("#", "def ", '"""', "else:", "return None")) or t.endswith('"""'):
            continue
        if first + i not in _COVER:
            out.append(first + i)
    return out


def check_live(bench, verbose=True, cover=False):
    if not os.path.exists(RRVERIFY):
        bench.check(False, f"{RRVERIFY} missing - the live comparison cannot run")
        return {}
    os.makedirs(os.path.join(WORK, "probe"), exist_ok=True)
    funcs = [f for f, _, _, _ in MODELS]
    agg = {f: Counter() for f in funcs}
    traces = {}
    for name, src, frames in RUNS:
        tr, stop, edits = run_trace(name, src, frames, funcs)
        traces[name] = tr
        if verbose:
            print(f"  {name}: {len(edits)} data edits of {src}; {'; '.join(stop)}")
        line = []
        for f, frame, model, nm in MODELS:
            if cover:
                sys.settrace(_tracer)
            res = run_model(tr, model, f, frame, watched)
            sys.settrace(None)
            ok = sum(1 for r in res if r[1] is True)
            bad = [r for r in res if r[1] is False]
            agg[f]["calls"] += ok + len(bad)
            agg[f]["match"] += ok
            agg[f]["truncated"] += sum(1 for r in res if r[1] is None)
            line.append(f"{nm} {ok}/{ok + len(bad)}")
            for r in bad[:2]:
                print(f"    MISMATCH {nm} {name} seq {r[0]}: {r[2]}")
        if verbose:
            print("    " + ", ".join(line))
    for f, frame, model, nm in MODELS:
        a = agg[f]
        bench.check(a["calls"] > 0 and a["match"] == a["calls"],
                    f"{nm} {f:#x}: model == interpreter on {a['match']}/{a['calls']} calls")
        if verbose:
            print(f"  {nm} {f:#x}: {a['match']}/{a['calls']} calls ({a['truncated']} cut off by the end of a run)")
    if cover:
        for fn in [m for _, _, m, _ in MODELS] + [dispatch, tail, cell_of, mid_wipeout]:
            print(f"  never executed in {fn.__name__}: lines {uncovered(fn)}")
    return traces


def frame1(tr):
    """RAM at the entry and at the exit of the first collision pass of a run"""
    (a, b), = passes(tr)[:1]
    snap = snapshots(tr, [a, b])
    return snap[a], snap[b]


def writes_in(tr, lo, hi, addr):
    return [(sq, pc, v) for sq, pc, a, n, v in tr.writes if lo < sq < hi and a == addr]


def check_contact(bench, traces, verbose=True):
    mut = MUTATE["on"]
    fx = lambda v: f"{s32(v) / 65536:.4f}"
    # 1. bike against bike, the player pushed sideways off AI bike 15
    tr = traces["bb-side"]
    (p0, p1), = passes(tr)[:1]
    r0, r1 = frame1(tr)
    cs = contacts(r1)
    disp = tuple(s32(rd(r1, PLAYER + 0xB8 + 4 * k) - rd(r0, PLAYER + 0xB8 + 4 * k)) for k in range(3))
    want_pushed = 0x0F if mut else 0x00
    ok = len(cs) == 1 and cs[0][0] == want_pushed and cs[0][1] == 0x0F and disp == cs[0][2]
    bench.check(ok, f"bb-side: one contact, the player (0) pushed by bike 15, and his box moved by exactly the "
                    f"logged vector: {[(hex(a), hex(b), [fx(x) for x in v], hex(w)) for a, b, v, w in cs]} "
                    f"disp {[fx(x) for x in disp]}")
    code = [r["v0"] for s, r in tr.probes.get(0x800ABA00, []) if p0 < s < p1]
    bench.check(code[:1] == [0x1F], f"bb-side: the classifier returns 31 (side by side, parallel, matched speed): {code}")
    riders = {rd(r0, PLAYER + 0x354, "<I"), rd(r0, BIKE15 + 0x354, "<I")}
    st = [(hex(a), v) for sq, pc, a, n, v in tr.writes if p0 < sq < p1 and pc == 0x800C306C and a - 0x220 in riders]
    bench.check(sorted(v for _, v in st) == [26, 26],
                f"bb-side: both riders are put in rider state 26 by SetRiderState inside the pass: {st}")
    if verbose:
        print(f"  bb-side frame 1: contacts {[(hex(a), hex(b), [fx(x) for x in v], hex(w)) for a, b, v, w in cs]}; "
              f"player box moved {[fx(x) for x in disp]}; classifier {code}; rider states {st}")
    # 2. two AI bikes, no player
    tr = traces["bb-ai"]
    hs = set()
    for a, b in passes(tr):
        if b is None:
            continue
        for x, y, v, w in contacts(snapshots(tr, [b])[b]):
            hs |= {x, y}
    bench.check(hs == {0x0E, 0x0F}, f"bb-ai: every contact record is between bikes 14 and 15: {sorted(hs)}")
    # 3. the traffic car on the player's spot
    tr = traces["tr-over0"]
    (p0, p1), = passes(tr)[:1]
    r0, r1 = frame1(tr)
    cs = contacts(r1)
    disp = tuple(s32(rd(r1, PLAYER + 0xB8 + 4 * k) - rd(r0, PLAYER + 0xB8 + 4 * k)) for k in range(3))
    v240 = writes_in(tr, p0, p1, PLAYER + 0x240)
    ok = len(cs) == 1 and cs[0][:2] == (0x00, 0x60) and disp == cs[0][2]
    bench.check(ok, f"tr-over0: the player pushed out of the car (0x60) by exactly the logged vector: "
                    f"{[(hex(a), hex(b), [fx(x) for x in v]) for a, b, v, w in cs]} disp {[fx(x) for x in disp]}")
    bench.check(len(v240) == 1 and s32(v240[0][2]) > s32(rd(r0, PLAYER + 0x240)) + 0x40000,
                f"tr-over0: the car hands the player speed: +0x240 {fx(rd(r0, PLAYER + 0x240))} -> "
                f"{[(hex(pc), fx(v)) for _, pc, v in v240]} (car +0x1E0 {fx(rd(r0, CAR + 0x1E0))})")
    if verbose:
        print(f"  tr-over0 frame 1: contacts {[(hex(a), hex(b), [fx(x) for x in v], hex(w)) for a, b, v, w in cs]}; "
              f"player box moved {[fx(x) for x in disp]}; +0x240 {[(hex(pc), fx(v)) for _, pc, v in v240]}")
    # 4. the unmodified savestates have no contact: the edits are what make one
    for name in ("rr-race", "quick"):
        tr = traces[name]
        n = 0
        for a, b in passes(tr):
            if b is not None:
                n += len(contacts(snapshots(tr, [b])[b]))
        bvb = sum(1 for c in tr.calls if c[2] == 0x800AB7A0)
        want = (n >= 1) if mut and name == "rr-race" else (n == 0 and bvb == 0)
        bench.check(want, f"{name} unmodified: {n} contact records and {bvb} bike pair calls over the run")


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["info", "live", "verify"])
    ap.add_argument("--mutate", action="store_true")
    ap.add_argument("--cover", action="store_true", help="report the model lines no live run executed")
    a = ap.parse_args(argv)
    MUTATE["on"] = a.mutate
    bench = Bench()
    print("skeleton:")
    check_skeleton(bench)
    if a.cmd in ("live", "verify"):
        print("live:")
        traces = check_live(bench, cover=a.cover)
        if traces:
            print("contact:")
            check_contact(bench, traces)
    print(f"pairs: {bench.n} checks, {bench.fail} failures")
    return 1 if (a.cmd == "verify" and bench.fail and not a.mutate) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
