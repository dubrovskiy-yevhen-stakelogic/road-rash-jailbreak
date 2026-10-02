"""Scout probe for THE RACE SPINE'S SEAMS (Road Rash: Jailbreak, USA, SLUS_01053).

Each seam function of the race-spine bench rows is transcribed below as an executable Python
model, one statement per original load/store/call, in the original's order:

    SLUS    0x80043F00  GetRCnt              SLUS    0x8001447C  Malloc (+ 0x800142B4)
    SLUS    0x80027178  EffectFindFree       SLUS    0x800271CC  EffectLink
    SLUS    0x8002705C  EffectJitterSpray    SLUS    0x800270F0  EffectJitterBurst
    SLUS    0x800289E8  EffectLocalToWorld   SLUS    0x80027540  CrashEmit
    SLUS    0x80027778  EffectBurst          SLUS    0x80027974  EffectSpray
    SLUS    0x8002076C  ReleaseContact       SLUS    0x8002090C  ResetBikeState
    RASHCDG 0x80090270  RaceGo               RASHCDG 0x8008A998  ViewEvent
    RASHCDG 0x800BC7CC  StampResult

What it proves.  For every function it builds synthetic cases from the `rr-race` snapshot (the
player's own RAM, which holds the resident EXE and RASHCDG), then

  * runs the ORIGINAL bytes of the function on SpineCpu, a small R3000 integer executor (the
    project's own, adapted from roadq.py's PyCpu), with its callees executed as original code too,
    except two that cannot run in an integer executor and are supplied identically to both sides:
    the GTE dot product SLUS 0x8002E698 (a Python model) and the stance event RASHCDG 0x800C4550
    (opaque: recorded, no effect).  A load of a root-counter VALUE register answers the case's
    counter value, as the bench's concession does;
  * runs the MODEL on a second copy of the same RAM.  A callee the model calls is the ORIGINAL
    callee's bytes on SpineCpu (the same two exceptions), so a green case means: given the
    callees, the transcription does what the original body does;
  * compares the whole 2 MiB RAM outside the stack window, v0 where the function returns one, and
    the sequence of calls the function body makes (target and declared arguments).

It also checks, statically, the images' SHA-1, that the snapshot holds the images' bytes over every
function, each function's extent (its one `jr ra`) and its exact callee set.

Reads ONLY work\\disc_us and work\\oracle\\state (both gitignored).  Writes ONLY to stdout.

Usage (from the project root):

    python tools\\scout\\spine.py verify [--cases N] [--seed S] [--only NAME] [--verbose]
    python tools\\scout\\spine.py verify --mutate [--cases N]   # every mutant must fail ITS OWN function

Each function draws its cases from its own stream (seed and name), so a mutant's run on its owner
sees exactly the cases `verify` gives that function. Each command ends with one line a gate can match:

    spine: <n> checks, <m> failures
    spine --mutate: <k> of <n> mutants caught by their own function, <n - k> missed
"""

from __future__ import annotations

import argparse
import json
import os
import random
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import exe as E  # noqa: E402

ROOT = E.ROOT
STATE = os.path.join(ROOT, "work", "oracle", "state", "rr-race")
SLUS_SHA1 = "67ed165a2c517d4e6106fb0dfa324d66dd9a76f1"
G_SHA1 = "cfe43a7786759f2cb9c57751cf99e84d1074782c"
G_BASE = 0x8005B5E8

# name: (entry, end = the address after the delay slot of its one `jr ra`, frame, callees, image)
FUNCS = {
    "get_rcnt": (0x80043F00, 0x80043F38, 0, set(), "S"),
    "find_free": (0x80027178, 0x800271CC, 0, set(), "S"),
    "link": (0x800271CC, 0x80027258, 0, set(), "S"),
    "jitter_spray": (0x8002705C, 0x800270F0, 32, {0x80043F00}, "S"),
    "jitter_burst": (0x800270F0, 0x80027178, 24, {0x80043F00}, "S"),
    "local_to_world": (0x800289E8, 0x80028C78, 40, set(), "S"),
    "crash_emit": (0x80027540, 0x80027778, 32, {0x80027178, 0x800271CC}, "S"),
    "burst": (0x80027778, 0x80027974, 48, {0x80027178, 0x800289E8, 0x800270F0, 0x800271CC}, "S"),
    "spray": (0x80027974, 0x80027B80, 40, {0x80027178, 0x8002705C, 0x800271CC}, "S"),
    "malloc": (0x8001447C, 0x800144B8, 24, {0x800142B4}, "S"),
    "heap_alloc": (0x800142B4, 0x80014370, 24, {0x80044894}, "S"),
    "release_contact": (0x8002076C, 0x8002090C, 32, {0x8002E698, 0x8001FC90, 0x8002EE50}, "S"),
    "reset_bike_state": (0x8002090C, 0x80020BEC, 32,
                         {0x8002076C, 0x8001FC90, 0x80010028, 0x8001FF3C, 0x80020018}, "S"),
    "race_go": (0x80090270, 0x800903F4, 0, set(), "G"),
    "view_event": (0x8008A998, 0x8008AAB0, 0, set(), "G"),
    "stamp_result": (0x800BC7CC, 0x800BC8DC, 40, {0x800C4550, 0x800BCD10, 0x800BCA68}, "G"),
}
CASE_FUNCS = list(FUNCS)

GAME_STATE = 0x8005B2F8
POOL_TABLE = 0x800CE4D0
POOL0 = 0x8005B3A0
LIVE = 0x8005B1F8
EFFECTS = 0x800D39B0
LAST_STAMP = 0x8005B360
HEAP = 0x800D6500
VIEWS = 0x800CD898
STANCE_TABLE = 0x800541D4
GTE_DOT = 0x8002E698
STANCE = 0x800C4550
STOP = 0x80000180
SP0 = 0x801FF000

MUT: set[str] = set()
# mutant -> the function whose model it perturbs: a mutant counts as caught only when THAT
# function's own cases fail (a model calls the ORIGINAL callees, so a mutant cannot leak elsewhere)
MUTANTS = {
    "rcnt_bound": "get_rcnt", "free_state_bits": "find_free", "link_unsigned": "link",
    "spray_120_bound": "jitter_spray", "burst_101": "jitter_burst", "l2w_uncrossed": "local_to_world",
    "emit_pending_ge": "crash_emit", "emit_twolive": "crash_emit", "emit_part_b1": "crash_emit",
    "burst_half": "burst", "burst_stamp_skip": "burst", "spray_speed_clamp": "spray",
    "spray_kind1_bits": "spray", "go_count_police": "race_go", "go_partner_slot": "race_go",
    "malloc_heap_arg": "malloc", "malloc_split_rest": "heap_alloc", "view_same": "view_event",
    "view_arm12": "view_event", "release_floor": "release_contact", "reset_mask_230": "reset_bike_state",
    "reset_157": "reset_bike_state", "reset_partner_gate": "reset_bike_state",
    "stamp_top_index": "stamp_result", "stamp_second_target": "stamp_result",
}


def mut(name: str) -> bool:
    return name in MUT


def u(x): return x & 0xFFFFFFFF
def s32(x): x &= 0xFFFFFFFF; return x - (1 << 32) if x & 0x80000000 else x
def s16(x): x &= 0xFFFF; return x - 0x10000 if x & 0x8000 else x
def s8(x): x &= 0xFF; return x - 0x100 if x & 0x80 else x
def fixmul(a, b): p = s32(a) * s32(b); return u(p >> 16)


# ---------------------------------------------------------------------------- the machine
class Stop(Exception):
    pass


class Ram:
    """Guest RAM, KSEG0/KSEG1/KUSEG over the 8 MiB mirror of 2 MiB; anything else stops."""

    def __init__(self, data: bytearray, rc: int):
        self.m = data
        self.rc = rc

    def phys(self, a, n):
        a = u(a)
        if a % n:
            raise Stop(f"misaligned {n}-byte access 0x{a:08X}")
        seg = a & 0x1FFFFFFF if a >= 0x80000000 else a
        if seg >= 0x800000:
            raise Stop(f"access outside RAM 0x{a:08X}")
        return seg & 0x1FFFFF

    def ld(self, a, n):
        seg = u(a) & 0x1FFFFFFF
        if seg in (0x1F801100, 0x1F801110, 0x1F801120):
            return self.rc & ((1 << (8 * n)) - 1)
        o = self.phys(a, n)
        return int.from_bytes(self.m[o:o + n], "little")

    def st(self, a, n, v):
        o = self.phys(a, n)
        self.m[o:o + n] = (v & ((1 << (8 * n)) - 1)).to_bytes(n, "little")

    def w(self, a): return self.ld(a, 4)
    def h(self, a): return self.ld(a, 2)
    def b(self, a): return self.ld(a, 1)
    def W(self, a, v): self.st(a, 4, v)
    def H(self, a, v): self.st(a, 2, v)
    def B(self, a, v): self.st(a, 1, v)


def gte_dot(ram: Ram, a0, a1):
    v = [s16(ram.h(a0 + 2 * k)) for k in range(3)]
    m = [s16(ram.h(a1 + 2 * k)) for k in range(3)]
    return u(s32(sum(v[k] * m[k] for k in range(3))) >> 8)


class SpineCpu:
    """R3000 integer executor. `run(pc, args, sp)` returns v0; calls made by code in `body` are
    recorded as (target, a0..a3)."""

    def __init__(self, ram: Ram, body=None):
        self.ram = ram
        self.body = body
        self.calls = []
        self._callpend = None

    def call(self, pc, args, sp, max_steps=2_000_000):
        r = [0] * 32
        for i, v in enumerate(args[:4]):
            r[4 + i] = u(v)
        r[29] = u(sp)
        r[31] = STOP
        hi = lo = 0
        npc = u(pc + 4)
        pending = None
        ram = self.ram
        for _ in range(max_steps):
            if pc == STOP:
                return r[2]
            if self._callpend is not None and pc == self._callpend:
                # the arguments at the callee's entry: after the delay slot, with a load the delay
                # slot issued already visible
                regs = list(r)
                if pending is not None and pending[0] != 0:
                    regs[pending[0]] = pending[1]
                self.calls.append((pc, regs[4], regs[5], regs[6], regs[7]))
                self._callpend = None
            if pc in (GTE_DOT, STANCE):
                # supplied, identically on both sides: the GTE dot product, and the stance event
                # as an opaque witness (no effect).
                if pending is not None and pending[0] != 0:
                    r[pending[0]] = pending[1]
                pending = None
                r[2] = gte_dot(ram, r[4], r[5]) if pc == GTE_DOT else 0
                pc, npc = r[31], u(r[31] + 4)
                continue
            w = ram.w(pc)
            op = w >> 26; rs = (w >> 21) & 31; rt = (w >> 16) & 31; rd = (w >> 11) & 31
            sa = (w >> 6) & 31; fn = w & 63; imm = w & 0xFFFF; simm = imm - 0x10000 if imm & 0x8000 else imm
            written = None; val = 0; newpend = None; nnpc = u(npc + 4)
            A = r[rs]; B = r[rt]
            if op == 0:
                if fn == 0x00: val = u(B << sa); written = rd
                elif fn == 0x02: val = B >> sa; written = rd
                elif fn == 0x03: val = u(s32(B) >> sa); written = rd
                elif fn == 0x04: val = u(B << (A & 31)); written = rd
                elif fn == 0x06: val = B >> (A & 31); written = rd
                elif fn == 0x07: val = u(s32(B) >> (A & 31)); written = rd
                elif fn == 0x08: nnpc = A
                elif fn == 0x09:
                    self._rec(pc, A, r)
                    val = u(pc + 8); written = rd; nnpc = A
                elif fn == 0x10: val = hi; written = rd
                elif fn == 0x11: hi = A
                elif fn == 0x12: val = lo; written = rd
                elif fn == 0x13: lo = A
                elif fn == 0x18:
                    p = s32(A) * s32(B); lo = u(p); hi = u(p >> 32)
                elif fn == 0x19:
                    p = A * B; lo = u(p); hi = u(p >> 32)
                elif fn == 0x1A:
                    n_, d_ = s32(A), s32(B)
                    if d_ == 0: lo = 1 if n_ < 0 else 0xFFFFFFFF; hi = A
                    elif n_ == -0x80000000 and d_ == -1: lo = 0x80000000; hi = 0
                    else:
                        q = abs(n_) // abs(d_)
                        if (n_ < 0) != (d_ < 0): q = -q
                        lo = u(q); hi = u(n_ - q * d_)
                elif fn == 0x1B:
                    if B == 0: lo = 0xFFFFFFFF; hi = A
                    else: lo = A // B; hi = A % B
                elif fn in (0x20, 0x22):
                    x = s32(A) + s32(B) if fn == 0x20 else s32(A) - s32(B)
                    if not -0x80000000 <= x <= 0x7FFFFFFF: raise Stop(f"overflow at 0x{pc:08X}")
                    val = u(x); written = rd
                elif fn == 0x21: val = u(A + B); written = rd
                elif fn == 0x23: val = u(A - B); written = rd
                elif fn == 0x24: val = A & B; written = rd
                elif fn == 0x25: val = A | B; written = rd
                elif fn == 0x26: val = A ^ B; written = rd
                elif fn == 0x27: val = u(~(A | B)); written = rd
                elif fn == 0x2A: val = 1 if s32(A) < s32(B) else 0; written = rd
                elif fn == 0x2B: val = 1 if A < B else 0; written = rd
                else: raise Stop(f"SPECIAL 0x{fn:02X} at 0x{pc:08X}")
            elif op == 1:
                cond = (s32(A) < 0) if rt in (0x00, 0x10) else (s32(A) >= 0)
                if rt in (0x10, 0x11): val = u(pc + 8); written = 31
                if cond: nnpc = u(npc + (simm << 2))
            elif op in (2, 3):
                tgt = (npc & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                if op == 3:
                    self._rec(pc, tgt, r)
                    val = u(pc + 8); written = 31
                nnpc = tgt
            elif op == 4:
                if A == B: nnpc = u(npc + (simm << 2))
            elif op == 5:
                if A != B: nnpc = u(npc + (simm << 2))
            elif op == 6:
                if s32(A) <= 0: nnpc = u(npc + (simm << 2))
            elif op == 7:
                if s32(A) > 0: nnpc = u(npc + (simm << 2))
            elif op == 8:
                x = s32(A) + simm
                if not -0x80000000 <= x <= 0x7FFFFFFF: raise Stop(f"overflow at 0x{pc:08X}")
                val = u(x); written = rt
            elif op == 9: val = u(A + simm); written = rt
            elif op == 10: val = 1 if s32(A) < simm else 0; written = rt
            elif op == 11: val = 1 if A < u(simm) else 0; written = rt
            elif op == 12: val = A & imm; written = rt
            elif op == 13: val = A | imm; written = rt
            elif op == 14: val = A ^ imm; written = rt
            elif op == 15: val = u(imm << 16); written = rt
            elif op in (0x20, 0x21, 0x23, 0x24, 0x25):
                n = {0x20: 1, 0x24: 1, 0x21: 2, 0x25: 2, 0x23: 4}[op]
                v = ram.ld(u(A + simm), n)
                if op == 0x20 and v & 0x80: v = u(v - 0x100)
                if op == 0x21 and v & 0x8000: v = u(v - 0x10000)
                newpend = (rt, v)
            elif op in (0x28, 0x29, 0x2B):
                n = {0x28: 1, 0x29: 2, 0x2B: 4}[op]
                ram.st(u(A + simm), n, B)
            elif op in (0x22, 0x26):
                # lwl / lwr (little-endian); the value merged into is the one a pending load of the
                # same register is about to deliver, as the R3000 forwards it
                cur = pending[1] if pending is not None and pending[0] == rt else B
                a_ = u(A + simm)
                wv = ram.ld(a_ & ~3, 4)
                sh = a_ & 3
                if op == 0x22:
                    v = (cur & (0x00FFFFFF >> (8 * sh))) | u(wv << (24 - 8 * sh))
                else:
                    v = (cur & u(0xFFFFFF00 << (24 - 8 * sh))) | (wv >> (8 * sh)) if sh else wv
                newpend = (rt, u(v))
            elif op in (0x2A, 0x2E):
                a_ = u(A + simm)
                mv = ram.ld(a_ & ~3, 4)
                sh = a_ & 3
                if op == 0x2A:
                    v = (mv & u(0xFFFFFF00 << (8 * sh))) | (B >> (24 - 8 * sh)) if sh < 3 else B
                else:
                    v = (mv & (0x00FFFFFF >> (24 - 8 * sh))) | u(B << (8 * sh)) if sh else B
                ram.st(a_ & ~3, 4, u(v))
            else:
                raise Stop(f"opcode 0x{op:02X} at 0x{pc:08X}")
            if written is not None and written != 0: r[written] = val
            if pending is not None and pending[0] != written and pending[0] != 0: r[pending[0]] = pending[1]
            pending = newpend
            pc, npc = npc, nnpc
        raise Stop("step budget exhausted")

    def _rec(self, pc, tgt, r):
        if self.body is not None and self.body[0] <= pc < self.body[1]:
            self._callpend = tgt


class Model:
    """What a transcription runs on: the RAM view, and the ORIGINAL callees on SpineCpu."""

    def __init__(self, ram: Ram, sp: int):
        self.ram = ram
        self.sp = sp
        self.calls = []

    def call(self, tgt, *args, arity=4):
        a = list(args) + [0] * (4 - len(args))
        self.calls.append((tgt, *a[:4]))
        if tgt in (GTE_DOT, STANCE):
            return gte_dot(self.ram, a[0], a[1]) if tgt == GTE_DOT else 0
        return SpineCpu(self.ram).call(tgt, a, self.sp - 512)


# ---------------------------------------------------------------------------- the models
def m_get_rcnt(M: Model, spec):
    r = M.ram
    idx = spec & 0xFFFF
    if not (idx < 3 if not mut("rcnt_bound") else idx < 4):
        return 0
    return r.h(u((idx << 4) + r.w(0x800549B8)))


def m_find_free(M: Model):
    r = M.ram
    p = r.w(0x800D8068)
    i = 0
    while True:
        st = (r.w(p) >> 6) & (0xF if not mut("free_state_bits") else 0x7)
        if st == 0:
            break
        i += 1
        p = u(p + 112)
        if not i < 20:
            break
    return i if i < 20 else 0xFFFFFFFF


def m_link(M: Model, e, idx):
    r = M.ram
    head = s8(r.b(e + 0x49))
    if head == -1:
        r.B(e + 0x49, idx)
        return
    rec = u(EFFECTS + 112 * head)
    w = r.w(rec)
    while (w & 0x3F) != 63:
        nxt = (w & 0x3F) if mut("link_unsigned") else s32(u(w << 26)) >> 26
        rec = u(EFFECTS + 112 * nxt)
        w = r.w(rec)
    r.W(rec, (r.w(rec) & ~0x3F) | (idx & 0x3F))


def m_jitter_spray(M: Model, rec, speed):
    r = M.ram
    r1 = M.call(0x80043F00, 0xF2000002)
    r.H(rec + 0x38, (r1 & 0xFF) << 4)
    if speed == 0:
        r.H(rec + 0x3A, 50)
    elif s32(speed) < (18 if not mut("spray_120_bound") else 17):
        r.H(rec + 0x3A, 120)
    else:
        r2 = M.call(0x80043F00, 0xF2000002) & 0xFF
        v = s32(u((r2 << 8) - r2)) >> 8
        v = s32(u(v - 127) << 24) >> 24
        r.H(rec + 0x3A, u(v))


def m_jitter_burst(M: Model, rec):
    r = M.ram
    r1 = M.call(0x80043F00, 0xF2000002)
    r.H(rec + 0x38, (r1 & 0xFF) << 4)
    r2 = M.call(0x80043F00, 0xF2000002) & 0xFF
    v = r2 * (101 if not mut("burst_101") else 100)
    sv = s32(u(v << 16)) >> 24
    r.H(rec + 0x3A, u(sv))
    if not s16(sv) < 51:
        r.H(rec + 0x3A, u(50 - sv))


def m_local_to_world(M: Model, e, v, out):
    r = M.ram
    sc = (0x130, 0x138, 0x134) if not mut("l2w_uncrossed") else (0x130, 0x134, 0x138)
    x = fixmul(r.w(e + sc[0]), r.w(v))
    y = fixmul(r.w(e + sc[1]), r.w(v + 4))
    z = fixmul(r.w(e + sc[2]), r.w(v + 8))
    res = []
    for k in range(3):
        m0 = u(s16(r.h(e + 0x1B0 + 2 * k)) << 4)
        m1 = u(s16(r.h(e + 0x1B6 + 2 * k)) << 4)
        m2 = u(s16(r.h(e + 0x1BC + 2 * k)) << 4)
        t = u(fixmul(m0, x) + fixmul(m1, y) + fixmul(m2, z) + r.w(e + 0xB8 + 4 * k))
        res.append(u(s32(t) >> 10))
    for k in range(3):
        r.W(out + 4 * k, res[k])


def m_crash_emit(M: Model, e, which, kind):
    r = M.ram
    gs = r.w(GAME_STATE)
    if not r.h(e + 0xAC) < r.w(gs + 0x30):
        return
    if (s8(r.b(e + 8)) >= 0) if mut("emit_pending_ge") else (s8(r.b(e + 8)) > 0):
        return
    if not (r.w(e + 0x24) >> 30) < (2 if not mut("emit_twolive") else 3):
        return
    i = M.call(0x80027178)
    if i == 0xFFFFFFFF:
        return
    w24 = r.w(e + 0x24)
    r.W(e + 0x24, (w24 & 0x3FFFFFFF) | u(((w24 >> 30) + 1) << 30))
    rec = u(EFFECTS + 112 * s32(i))
    w = r.w(rec)
    r.B(rec + 0x3C, 0)
    w = (w & ~0x3C0) | 0x100
    r.W(rec, w)
    w = (w & 0xFFC03FFF) | ((which & 0xFF) << 14)
    w = (w & 0xFFFFC3FF) | 0x400
    gs1 = r.w(GAME_STATE)
    r.W(rec, w)
    clock = r.w(gs1 + 0x10)
    r.B(rec + 0x3D, 0)
    gs2 = r.w(GAME_STATE)
    r.W(rec + 0x34, 900); r.H(rec + 0x3E, 30); r.W(rec + 0x60, 9)
    r.W(rec + 0x24, 0); r.W(rec + 0x2C, 0); r.W(rec + 0x28, 0); r.W(rec + 0x30, clock)
    clock2 = r.w(gs2 + 0x10)
    r.B(rec + 0x6C, kind); r.B(rec + 0x6D, 4); r.W(rec + 0x64, clock2)
    for k in range(4):
        r.B(rec + 0x68 + k, k + 1)
    ta = u(0x800537DA + 6 * r.w(e + 0xB4) + ((r.w(rec) >> 13) & 0x1FE))
    model = r.w(e)
    b1 = s8(r.b(ta + (1 if not mut("emit_part_b1") else 0)))
    parts = r.h(model + 24)
    b0 = s8(r.b(ta))
    tb = u(0x800536F0 + 4 * b1)
    node = r.w(u(24 * parts + r.w(e + 4) - 24))
    base = u(r.w(node + 20) + 20 * b0 + 4)
    r.H(rec + 0x4C, r.h(u(base + 2 * s8(r.b(tb + 2)) + 12)))
    r.H(rec + 0x4E, r.h(u(base + 2 * s8(r.b(tb + 3)) + 12)))
    M.call(0x800271CC, e, i)


def m_burst(M: Model, e, kind, life, tag):
    r = M.ram
    count = (r.w(e + 0x24) >> 19) & 0xF
    half = (r.b(r.w(GAME_STATE) + 4) >> 4) & 1
    if mut("burst_half"):
        half = 0
    if not count < (20 >> half):
        return
    i = M.call(0x80027178)
    if i == 0xFFFFFFFF:
        return
    gs = r.w(GAME_STATE)
    last = r.w(LAST_STAMP)
    if r.w(gs + 0x10) == last and not mut("burst_stamp_skip"):
        return
    rec = u(EFFECTS + 112 * s32(i))
    w = r.w(rec)
    r.W(rec + 0x34, life)
    w = (w & 0xFFFFC3FF) | 0x400
    w = (w & ~0x3C0) | 0x1C0
    w &= 0xFFC03FFF
    w |= (kind & 0xFF) << 14
    clock = r.w(gs + 0x10)
    r.W(rec, w)
    r.W(LAST_STAMP, clock)
    r.B(rec + 0x3C, tag)
    w24 = r.w(e + 0x24)
    r.W(e + 0x24, (w24 & 0xFF87FFFF) | (((((w24 >> 19) & 0xF) + 1) & 0xF) << 19))
    M.call(0x800289E8, e, u(0x80053670 + 12 * kind), rec + 20)
    a0 = r.w(LAST_STAMP)
    r.B(rec + 0x6C, 0)
    v1 = r.w(LAST_STAMP)
    r.W(rec + 0x60, 300 >> half); r.B(rec + 0x6D, 1); r.W(rec + 0x30, a0); r.W(rec + 0x64, v1)
    for k in range(4):
        r.B(rec + 0x68 + k, 7 + k)
    M.call(0x800270F0, rec)
    r.B(rec + 0x3D, 1 if (kind ^ 1) else 0)
    r.W(rec + 0x40, 3); r.W(rec + 0x24, 0); r.W(rec + 0x2C, 0); r.W(rec + 0x28, 0); r.H(rec + 0x3E, 30)
    M.call(0x800271CC, e, i)


def m_spray(M: Model, e, _unused, kind):
    r = M.ram
    gs0 = r.w(GAME_STATE)
    player = r.h(e + 0xAC) < r.w(gs0 + 0x30)
    speed = s16(r.h(e + 0x1E2))
    if not player:
        return
    w24 = r.w(e + 0x24)
    if (w24 >> 5) & 3:
        return
    if kind == 0:
        if not ((w24 >> 19) & 0xF) < 4:
            return
        if ((w24 >> 23) & 3) > 0:
            return
    i = M.call(0x80027178)
    if i == 0xFFFFFFFF:
        return
    clock = r.w(r.w(GAME_STATE) + 0x10)
    if clock == r.w(LAST_STAMP):
        return
    r.W(LAST_STAMP, clock)
    rec = u(EFFECTS + 112 * s32(i))
    w = r.w(rec)
    w = (w & ~0x3C0) | 0x80
    w &= 0xFFC03FFF
    w |= (kind & 0xFF) << 14
    w = (w & 0xFFFFC3FF) | 0x400
    r.W(rec, w)
    if kind == 0:
        v = r.w(e + 0x24)
        r.W(e + 0x24, (v & 0xFF87FFFF) | (((((v >> 19) & 0xF) + 1) & 0xF) << 19))
        life = 30
    else:
        v = r.w(e + 0x24)
        bits = (0xFE7FFFFF, 23, 3) if not mut("spray_kind1_bits") else (0xFF87FFFF, 19, 0xF)
        r.W(e + 0x24, (v & bits[0]) | (((((v >> bits[1]) & bits[2]) + 1) & bits[2]) << bits[1]))
        life = 35
    r.H(rec + 0x3E, life)
    a2 = s32(u(150 - 2 * speed))
    lo = a2 if (a2 > 0 or mut("spray_speed_clamp")) else 0
    d = s32(u(0x960000 - a2))
    hi = d if d < 0 else 0     # unreachable from a s16 speed: 150 - 2 * speed <= 65686
    clock2 = r.w(r.w(GAME_STATE) + 0x10)
    r.W(rec + 0x34, u(lo + hi))
    r.W(rec + 0x30, clock2)
    M.call(0x8002705C, rec, u(speed))
    r.B(rec + 0x3D, 1); r.W(rec + 0x24, 0); r.W(rec + 0x2C, 0); r.W(rec + 0x28, 0); r.W(rec + 0x40, 9)
    M.call(0x800271CC, e, i)


def m_race_go(M: Model):
    r = M.ram
    high = s32(r.w(r.w(POOL_TABLE + 12)))
    e = r.w(POOL_TABLE)
    count = 0
    if high >= 0:
        gs = r.w(GAME_STATE)
        while high >= 0:
            if s16(r.h(e + 0x140)) == 0 and not r.h(e + 0xAC) < r.w(gs + 0x30):
                cls = r.b(r.w(e + 0x43C) + 1) & 0xF
                count += 1 if (cls ^ 2) != 0 or mut("go_count_police") else 0
            e = u(e + r.w(POOL_TABLE + 4))
            high -= 1
    high = s32(r.w(r.w(POOL_TABLE + 12)))
    e = r.w(POOL_TABLE)
    if high < 0:
        return
    gs = r.w(GAME_STATE)
    while high >= 0:
        rd = r.w(e + 0x43C)
        run = True
        if (r.b(rd + 1) & 0xF) == 2:
            run = r.h(e + 0xAC) < r.w(gs + 0x30)
        if run:
            slot = u(e + 0x3BC)
            if s16(r.h(e + 0x140)) == 0:
                r.H(slot + 4, 1)
            else:
                v = r.b(rd + 0x27) - count - 1
                v = max(v, 0) >> 1
                r.H(slot + 4, (v << 2) if v else 1)
                if r.b(r.w(e + 0x354) + 0x23C) & 0x10:
                    p = r.w(e + 0x358)
                    depth = s8(r.b(p + 0x3B2))
                    r.H(u(p + 8 * depth + (0x3B8 if not mut("go_partner_slot") else 0x3BC)), r.h(slot + 4))
        e = u(e + r.w(POOL_TABLE + 4))
        high -= 1


def m_malloc(M: Model, n, heap):
    if n == 0:
        return 0
    if not heap < 2:
        return 0
    return M.call(0x800142B4, n, 1 if mut("malloc_heap_arg") else 0)


def m_heap_alloc(M: Model, n, heap):
    r = M.ram
    prev = u(HEAP + 16 * heap)
    size = u(n + 11) & 0xFFFFFFF8
    blk = r.w(prev)
    while blk:
        have = r.w(blk + 4)
        if have == size:
            r.W(prev, r.w(blk)); r.W(blk, size)
            return u(blk + 4)
        if size < have:
            rest = u(blk + size)
            r.W(prev, rest); r.W(rest, r.w(blk))
            r.W(rest + 4, u(r.w(blk + 4) - size - (8 if mut("malloc_split_rest") else 0)))
            r.W(blk, size)
            return u(blk + 4)
        prev = blk
        blk = r.w(blk)
    raise Stop("heap: out of memory (the BIOS print arm)")


def m_view_event(M: Model, v, mode):
    r = M.ram
    same = mode == r.w(v + 0x220)
    if mut("view_same"):
        same = not same
    if mode < 4 and r.w(v + 0x21C) != mode:
        add = 2 if same else 6
        w224 = r.w(v + 0x224)
        r.W(v + 0x220, mode)
        t = r.w(v + 0x238)
        r.W(v + 0x224, (w224 & ~0x18 & 0xFFFFFFFF) | add)
        if r.h(t + 0xAC) >> 5:
            idx = 1 if r.h(v + 0xAC) == 0x9E else 0
            r.W(v + 0x238, r.w(0x8005B268 + 4 * idx))
    w228 = r.w(v + 0x228)
    r.W(v + 0x304, 0 if mode < 7 else 1)
    if w228 & 1:
        r.W(v + 0x224, r.w(v + 0x224) | 0x100)
    cleared = r.w(v + 0x228) & 0xFFFFFFFA
    arm = r.w(v + 0x21C)
    r.W(v + 0x228, cleared)
    r.W(v + 0x228, (cleared | 0x80) if arm == (12 if not mut("view_arm12") else 13) else (cleared & 0xFFFFFC7F))
    r.W(v + 0x21C, mode)
    if same and (r.w(v + 0x228) & 0x40):
        w = r.w(v + 0x228)
        r.W(v + 0x304, 1)
        r.W(v + 0x228, w | 0x10)


def m_release_contact(M: Model, e):
    r = M.ram
    c = r.w(e + 0x340)
    if c == 0:
        return
    h = r.h(c)
    pool = h >> 5
    slot = h & 0x1F
    if pool == 3:
        if (r.w(e + 0x234) & 0x60000) != 0x40000:
            return
        car = u(0x800CF660 + (slot << 9))
        d = M.call(GTE_DOT, car + 0x1C2, e + 0x1C2)
        f = M.call(0x8001FC90, r.w(car + 0x1E0), d)
        sv = u(r.w(e + 0x240) + f)
        r.W(e + 0x240, sv)
        if s32(sv) < 0x23C36 and not mut("release_floor"):
            sv = 0x23C36
        r.W(e + 0x240, sv)
        r.W(e + 0x1E0, sv)
        M.call(0x8002EE50, sv, e + 0x1C2, e + 0x1C8)
    elif pool < 4:
        if pool == 0:
            b = u(r.w(POOL0) + 1096 * slot)
            r.W(b + 0x230, r.w(b + 0x230) & 0xFDFFFFFF)
    elif pool == 4:
        p = u(r.w(0x800CD6D4) + 596 * slot)
        k = (r.h(u(r.w(p) + 14)) & 0xF80) >> 7
        if u(k - 3) < 3 and (r.w(p + 0x250) & 0x200):
            r.W(p + 0x22C, 0x10000)


def m_reset_bike_state(M: Model, e):
    r = M.ram
    stats = r.w(e + 0x22C)
    if r.w(e + 0x340):
        M.call(0x8002076C, e)
    f230 = r.w(e + 0x230)
    for off in (0x30C, 0x308, 0x304, 0x300, 0x2FC, 0x2F8, 0x2F4, 0x2F0, 0x2EC, 0x2D8, 0x2D4, 0x2D0, 0x2CC,
                0x2C8, 0x2C0, 0x2B8, 0x2BC, 0x2B0, 0x2A4, 0x2A8, 0x2AC, 0x2B4, 0x290, 0x294, 0x2A0, 0x284, 0x27C):
        r.W(e + off, 0)
    r.W(e + 0x230, f230 & (0xF8000000 if not mut("reset_mask_230") else 0xFC000000))
    r.W(e + 0x234, r.w(e + 0x234) & 0xC0000000)
    r.W(e + 0x238, r.w(e + 0x238) & 0xF7B00000)
    for off in (0x280, 0x268, 0x26C, 0x270, 0x248, 0x2E8, 0x1E8, 0x260, 0x25C, 0x254, 0x24C, 0x244, 0x240, 0x1E4):
        r.W(e + off, 0)
    m = s32(M.call(0x8001FC90, r.w(stats + 0x0C), 0x9D087))
    d = s32(r.w(stats + 0x10))
    if m > 0:
        m = s32(M.call(0x8001FC90, r.w(stats + 0x0C), 0x9D087))
        res = M.call(0x80010028, u(m), u(d)) if d > 0 else u(-s32(M.call(0x80010028, u(m), u(-d))))
    else:
        m = s32(M.call(0x8001FC90, r.w(stats + 0x0C), 0x9D087))
        res = u(-s32(M.call(0x80010028, u(-m), u(d)))) if d > 0 else M.call(0x80010028, u(-m), u(-d))
    r.W(e + 0x258, res)
    bc = r.w(stats + 0xBC)
    heading = s16(r.h(e + 0x212))
    r.B(e + 0x351, 0); r.W(e + 0x33C, 0); r.W(e + 0x3A4, 0); r.W(e + 0x2C4, 0)
    for off in (0x34E, 0x34C, 0x34A, 0x348, 0x346, 0x344, 0x33A):
        r.H(e + off, 0)
    r.W(e + 0x250, bc)
    r.H(e + 0x34A, M.call(0x8001FF3C, u(heading << 4)))
    k = 157 if not mut("reset_157") else 156
    a = u(s16(r.h(e + 0x206)) * k); r.W(e + 0x2DC, a)
    b = u(s16(r.h(e + 0x20C)) * k); r.W(e + 0x2E0, b)
    r.W(e + 0x2E4, u(s16(r.h(e + 0x212)) * k))
    ang = M.call(0x80020018, a, b)
    sc = s32(u(ang * 25736)) >> 8
    partner = r.w(e + 0x358)
    r.W(e + 0x29C, u(-sc))
    if partner and (r.w(e + 0x440) or mut("reset_partner_gate")):
        r.W(partner + 0x1E8, 0)
    r.W(e + 0x28C, u(r.w(e + 0x29C) + r.w(e + 0x27C)))


def m_stamp_result(M: Model, e):
    r = M.ram
    depth = s8(r.b(e + 0x3B2))
    top = r.h(u(e + 0x3BC + 8 * (depth - (1 if not mut("stamp_top_index") else 0))))
    if r.w(e + 0x230) & 0x08000000:
        rider = r.w(e + 0x354)
        if r.h(STANCE_TABLE + 8 * r.h(rider + 0x220) + 2) == 3:
            ev = r.h(u(r.w(0x8005AD4C) + 12 * r.b(rider + 0x239)))
            M.call(STANCE, ev, rider, 2)
    M.call(0x800BCD10, e)
    cmd = u(M.sp - 40 + 16)          # the original's own frame slot
    r.H(cmd, 2); r.H(cmd + 2, 224)
    M.call(0x800BCA68, cmd, 1, e)
    if top == 0 or top == 18:
        r.H(cmd, top)
        r.H(cmd + 2, r.h(e + 0xAC) if top == 18 and not mut("stamp_second_target") else 224)
        M.call(0x800BCA68, cmd, 0, e)
    r.H(e + 0x3B0, 0)
    r.W(e + 0x38C, 0)


MODELS = {
    "get_rcnt": (m_get_rcnt, 1, True), "find_free": (m_find_free, 0, True), "link": (m_link, 2, False),
    "jitter_spray": (m_jitter_spray, 2, False), "jitter_burst": (m_jitter_burst, 1, False),
    "local_to_world": (m_local_to_world, 3, False), "crash_emit": (m_crash_emit, 3, False),
    "burst": (m_burst, 4, False), "spray": (m_spray, 3, False), "race_go": (m_race_go, 0, False),
    "malloc": (m_malloc, 2, True), "heap_alloc": (m_heap_alloc, 2, True), "view_event": (m_view_event, 2, False),
    "release_contact": (m_release_contact, 1, False), "reset_bike_state": (m_reset_bike_state, 1, False),
    "stamp_result": (m_stamp_result, 1, False),
}
# the arity of each callee, read out of its own code (the call comparison uses only those registers)
ARITY = {0x80043F00: 1, 0x80027178: 0, 0x800271CC: 2, 0x800289E8: 3, 0x800270F0: 1, 0x8002705C: 2,
         0x800142B4: 2, 0x80044894: 3, GTE_DOT: 2, 0x8001FC90: 2, 0x8002EE50: 3, 0x8002076C: 1,
         0x80010028: 2, 0x8001FF3C: 1, 0x80020018: 2, STANCE: 3, 0x800BCD10: 1, 0x800BCA68: 3}


# ---------------------------------------------------------------------------- the cases
class Case:
    def __init__(self, ram: bytearray, args, rc):
        self.ram, self.args, self.rc = ram, args, rc


def rd32(b, a): return struct.unpack_from("<I", b, a & 0x1FFFFF)[0]
def wr32(b, a, v): struct.pack_into("<I", b, a & 0x1FFFFF, v & 0xFFFFFFFF)
def wr16(b, a, v): struct.pack_into("<H", b, a & 0x1FFFFF, v & 0xFFFF)
def wr8(b, a, v): b[a & 0x1FFFFF] = v & 0xFF


def bikes(ram):
    base = rd32(ram, POOL0)
    live = rd32(ram, LIVE)
    live = live if 1 <= live <= 24 else 1
    return [base + 1096 * i for i in range(live)]


def plant_effects(rnd, ram, e):
    mode = rnd.randrange(4)
    if mode:
        for i in range(20):
            rec = EFFECTS + 112 * i
            w = rd32(ram, rec)
            st = rnd.randrange(1, 16) if (mode == 1 or rnd.randrange(3)) else 0
            wr32(ram, rec, (w & ~0x3FF) | (st << 6) | 63)
    if rnd.randrange(2):
        order = rnd.sample(range(20), rnd.randrange(6))
        head = order[0] if order else 0xFF
        # a quarter of the chains end in a NEGATIVE link: the field is a signed 6-bit number, -1 is
        # 63 (the terminator), so the walk goes on to -2 = the word 224 bytes BELOW the pool, whose
        # own link is then planted as 63
        neg = order and rnd.randrange(4) == 0
        for k, i in enumerate(order):
            rec = EFFECTS + 112 * i
            nxt = order[k + 1] if k + 1 < len(order) else 63
            if neg and k + 1 == len(order):
                nxt = 0x3E
                below = EFFECTS - 224
                wr32(ram, below, (rd32(ram, below) & ~0x3F) | 63)
            wr32(ram, rec, (rd32(ram, rec) & ~0x3F) | nxt)
        wr8(ram, e + 0x49, head)


def make_case(name, rnd, snap):
    ram = bytearray(snap)
    rc = rnd.getrandbits(32)
    gs = rd32(ram, GAME_STATE)
    bs = bikes(ram)
    e = rnd.choice(bs) if rnd.randrange(4) == 0 else rnd.choice(bs[:2])
    scratch = 0x801E0000
    wr32(ram, gs + 0x30, rnd.randrange(3))
    clock = rnd.randrange(400000)
    wr32(ram, gs + 0x10, clock)
    wr32(ram, LAST_STAMP, clock if rnd.randrange(4) == 0 else rnd.randrange(400000))
    if name == "get_rcnt":
        return Case(ram, [rnd.choice([0xF2000002, 0xF2000000, 0xF2000001, 3, 0xFFFF, rnd.getrandbits(32)])], rc)
    if name in ("find_free", "link", "crash_emit", "burst", "spray"):
        plant_effects(rnd, ram, e)
    if name == "find_free":
        return Case(ram, [], rc)
    if name == "link":
        return Case(ram, [e, rnd.randrange(20) if rnd.randrange(5) else rnd.getrandbits(32)], rc)
    if name == "jitter_spray":
        sp = rnd.choice([0, 1, 17, 18, 0xFFFFFFFF, rnd.getrandbits(32) >> rnd.randrange(32)])
        return Case(ram, [EFFECTS + 112 * rnd.randrange(20), sp], rc)
    if name == "jitter_burst":
        return Case(ram, [EFFECTS + 112 * rnd.randrange(20)], rc)
    if name == "local_to_world":
        v = scratch
        for k in range(3):
            wr32(ram, v + 4 * k, s32(rnd.getrandbits(32)) >> rnd.randrange(24))
            if rnd.randrange(2):
                wr32(ram, e + 0x130 + 4 * k, s32(rnd.getrandbits(32)) >> rnd.randrange(20))
        for k in range(9):
            if rnd.randrange(2):
                wr16(ram, e + 0x1B0 + 2 * k, rnd.getrandbits(16))
        out = v if rnd.randrange(8) == 0 else scratch + 64
        return Case(ram, [e, v, out], rc)
    if name in ("crash_emit", "burst", "spray"):
        if rnd.randrange(2):
            wr32(ram, e + 0x24, rnd.getrandbits(32))
            wr8(ram, e + 0x08, 0 if rnd.randrange(2) else rnd.getrandbits(8))
            wr16(ram, e + 0x1E2, rnd.randrange(120) if rnd.randrange(2) else rnd.getrandbits(16))
            f4 = ram[(gs + 4) & 0x1FFFFF]
            wr8(ram, gs + 4, (f4 & ~0x10) | (0x10 if rnd.randrange(2) else 0))
        if name == "crash_emit":
            if rnd.randrange(2):
                wr32(ram, e + 0xB4, rnd.randrange(12))
            return Case(ram, [e, rnd.randrange(3), rnd.getrandbits(32) if rnd.randrange(3) == 0 else rnd.randrange(4)], rc)
        if name == "burst":
            return Case(ram, [e, 1 if rnd.randrange(2) else rnd.randrange(8), 600 if rnd.randrange(2) else rnd.getrandbits(32),
                              0 if rnd.randrange(2) else rnd.getrandbits(32)], rc)
        wr16(ram, e + 0x1E2, rnd.choice([0, 10, 74, 75, 76, 200, rnd.getrandbits(16)]))
        return Case(ram, [e, e + 0xB8, rnd.choice([0, 0, 1, 2, rnd.randrange(256)])], rc)
    if name == "race_go":
        for b in bs:
            wr16(ram, b + 0x140, 0 if rnd.randrange(3) == 0 else rnd.randrange(1, 4))
            rdp = rd32(ram, b + 0x43C)
            if rdp >= 0x80000000:
                c1 = ram[(rdp + 1) & 0x1FFFFF]
                if rnd.randrange(4) == 0:
                    c1 = (c1 & 0xF0) | (2 if rnd.randrange(2) else rnd.randrange(16))
                wr8(ram, rdp + 1, c1)
                wr8(ram, rdp + 0x27, rnd.getrandbits(8) if rnd.randrange(4) == 0 else rnd.randrange(1, 21))
            ow = rd32(ram, b + 0x354)
            if ow >= 0x80000000:
                two = rnd.randrange(3) == 0
                x = ram[(ow + 0x23C) & 0x1FFFFF]
                wr8(ram, ow + 0x23C, (x & ~0x10) | (0x10 if two else 0))
                if two:
                    pe = rnd.choice(bs)
                    wr32(ram, b + 0x358, pe)
                    wr8(ram, pe + 0x3B2, rnd.randrange(16))
        return Case(ram, [], rc)
    if name in ("malloc", "heap_alloc"):
        n = rnd.choice([0, 1, 4, 5, 12, 100, 2048, 4000, rnd.randrange(20000)])
        heap = rnd.choice([0, 0, 1, rnd.randrange(5)])
        if rnd.randrange(2):
            want = (n + 11) & ~7
            nxt = rd32(ram, HEAP)
            addrs = [scratch + 0x1000 + 904 * k for k in range(rnd.randrange(1, 5))]
            for a in reversed(addrs):
                roll = rnd.randrange(3)
                sz = want if roll == 0 else want + 8 * rnd.randrange(1, 9) if roll == 1 else max(8, want - 8 * rnd.randrange(1, 8))
                if sz > 900:
                    sz = 8 * rnd.randrange(1, 100)
                wr32(ram, a, nxt); wr32(ram, a + 4, sz)
                nxt = a
            wr32(ram, HEAP, nxt)
        return Case(ram, [n, heap if name == "malloc" else 0], rc)
    if name == "view_event":
        v = VIEWS + 1132 * rnd.randrange(2)
        wr32(ram, v + 0x21C, rnd.randrange(15) if rnd.randrange(6) else rnd.getrandbits(32))
        wr32(ram, v + 0x220, rnd.randrange(8) if rnd.randrange(6) else rnd.getrandbits(32))
        wr32(ram, v + 0x224, rnd.getrandbits(32))
        wr32(ram, v + 0x228, rnd.getrandbits(32))
        wr16(ram, v + 0xAC, 0x9E if rnd.randrange(2) else rnd.getrandbits(16))
        rider = rd32(ram, e + 0x354)
        wr32(ram, v + 0x238, rider if (rnd.randrange(2) and rider >= 0x80000000) else e)
        return Case(ram, [v, rnd.randrange(13) if rnd.randrange(6) else rnd.getrandbits(32)], rc)
    if name in ("release_contact", "reset_bike_state"):
        contact = scratch
        roll = rnd.randrange(6)
        if roll == 0:
            wr32(ram, e + 0x340, 0)
        else:
            pool = [0, 3, 4, 1, 5][roll - 1]
            slot = rnd.randrange(len(bs)) if pool == 0 else rnd.randrange(8)
            wr16(ram, contact, (pool << 5) | slot)
            wr32(ram, e + 0x340, contact)
            if pool == 3:
                car = 0x800CF660 + 512 * slot
                wr32(ram, car + 0x1E0, s32(rnd.getrandbits(32)) >> rnd.randrange(8, 16))
                for k in range(3):
                    wr16(ram, car + 0x1C2 + 2 * k, rnd.randrange(-4096, 4097))
        f = rd32(ram, e + 0x234)
        if rnd.randrange(2):
            f = (f & ~0x60000) | (0x20000 if rnd.randrange(3) == 0 else 0x40000)
        wr32(ram, e + 0x234, f)
        if rnd.randrange(2):
            wr32(ram, e + 0x230, rnd.getrandbits(32)); wr32(ram, e + 0x238, rnd.getrandbits(32))
            wr32(ram, e + 0x240, s32(rnd.getrandbits(32)) >> rnd.randrange(8, 18))
            for off in (0x206, 0x20C, 0x212):
                wr16(ram, e + off, rnd.getrandbits(16))
        if rnd.randrange(3) == 0:
            wr32(ram, e + 0x358, scratch + 1024)
            wr32(ram, e + 0x440, rnd.randrange(2))
        return Case(ram, [e], rc)
    if name == "stamp_result":
        if rnd.randrange(2):
            wr8(ram, e + 0x3B2, rnd.randrange(1, 16))
        d = s8(ram[(e + 0x3B2) & 0x1FFFFF])
        wr16(ram, e + 0x3BC + 8 * (d - 1), rnd.choice([0, 18, 4, 16]))
        f = rd32(ram, e + 0x230)
        wr32(ram, e + 0x230, (f & ~0x08000000) if rnd.randrange(3) == 0 else (f | 0x08000000))
        rider = rd32(ram, e + 0x354)
        if rider >= 0x80000000 and rnd.randrange(2):
            st = ram[(rider + 0x220) & 0x1FFFFF] | (ram[(rider + 0x221) & 0x1FFFFF] << 8)
            if st < 256:
                wr16(ram, STANCE_TABLE + 8 * st + 2, 3)
        return Case(ram, [e], rc)
    raise KeyError(name)


# ---------------------------------------------------------------------------- the checks
def static_checks(slus: E.Image, g: E.Image, snap: bytes) -> list[str]:
    out = []
    if slus.sha1 != SLUS_SHA1:
        out.append(f"SLUS_010.53 sha1 {slus.sha1}")
    if g.sha1 != G_SHA1:
        out.append(f"RASHCDG.BIN sha1 {g.sha1}")
    for name, (a, end, frame, callees, img) in FUNCS.items():
        im = slus if img == "S" else g
        if bytes(snap[a & 0x1FFFFF:end & 0x1FFFFF]) != im.data[im.off(a):im.off(end)]:
            out.append(f"{name}: the snapshot does not hold the image's bytes")
        words = [im.word(x) for x in range(a, end, 4)]
        jrs = [a + 4 * i for i, w in enumerate(words) if w == 0x03E00008]
        leaf = frame == 0
        if not jrs or jrs[-1] != end - 8:
            out.append(f"{name}: its last `jr ra` is not at 0x{end - 8:08X} ({[hex(j) for j in jrs]})")
        if not leaf and len(jrs) != 1:
            out.append(f"{name}: {len(jrs)} `jr ra` in a non-leaf")
        if frame and words[0] != (0x27BD0000 | ((-frame) & 0xFFFF)) and (a, frame) not in (
                (0x80027540, 32), (0x80027974, 40)):
            out.append(f"{name}: first word 0x{words[0]:08X} is not addiu sp,sp,-{frame}")
        found = {(((a + 4 * i + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)) for i, w in enumerate(words) if w >> 26 == 3}
        if found != callees:
            out.append(f"{name}: callees {sorted(hex(x) for x in found)} != {sorted(hex(x) for x in callees)}")
        if any((w >> 26) == 0 and (w & 63) == 9 for w in words):
            out.append(f"{name}: a jalr")
    return out


STATS = {}


def run_case(name, case, verbose=False):
    entry, end, frame, callees, _ = FUNCS[name]
    fn, nargs, has_v0 = MODELS[name]
    ga = Ram(bytearray(case.ram), case.rc)
    ma = Ram(bytearray(case.ram), case.rc)
    cpu = SpineCpu(ga, body=(entry, end))
    gstop = mstop = None
    try:
        gv = cpu.call(entry, case.args, SP0)
    except Stop as ex:
        gstop, gv = str(ex), None
    M = Model(ma, SP0)
    try:
        mv = fn(M, *case.args[:nargs])
    except Stop as ex:
        mstop, mv = str(ex), None
    if gstop or mstop:
        if (gstop is None) != (mstop is None):
            return f"guest stop {gstop!r}, model stop {mstop!r}"
        return None   # both stop: the console crashes / the port refuses; nothing else is claimed
    lo, hi = (SP0 - 4096) & 0x1FFFFF, (SP0 + 16) & 0x1FFFFF
    if ga.m[:lo] != ma.m[:lo] or ga.m[hi:] != ma.m[hi:]:
        diff = next(i for i in range(len(ga.m)) if ga.m[i] != ma.m[i] and not lo <= i < hi)
        return f"RAM differs first at 0x{0x80000000 + diff:08X}: guest {ga.m[diff]:02X} model {ma.m[diff]:02X}"
    STATS[name][0] += 1 if ga.m != bytearray(case.ram) else 0
    STATS[name][1] += 1 if cpu.calls else 0
    if has_v0 and u(gv) != u(mv if mv is not None else 0):
        return f"v0 guest 0x{u(gv):08X} model 0x{u(mv or 0):08X}"
    gc = [c[:1 + ARITY.get(c[0], 4)] for c in cpu.calls]
    mc = [tuple(u(x) for x in c[:1 + ARITY.get(c[0], 4)]) for c in M.calls]
    # the stack-frame command record of StampResult is the original's own slot: same address both sides
    if [tuple(u(x) for x in c) for c in gc] != mc:
        return f"calls differ: guest {[tuple(hex(x) for x in c) for c in gc][:4]} model {[tuple(hex(x) for x in c) for c in mc][:4]}"
    return None


def verify(cases: int, seed: int, only, verbose) -> tuple[int, int, list]:
    slus = E.load_exe()
    g = E.load_overlay("RASHCDG.BIN", G_BASE)
    snap = open(os.path.join(STATE, "ram.bin"), "rb").read()[:0x200000]
    fails = static_checks(slus, g, snap)
    checks = len(FUNCS) * 4
    names = [only] if only else CASE_FUNCS
    for name in names:
        # one stream per function, so `--only NAME` (and each mutant's run) sees exactly the cases the
        # full verify gives that function
        rnd = random.Random(f"{seed}:{name}")
        STATS[name] = [0, 0]
        bad = 0
        first = None
        for k in range(cases):
            c = make_case(name, rnd, snap)
            err = run_case(name, c, verbose)
            checks += 1
            if err:
                bad += 1
                first = first or f"case {k}: {err}"
        if verbose or bad:
            print(f"  {name:18s} {cases} cases, wrote {STATS[name][0]:4d}, called {STATS[name][1]:4d}, "
                  f"{bad} failure(s)" + (f"  [{first}]" if first else ""))
        if bad:
            fails.append(f"{name}: {bad} of {cases}")
    return checks, len(fails), fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["verify"])
    ap.add_argument("--cases", type=int, default=300)
    ap.add_argument("--seed", type=int, default=0x5350494E)
    ap.add_argument("--only")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--mutate", action="store_true")
    a = ap.parse_args()
    if a.mutate:
        # each mutant is run on its own function's cases (the same cases `verify` gives it) and must
        # make THAT function's case comparison fail; a static failure does not count
        todo = {k: f for k, f in MUTANTS.items() if not a.only or f == a.only}
        caught = 0
        for m_, owner in todo.items():
            MUT.clear()
            MUT.add(m_)
            _, nf, fl = verify(a.cases, a.seed, owner, False)
            own = [f for f in fl if f.startswith(owner + ": ") and f.endswith(f" of {a.cases}")]
            print(f"  mutant {m_:22s} {'CAUGHT by ' + owner if own else 'MISSED'}  {fl[:1]}")
            caught += 1 if own else 0
        MUT.clear()
        print(f"spine --mutate: {caught} of {len(todo)} mutants caught by their own function, "
              f"{len(todo) - caught} missed")
        return 0 if caught == len(todo) else 1
    n, nf, fl = verify(a.cases, a.seed, a.only, a.verbose)
    for f in fl:
        print("  FAIL", f)
    print(f"spine: {n} checks, {nf} failures")
    return 0 if nf == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
