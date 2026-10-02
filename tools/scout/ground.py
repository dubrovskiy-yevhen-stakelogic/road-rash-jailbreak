"""Scout probe for the GROUND QUERY of Road Rash: Jailbreak (USA, SLUS_01053).

The subject is `RASHCDG 0x800A7BF8` (the ground query the ported bike ground frame asks), the
collision-world lookup under it (`0x800A8498`, the point in cell test), the two geometry leaves
(`0x800B6E08` point in polygon, `0x800B6844` Newell normal), the SLUS leaf `0x8002E14C` (a 16.16
vector normalise), and who sets `entity+0x184` bit 0 - the flag every one of the seven call sites
of the query is gated on.

This probe re-derives those claims independently of the C++ tree:

  * SKELETON: the `jal` word at every named site, the exact caller census of the five functions
    over RASHCDG and SLUS, the `+0x184 & 1` gate in front of all seven query sites, the key
    literals (the 1280 LOD threshold, the 112-byte cell slot stride at 0x800D87E8, the region
    offsets), and a COP2 census (the only GTE use in the whole tree is LZCS/LZCR);
  * CELLS: the region 3 / region 4 structures the query walks, over every cell of both stream
    files: polygon vertex counts, the region-4 record walk, the tag layout, and that every list
    entry indexes a primitive of the group the lookup hands the query;
  * OFFROAD: `+0x184` bits 0..3 recomputed with a transcription of `SLUS 0x8003E67C` from the
    entity's road piece `+0x174` and lateral offset `+0x158`, over every live entity of pools
    0..3 in all four savestates and all fourteen frame dumps;
  * TRACE: the transcription run as a MODEL against the real function executing in our
    interpreter. The rr-race snapshot is copied under work\\ground and ONE instruction of the
    CALLER is changed (`andi s4,v0,0x1` at 0x8007507C -> `li s4,1`) so the ground frame asks the
    query every frame; nothing of the query itself is touched. A second copy additionally
    replaces the query's own LOD literal (`slti v0,t5,1280` -> `slti v0,t5,-32768`) so the
    coarse branch is exercised too - that copy tests the model with the same literal.
    Every call is then recomputed from the snapshot RAM plus every write the watch saw before
    the call, and compared on the return value, the output point, the output normal, the
    surface byte `+0x216` and the exact sequence of point-in-polygon calls (caller, vertex
    count, answer);
  * CAPTURE: the player's own `+0x218` in the frame dumps where he is off the road, re-queried
    at his contact point `+0x1F8` (a consistency check, not an identity).

Reads ONLY work\\disc_us, work\\oracle\\state, work\\oracle\\vr_capture\\ramdumps; writes ONLY under
work\\ground (patched snapshot copies and trace outputs). Never copies game bytes into the repo.

    python tools\\scout\\ground.py info
    python tools\\scout\\ground.py cells
    python tools\\scout\\ground.py offroad
    python tools\\scout\\ground.py trace          # needs <build>\\rrverify.exe
    python tools\\scout\\ground.py verify         # ground: <n> checks, <m> failures
    python tools\\scout\\ground.py verify --mutate
"""

from __future__ import annotations

import argparse
import csv
import glob
import os
import shutil
import struct
import subprocess
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import exe as E  # noqa: E402
import cell as CL  # noqa: E402

ROOT = E.ROOT
DATA = os.path.join(E.DISC, "DATA")
STATE_DIR = os.path.join(ROOT, "work", "oracle", "state")
STATES = ("rr-race", "rr-grid", "rr-pack", "quick")
DUMP_DIR = os.path.join(ROOT, "work", "oracle", "vr_capture", "ramdumps")
WORK = os.path.join(ROOT, "work", "ground")
RRVERIFY = os.path.join(ROOT, "build_m0", "rrverify.exe")
OVL_BASE = 0x8005B5E8
SHA1 = {"SLUS": "67ed165a2c517d4e6106fb0dfa324d66dd9a76f1",
        "G": "cfe43a7786759f2cb9c57751cf99e84d1074782c"}

GROUND_QUERY = 0x800A7BF8
CELL_LOOKUP = 0x800A8498
PIP = 0x800B6E08
NEWELL = 0x800B6844
NORM32 = 0x8002E14C
FIXMUL = 0x8001FC90
CELL_SLOTS = 0x800D87E8          # 24 x 112 bytes, scene_cell.md 1.1
SLOT_STRIDE = 112
GAME_STATE_PTR = 0x8005B2F8
GP = 0x8005AC8C                  # gp+2260 -> the reciprocal-sqrt table
POOL_TABLE = 0x800CE4D0
PLAYER0_BIKE_PTR = 0x8005B3A0

# (image, site, callee)
SKELETON = [
    ("G", 0x8007509C, GROUND_QUERY), ("G", 0x80075348, GROUND_QUERY), ("G", 0x80075510, GROUND_QUERY),
    ("G", 0x8008A898, GROUND_QUERY), ("G", 0x800924C4, GROUND_QUERY), ("G", 0x800A3ACC, GROUND_QUERY),
    ("G", 0x800A43AC, GROUND_QUERY),
    ("G", 0x800A7CDC, CELL_LOOKUP),
    ("G", 0x800A8214, PIP), ("G", 0x800A823C, PIP), ("G", 0x800A879C, PIP),
    ("G", 0x800A827C, NEWELL), ("G", 0x800A82EC, NEWELL),
    ("G", 0x800B6A54, NORM32),
    ("G", 0x800B6ECC, FIXMUL), ("G", 0x800B6EE4, FIXMUL),
    ("G", 0x800B689C, FIXMUL), ("G", 0x800B68C8, FIXMUL), ("G", 0x800B68F4, FIXMUL),
    ("G", 0x800B693C, FIXMUL), ("G", 0x800B6968, FIXMUL), ("G", 0x800B6994, FIXMUL),
    ("SLUS", 0x8003DEF4, 0x8003EF34), ("SLUS", 0x8003F1A0, 0x8003E67C),
]
# the exact set of call sites of each callee over RASHCDG + SLUS
CENSUS = {
    GROUND_QUERY: [("G", a) for a in (0x8007509C, 0x80075348, 0x80075510, 0x8008A898, 0x800924C4,
                                      0x800A3ACC, 0x800A43AC)],
    CELL_LOOKUP: [("G", 0x800A7CDC)],
    PIP: [("G", 0x800A8214), ("G", 0x800A823C), ("G", 0x800A879C)],
    NEWELL: [("G", 0x800A827C), ("G", 0x800A82EC)],
    NORM32: [("G", 0x8008032C), ("G", 0x8009668C), ("G", 0x800A1DB8), ("G", 0x800B6A54)],
    0x8003E67C: [("SLUS", 0x8003F1A0)],
    0x8003EF34: [("SLUS", 0x8003DEF4)],
}
# (image, address, instruction word): literals and shapes the transcription rests on
WORDS = [
    ("G", 0x800A7CA4, 0x29A20500, "slti v0,t5,1280 - the LOD (fine) threshold on entity+0x2C"),
    ("G", 0x800A7C50, 0x8DAD002C, "lw t5,44(t5) - entity+0x2C, the view depth"),
    ("G", 0x800A7C7C, 0x8DC30030, "lw v1,48(t6) - entity+0x30, player 2's view depth"),
    ("G", 0x800A7D44, 0x248487E8, "addiu a0,a0,-30744 - the cell slot table 0x800D87E8"),
    ("G", 0x800A7D7C, 0x8C420030, "lw v0,48(v0) - body+0x30, REGION 4"),
    ("G", 0x800A7D88, 0x2C820200, "sltiu v0,a0,512 - the cached record offset must be < 512"),
    ("G", 0x800A8218, 0x24070001, "li a3,1 - point in polygon about axis 1 (Y), i.e. on XZ"),
    ("G", 0x800A8370, 0xA1A20216, "sb v0,534(t5) - entity+0x216, the surface nibble"),
    ("G", 0x800A8564, 0x8C420034, "lw v0,52(v0) - body+0x34, REGION 5 (vertices)"),
    ("G", 0x800A8580, 0x8C77002C, "lw s7,44(v1) - body+0x2C, REGION 3 (sub-area polygons)"),
    ("G", 0x800A8A74, 0x8C85003C, "lw a1,60(a0) - body+0x3C, REGION 7 (fine primitives)"),
    ("G", 0x800A8ADC, 0x8C420038, "lw v0,56(v0) - body+0x38, REGION 6 (coarse primitives)"),
    ("G", 0x800A8AA0, 0x8C830028, "lw v1,40(a0) - body+0x28, REGION 2 (group table)"),
    ("G", 0x800B699C, 0x3C08005A, "lui t0,0x5a - the Newell sum clamp 0x005A8000"),
    ("G", 0x8007507C, 0x30540001, "andi s4,v0,0x1 - the ground frame's +0x184 bit-0 gate"),
    ("SLUS", 0x8003F1B4, 0x2404FFF0, "li a0,-16 - +0x184 bits 0..3 replaced"),
    ("SLUS", 0x8003F1CC, 0x006C1825, "or v1,v1,t4 - ... by 0x8003E67C's answer"),
    ("SLUS", 0x8003F1D0, 0xAE430010, "sw v1,16(s2) - s2 = entity+0x174, so this is +0x184"),
    ("SLUS", 0x8003DEF8, 0x26060174, "addiu a2,s0,372 - s2 = entity+0x174"),
    ("SLUS", 0x8003E720, 0x354A0001, "ori t2,t2,0x1 - 'beyond the outer edge' is the only 1"),
]
# the gate in front of each query call site: `andi reg,reg,0x1` within 16 words before the jal,
# on a value loaded from +388
GATED = [0x8007509C, 0x80075348, 0x80075510, 0x8008A898, 0x800924C4, 0x800A3ACC, 0x800A43AC]
# functions whose COP2 use is censused: (image, start, end)
RANGES = [("G", GROUND_QUERY, CELL_LOOKUP), ("G", CELL_LOOKUP, 0x800A8BE0), ("G", NEWELL, 0x800B6AAC),
          ("G", PIP, 0x800B6F40), ("SLUS", NORM32, 0x8002E388), ("SLUS", FIXMUL, 0x8001FCB0)]


def s32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


def u32(v):
    return v & 0xFFFFFFFF


def fm(a, b):
    """SLUS 0x8001FC90 FixMul: bits 16..47 of the signed 64-bit product."""
    return s32((s32(a) * s32(b)) >> 16)


def iabs(v):
    """(v + (v >> 31)) ^ (v >> 31) - so abs(INT_MIN) stays INT_MIN."""
    v = s32(v)
    m = -1 if v < 0 else 0
    return s32((v + m) ^ m)


def lzcr(v):
    v = u32(v)
    p = (~v & 0xFFFFFFFF) if v & 0x80000000 else v
    n = 0
    while n < 32 and not (p & 0x80000000):
        n += 1
        p = (p << 1) & 0xFFFFFFFF
    return n


def half_trunc(v):
    v = s32(v)
    return s32((v + (1 if v < 0 else 0)) >> 1)


class Mem:
    def __init__(self, ram: bytes):
        self.b = bytearray(ram)

    def o(self, a):
        return a & 0x1FFFFF

    def w(self, a):
        return struct.unpack_from("<I", self.b, self.o(a))[0]

    def sw_(self, a):
        return s32(self.w(a))

    def h(self, a):
        return struct.unpack_from("<h", self.b, self.o(a))[0]

    def hu(self, a):
        return struct.unpack_from("<H", self.b, self.o(a))[0]

    def bu(self, a):
        return self.b[self.o(a)]

    def put(self, a, size, v):
        o = self.o(a)
        if size == 4:
            struct.pack_into("<I", self.b, o, u32(v))
        elif size == 2:
            struct.pack_into("<H", self.b, o, v & 0xFFFF)
        else:
            self.b[o] = v & 0xFF


# ---------------------------------------------------------------------------
# The MODEL: a transcription of the five functions, written from our own disassembly.
# `cfg` carries every literal a --mutate run perturbs.
# ---------------------------------------------------------------------------

def pip(p, poly, n, axis, calls, site):
    """RASHCDG 0x800B6E08(point, poly, n, axis) - strictly inside a convex polygon."""
    i = axis + 1
    i = i if i < 3 else 0
    j = i + 1
    j = j if j < 3 else 0
    prev = n - 1
    r = 1
    for k in range(n):
        c, q = poly[k], poly[prev]
        a = fm(s32(c[j] - q[j]), s32(p[i] - q[i]))
        b = fm(s32(c[i] - q[i]), s32(p[j] - q[j]))
        if s32(a - b) >= 0:
            r = 0
            break
        prev = k
    calls.append((site, n, r))
    return r


def norm32(v, mem):
    """SLUS 0x8002E14C: normalise a 16.16 vector in place; LZCS/LZCR and the table at *(gp+2260)."""
    x, y, z = (s32(c) for c in v)
    ay, ax = iabs(y), iabs(x)
    m = ax if ay < ax else ay
    az = iabs(z)
    if az < m:
        az = m
    m = az
    sh = 0 if m == 0 else 31 - lzcr(m)
    if sh >= 21:
        k = (sh - 20) & 31
        x, y, z = x >> k, y >> k, z >> k
    else:
        k = (20 - sh) & 31
        x, y, z = s32(x << k), s32(y << k), s32(z << k)
    t4 = s32(fm(z, z) + s32(fm(y, y) + fm(x, x)))
    if t4 <= 0:
        return v
    t2 = 22 - (lzcr(t4) & ~1)
    if t2 <= 0:
        t2 = 0
    idx = t4 >> t2
    half = t2 >> 1
    ent = mem.hu(u32(mem.w(GP + 2260) + 2 * idx))
    f = s32((ent >> 5) << (ent & 31)) >> (half & 31)
    return [fm(x, f), fm(y, f), fm(z, f)]


def newell(poly, n, mem, cfg):
    """RASHCDG 0x800B6844(poly, n, out s16[3]) - Newell normal, clamped, normalised, >> 4."""
    s = [0, 0, 0]
    k = 0
    while k < n - 1:
        a, b = poly[k], poly[k + 1]
        s[0] = s32(s[0] + fm(a[1] - b[1], a[2] + b[2]))
        s[1] = s32(s[1] + fm(a[2] - b[2], a[0] + b[0]))
        s[2] = s32(s[2] + fm(a[0] - b[0], a[1] + b[1]))
        k += 1
    a, b = poly[k], poly[0]
    s[0] = s32(s[0] + fm(a[1] - b[1], a[2] + b[2]))
    s[1] = s32(s[1] + fm(a[2] - b[2], a[0] + b[0]))
    s[2] = s32(s[2] + fm(a[0] - b[0], a[1] + b[1]))
    lim = cfg["clamp"]
    while iabs(s[0]) > lim or iabs(s[1]) > lim or iabs(s[2]) > lim:
        s = [c >> 1 for c in s]
    s = norm32(s, mem)
    return [(c >> 4) & 0xFFFF for c in s]


def cell_lookup(mem, point, heading, st, cfg, calls):
    """RASHCDG 0x800A8498(point, heading, &prim, &verts, &origin, &state) -> tri | quad << 16.

    Returns (value, prim, verts, origin); `st` is a one-element list holding the state word."""
    prim = verts = origin = None
    state = u32(st[0])
    lo, hi = (12, 24) if state & 0x80000000 else (0, 12)
    s2 = state & 0x3F
    for _ in range(lo, hi):
        s2 = s2 if (lo <= s2 < hi) else lo
        slot = CELL_SLOTS + SLOT_STRIDE * s2
        if mem.sw_(slot) != -1:
            body = mem.w(slot + 4)
            verts = u32(mem.w(body + 52) + 4)
            r3 = mem.w(body + 44)
            origin = [s32(mem.w(body + 8) << 10), s32(mem.w(body + 12) << 10), s32(mem.w(body + 16) << 10)]
            local = [s32(point[c] - origin[c]) for c in range(3)]
            s1 = (s32(u32(st[0])) >> 6) & 7
            B = mem.hu(body + 6)
            s4 = 0
            while B != 0:
                s1 = s1 if s1 < B else 0
                a0 = r3 + 2 * s1
                start = mem.h(a0 + 2)
                cnt = mem.h(a0 + 4) - start
                assert cnt <= 6, "region-3 polygon would overflow the 72-byte frame buffer"
                poly = [None] * max(cnt, 0)
                mnx, mxx, mnz, mxz = 0x3FFF0000, -0x3FFF0000, 0x3FFF0000, -0x3FFF0000
                for a3 in range(max(cnt, 0)):
                    vi = mem.h(r3 + 2 * (start + a3))
                    va = u32(verts + 8 * vi)
                    v = [s32(mem.h(va) << 10), s32(mem.h(va + 2) << 10), s32(mem.h(va + 4) << 10)]
                    poly[cnt - 1 - a3] = v
                    mnx, mxx = min(mnx, v[0]), max(mxx, v[0])
                    mnz, mxz = min(mnz, v[2]), max(mxz, v[2])
                inside = False
                if not (local[0] < mnx or mxx < local[0] or local[2] < mnz or mxz < local[2]):
                    inside = pip(local, poly, cnt, 1, calls, 0x800A87A4) != 0
                if inside:
                    state = u32((u32(st[0]) & 0xFFFFF800) | s2 | (s1 << 6))
                    assert cnt % 2 == 0, "odd polygon: the edge loop would read a stale frame slot"
                    best, t3 = 0x3FFF0000, 0
                    for a3 in range(0, cnt, 2):
                        p, q = poly[a3], poly[a3 + 1]
                        mx, mz = half_trunc(p[0] + q[0]), half_trunc(p[2] + q[2])
                        d0, d1 = iabs(local[0] - mx), iabs(local[2] - mz)
                        if d0 < d1:
                            d0, d1 = d1, d0
                        a2 = d1 + (d1 >> 1)
                        d = s32(d0 - (d0 >> 5) - (d0 >> 7) + (a2 >> 2) + (a2 >> 6))
                        if d < best:
                            t3 = half_trunc(cnt - a3 - 1)
                            best = d
                    state = u32(state | (t3 << 9))
                    pa, pb = poly[cnt - 2 - 2 * t3], poly[cnt - 1 - 2 * t3]
                    dv = []
                    for c in range(3):
                        x = s32(pa[c] - pb[c])
                        dv.append((x + 3) >> 2 if x < 0 else x >> 2)
                    hx, hy, hz = (mem.h(heading + 2 * c) << 4 for c in range(3))
                    dot = s32(fm(dv[2], hz) + s32(fm(dv[1], hy) + fm(dv[0], hx)))
                    cross = s32(fm(dv[2], hx) - fm(dv[0], hz))
                    if cross < 0:
                        state |= 0x2000
                    if dot < 0:
                        state |= 0x1000
                    A = mem.hu(body + 4)
                    r7 = mem.w(body + 60)
                    if (state >> 11) & 1 and r7 != 0:
                        g = A + B + s1
                        prim = u32(r7 + mem.w(mem.w(body + 40) + 12 * g))
                    else:
                        r6 = mem.w(body + 56)
                        if r6 == 0:
                            st[0] = 0
                            return 0, prim, verts, origin
                        state &= ~0x800
                        g = A + s1
                        prim = u32(r6 + mem.w(mem.w(body + 40) + 12 * g))
                    st[0] = state
                    grp = mem.w(body + 40) + 12 * g
                    return u32(mem.w(grp + 4) | (mem.w(grp + 8) << 16)), prim, verts, origin
                s4 += 1
                if s4 >= B:
                    break
                s1 += 1
        s2 += 1
    st[0] = 0
    return 0, prim, verts, origin


def ground_query(mem, e, ref_ptr, hint, cfg):
    """RASHCDG 0x800A7BF8(entity, refPoint|0, outPoint, outNormal s16[3], hint).

    Returns dict(ret, point, normal, surface, calls); point/normal/surface are None when the
    original leaves the buffer (or +0x216) untouched."""
    out = {"point": None, "normal": None, "surface": None, "calls": []}
    calls = out["calls"]
    hint = u32(hint)
    s0 = ref_ptr if ref_ptr else u32(e + 184)
    ref = [mem.sw_(s0), mem.sw_(s0 + 4), mem.sw_(s0 + 8)]
    gs = mem.w(GAME_STATE_PTR)
    dist = mem.sw_(e + 44)
    state = hint & 0x7FF
    if not (mem.w(gs + 48) < 2):
        v1 = mem.sw_(e + 48)
        if v1 < dist:
            state |= 0x80000000
            dist = v1
    if dist < cfg["lod"]:
        state |= 0x800
    st = [state]
    counts, prim, verts, origin = cell_lookup(mem, ref, u32(e + 450), st, cfg, calls)
    a1 = u32(st[0])
    t0 = a1 & 0x7FFFFFFF
    if counts == 0:
        out["ret"] = 0xFFFFFFFF
        return out
    local = [s32(ref[c] - origin[c]) for c in range(3)]
    same = ((a1 ^ hint) & 0xFFF) == 0
    body = mem.w(CELL_SLOTS + SLOT_STRIDE * (a1 & 0x3F) + 4)
    r4 = mem.w(body + 48)
    off = s32((u32(-1 if same else 0) & (((hint >> 14) & 0x3FF) + 1)) - 1)
    fine = (t0 >> 11) & 1
    if u32(off) >= 512:
        nrec = mem.bu(r4 + cfg["count_byte"](fine))
        off = 2
        if nrec == 0:
            out["ret"] = 0xFFFFFFFF
            return out
        k, found = 0, False
        while True:
            tag = mem.bu(r4 + off)
            if (tag >> 7) == fine and ((tag & 0x70) >> 4) == ((t0 >> 6) & 7) and \
               ((tag & 0xC) >> 2) == ((t0 >> 9) & 3):
                found = True
                break
            k += 1
            off = off + 2 + mem.bu(r4 + off + 1)
            if not (k < nrec):
                break
        if not found:
            out["ret"] = 0xFFFFFFFF
            return out
    rec = u32(r4 + off)
    if mem.bu(rec + 1) == 0:
        out["ret"] = u32((off << 14) | 0xFF000000 | (t0 & 0xFFF))
        return out
    s5 = (s32(hint) >> 24) & 0x7F
    L = mem.bu(rec + 1)
    b = [0] * 6
    b[0] = 2
    b[2] = (L + 4) & 0xFF
    b[1] = (L + 1) & 0xFF
    v0 = b[2] + mem.bu(rec + b[2] - 1)
    b[3] = (v0 - 1) & 0xFF
    b[4] = (v0 + 2) & 0xFF
    b[5] = (b[4] + (mem.bu(rec + b[4] - 1) & (-fine & 0xFF)) - 1) & 0xFF
    s8 = 1 - ((t0 >> 11) & 2)
    s136 = 1 - ((t0 >> 12) & 2)
    s3 = (s32(hint) >> 12) & 3
    ok = 0
    if s3 < 3:
        ok = 0 if b[2 * s3 + 1] < b[2 * s3] else 1
    s3 = s3 if ok else 0
    if (not same) or s5 == 127 or s5 < 2:
        s5 = b[2 * s3 + (1 if s8 < 0 else 0)]
    stop = b[2 * s3 + (1 if s8 >= 0 else 0)]
    s4 = 0
    if s136 == 1:
        s4 = fine + 1
        while s4 != 0 and b[2 * s4 + 1] < b[2 * s4]:
            s4 -= 1
    maxit = (b[1] - b[0]) + (b[3] - b[2]) + ((b[5] - b[4] + 1) if fine else 0) + 2
    it = 0
    tri = counts & 0xFFFF
    while True:
        idx = mem.bu(rec + s5)
        isq = 0 if idx < tri else 1
        if isq:
            idx -= tri
            ip = u32(prim + 14 + 20 * tri + 24 * idx + 2)
        else:
            ip = u32(prim + 14 + 20 * idx)
        nv = 3 + isq
        poly = []
        for k in range(nv):
            va = u32(verts + 8 * mem.hu(ip + 2 * k))
            poly.append([s32(mem.h(va) << 10), s32(mem.h(va + 2) << 10), s32(mem.h(va + 4) << 10)])
        xs, zs = [p[0] for p in poly], [p[2] for p in poly]
        mnx, mxx = min([0x3FFF0000] + xs), max([-0x3FFF0000] + xs)
        mnz, mxz = min([0x3FFF0000] + zs), max([-0x3FFF0000] + zs)
        hit = False
        if not (local[0] < mnx or mxx < local[0] or local[2] < mnz or mxz < local[2]):
            hit = pip(local, poly, nv, 1, calls, 0x800A821C) != 0
        if hit:
            if nv == 4:
                if cfg["quad_split"] == "orig":
                    t = poly[1:4] if pip(local, poly[1:4], 3, 1, calls, 0x800A8244) else \
                        [poly[0], poly[1], poly[3]]
                else:  # mutated: the other diagonal
                    t = poly[0:3] if pip(local, poly[0:3], 3, 1, calls, 0x800A8244) else \
                        [poly[0], poly[2], poly[3]]
                pt = poly[1]
            else:
                t = poly
                pt = poly[0]
            n = newell(t, 3, mem, cfg)
            out["normal"] = [(-c) & 0xFFFF if cfg["negate"] else c for c in n]
            out["point"] = [u32(pt[c] + origin[c]) for c in range(3)]
            out["surface"] = mem.hu(u32(ip - (14 if nv >= 4 else 12))) >> 12
            a = 2
        elif s5 != stop:
            s5 += s8
            a = 0
        else:
            a = 0
            if s3 == s4:
                a = 1
            else:
                s3 += s136
                j = 2 * s3 + (1 if s8 < 0 else 0)
                s5 = b[j] if 0 <= j < 6 else None
                stop = b[j + s8] if 0 <= j + s8 < 6 else None
                assert s5 is not None and stop is not None, "list cursor left the 6-byte table"
        it += 1
        if maxit < it or a != 0:
            break
    if a == 2:
        out["ret"] = u32((s5 << 24) | (off << 14) | (s3 << 12) | (t0 & 0xFFF))
    else:
        v = u32(((off << 14) | (t0 & 0xFFF) | 0xFF000000) + 1)
        out["ret"] = u32((u32(-a) & v) - 1)
    return out


def e67c(mem, piece, lat, cfg):
    """SLUS 0x8003E67C(piece, lateral, side, out s16[2]) -> 1 iff beyond the outer edge."""
    a1 = iabs(lat)
    side = 1 if lat < 0 else 2
    if cfg["side_swap"]:
        side = 3 - side
    sec = piece + (136 if side == 2 else 8)
    t1 = mem.sw_(sec + 8)
    if mem.h(sec + 6) > 0 and a1 < iabs(mem.sw_(sec + 72)):
        return 0, 5, mem.hu(sec + 82)
    t0 = mem.sw_(sec + 24) if mem.h(sec + 2) > 0 else t1
    if a1 < iabs(t0):
        return 0, 4, 1
    if a1 < iabs(t1):
        return 0, 1, mem.hu(sec + 18)
    return 1, 0, 0


def config(mutate):
    c = {"lod": 1280, "count_byte": (lambda fine: fine), "quad_split": "orig", "negate": True,
         "clamp": 0x005A8000, "side_swap": False, "poly_counts": (4, 6)}
    if mutate:
        c.update({"count_byte": (lambda fine: 0), "quad_split": "mutated", "negate": False,
                  "side_swap": True, "poly_counts": (4,)})
    return c


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


def images():
    slus = E.load_exe()
    g = E.load_overlay("RASHCDG.BIN", OVL_BASE)
    return {"SLUS": slus, "G": g}


def jal_target(img, site):
    w = img.word(site)
    if (w >> 26) != 3:
        return None
    return ((site + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)


def check_skeleton(bench, verbose=True):
    im = images()
    bench.check(im["SLUS"].sha1 == SHA1["SLUS"], "SLUS sha1")
    bench.check(im["G"].sha1 == SHA1["G"], "RASHCDG sha1")
    for img, site, callee in SKELETON:
        bench.check(jal_target(im[img], site) == callee, f"jal at {site:#x} -> {callee:#x}")
    calls = {k: E.scan_calls(v) for k, v in im.items()}
    for callee, want in CENSUS.items():
        got = sorted((k, a) for k in ("G", "SLUS") for a in calls[k].get(callee, []))
        bench.check(got == sorted(want), f"caller census of {callee:#x}: {[hex(a) for _, a in got]}")
        if verbose:
            print(f"  {callee:#010x}  callers {', '.join(f'{k}:{a:#x}' for k, a in got)}")
    for img, addr, word, what in WORDS:
        bench.check(im[img].word(addr) == word, f"word at {addr:#x}: {what}")
    g = im["G"]
    for site in GATED:
        if site == 0x80075348:
            # the ground frame's second query reuses s4 = +0x184 & 1 from 0x8007507C:
            # `beqz s4,0x80075418` at 0x80075328
            ok = g.word(0x80075328) == 0x12800000 | ((0x80075418 - 0x8007532C) >> 2)
            bench.check(ok, "call site 0x80075348 is gated on s4 = +0x184 & 1 (beqz at 0x80075328)")
            continue
        ws = [g.word(site - 4 * k) for k in range(1, 17)]
        has_load = any((w >> 26) == 0x23 and (w & 0xFFFF) == 388 for w in ws)
        has_andi = any((w >> 26) == 0x0C and (w & 0xFFFF) == 1 for w in ws)
        bench.check(has_load and has_andi, f"call site {site:#x} is gated on +0x184 & 1")
    for img, lo, hi in RANGES:
        cop2 = Counter()
        jalr = 0
        for a in range(lo, hi, 4):
            w = im[img].word(a)
            op = w >> 26
            if op == 0x12:
                rs = (w >> 21) & 0x1F
                kind = {0: "mfc2", 2: "cfc2", 4: "mtc2", 6: "ctc2"}.get(rs, "cmd")
                cop2[f"{kind} ${(w >> 11) & 0x1F}"] += 1
            elif op in (0x32, 0x3A):
                cop2[f"{'lwc2' if op == 0x32 else 'swc2'} ${(w >> 16) & 0x1F}"] += 1
            if op == 0 and (w & 0x3F) == 9:
                jalr += 1
        if verbose:
            print(f"  {lo:#010x}..{hi:#010x}  COP2 {dict(cop2) or 'none'}  jalr {jalr}")
        # the only GTE use anywhere under the query: LZCS in, LZCR out, no command word
        want = {"mtc2 $30": 2, "swc2 $31": 1, "mfc2 $31": 1} if lo == NORM32 else {}
        bench.check(dict(cop2) == want and jalr == 0, f"COP2/jalr census {lo:#x}: {dict(cop2)}")


def check_cells(bench, cfg, verbose=True):
    stats = Counter()
    for fn in ("STREAM1.STR", "STREAM2.STR"):
        cells = CL.load_cells(os.path.join(DATA, fn))
        for (_t, rid), (_i, c) in cells.items():
            b = c.raw
            A, B = c.na, c.nb
            r3 = c.R[3]
            hdr = [struct.unpack_from("<h", b, r3 + 2 * k)[0] for k in range(B + 2)]
            ok = hdr[0] == B + 2 and all(hdr[k] < hdr[k + 1] for k in range(B + 1))
            counts = [hdr[s + 2] - hdr[s + 1] for s in range(B)]
            ok &= all(n in cfg["poly_counts"] for n in counts)
            stats["cells"] += 1
            stats["region3 ok"] += ok
            for n in counts:
                stats[f"poly{n}"] += 1
            # region 4
            r4 = c.R[4]
            end = r4 + c.rsize[4]
            ncoarse, nall = b[r4], b[r4 + 1]
            off, recs = 2, []
            good = True
            for k in range(nall):
                if r4 + off + 2 > end:
                    good = False
                    break
                tag, ln = b[r4 + off], b[r4 + off + 1]
                recs.append((off, tag, ln))
                off += 2 + ln
            good &= r4 + off <= end
            good &= all((t >> 7) == (0 if k < ncoarse else 1) for k, (_, t, _l) in enumerate(recs))
            good &= all(o < 512 for o, _, _ in recs)
            # groups: coarse A+s, fine A+B+s; every entry of every list indexes that group
            groups = c.groups

            def gsize(fine, s):
                g = (A + B + s) if fine else (A + s)
                return groups[g][1] + groups[g][2]
            # the search takes the first record with a given (bit7, sub-area, edge): it must be the
            # list-0 record (low bits 1), followed by list 1 (low 2 fine / 3 coarse) and, if fine,
            # list 2 (low 3)
            seen = {}
            for k, (o, t, ln) in enumerate(recs):
                key = (t >> 2)
                if key in seen:
                    continue
                seen[key] = k
                fine, sub = t >> 7, (t >> 4) & 7
                pat = [1, 2, 3] if fine else [1, 3]
                trio = recs[k:k + len(pat)]
                good &= len(trio) == len(pat) and \
                    all((tt & 3) == p and (tt >> 2) == key for (_, tt, _), p in zip(trio, pat))
                good &= sub < B
                for (oo, _tt, ll) in trio:
                    good &= all(b[r4 + oo + 2 + q] < gsize(fine, sub) for q in range(ll))
                    stats["list entries"] += ll
                    stats["empty lists"] += (ll == 0)
                # the query keeps its list bounds as u8 offsets from the list-0 record: the whole
                # trio must span < 256 bytes or `sb` would wrap them
                span = sum(2 + ll for (_, _, ll) in trio)
                stats["max trio span"] = max(stats["max trio span"], span)
                # the one shape that would send the walk out of its lists (0x800A83DC keeps
                # stepping s5 past an empty middle list): fine, list 1 empty, list 2 not
                if fine and len(trio) == 3 and trio[1][2] == 0 and trio[2][2] > 0:
                    stats["runaway shape"] += 1
                if trio and trio[0][2] == 0:
                    stats["list 0 empty (0xFF000000 return)"] += 1
            stats["region4 ok"] += good
            stats["records"] += len(recs)
            stats["keys"] += len(seen)
    if verbose:
        print("  " + ", ".join(f"{k} {v}" for k, v in sorted(stats.items())))
    bench.check(stats["region3 ok"] == stats["cells"], f"region 3 layout on {stats['cells']} cells")
    bench.check(stats["region4 ok"] == stats["cells"], f"region 4 layout on {stats['cells']} cells")
    bench.check(stats["max trio span"] < 256, f"every record trio spans < 256 bytes ({stats['max trio span']})")
    bench.check(stats["runaway shape"] == 0, "no fine record with an empty list 1 and a non-empty list 2")
    return stats


def ram_files():
    out = [(s, os.path.join(STATE_DIR, s, "ram.bin")) for s in STATES]
    out += [(os.path.basename(f)[:-4], f) for f in sorted(glob.glob(os.path.join(DUMP_DIR, "ram_*.bin")))]
    return [(n, f) for n, f in out if os.path.exists(f)]


def check_offroad(bench, cfg, verbose=True):
    tot = Counter()
    player_off = []
    for name, f in ram_files():
        mem = Mem(open(f, "rb").read())
        for pool, n in ((0, 18), (1, 18), (2, 4), (3, 16)):
            base, stride = mem.w(POOL_TABLE + 16 * pool), mem.sw_(POOL_TABLE + 16 * pool + 4)
            for s in range(n):
                e = u32(base + stride * s)
                if not (mem.hu(e + 0x140) & 1):
                    continue
                piece = mem.w(e + 0x174)
                f184 = mem.w(e + 0x184)
                if piece == 0:
                    tot["no piece"] += 1
                    tot["no piece, nibble 0"] += (f184 & 0xF) == 0
                    continue
                b0, c0, c1 = e67c(mem, piece, mem.sw_(e + 0x158), cfg)
                tot["samples"] += 1
                tot[f"bit0={b0}"] += 1
                tot["nibble ok"] += (f184 & 0xF) == b0
                cls_ok = c0 == mem.h(e + 0x188) and c1 == mem.hu(e + 0x18A)
                tot["class ok"] += cls_ok
                if not cls_ok:
                    tot[f"class miss pool {pool} slot {s}"] += 1
                if pool == 0 and s == 0 and b0:
                    player_off.append((name, mem.w(e + 0x218)))
    if verbose:
        print("  " + ", ".join(f"{k} {v}" for k, v in sorted(tot.items())))
        print("  player off the road in: " + ", ".join(f"{n} (+0x218 {v:#010x})" for n, v in player_off))
    bench.check(tot["nibble ok"] == tot["samples"] and tot["samples"] > 0,
                f"+0x184 bits 0..3 == 0x8003E67C on {tot['nibble ok']}/{tot['samples']} live samples")
    bench.check(tot["no piece, nibble 0"] == tot["no piece"], "no road piece -> nibble 0")
    bench.check(tot["bit0=1"] >= 20, f"bit 0 observed set on {tot['bit0=1']} live samples")
    bench.check(len(player_off) >= 10, f"player off the road in {len(player_off)} captures")
    return tot


# ---------------------------------------------------------------------------
# the live oracle
# ---------------------------------------------------------------------------

PATCH_GATE = (0x8007507C, 0x30540001, 0x34140001)  # andi s4,v0,0x1 -> li s4,1 (the CALLER)
PATCH_LOD = (0x800A7CA4, 0x29A20500, 0x29A28000)   # slti v0,t5,1280 -> slti v0,t5,-32768
PLAYER = 0x801B65D4
RIDER0 = 0x801BB2EC          # pool 1 slot 0, the player's own rider (bike+0x354)
RIDER6 = 0x801BC1A4          # pool 1 slot 6, the downed rider whose +0x184 bit 0 is set
OUTPT = 0x801FFE58           # the ground frame's sp+24 in these frames (checked per call)
CAMERA = 0x800CD898          # the per-player view object 0x800A421C queries for (its +0xB8 = 0x800CD950)
WATCHES = [(PLAYER, 1096, "bike0"), (RIDER0, 628, "rider0"), (RIDER6, 628, "rider6"), (CAMERA, 1132, "cam"),
           (OUTPT, 24, "outpt"), (CELL_SLOTS, 24 * SLOT_STRIDE, "slots")]
STATEFUL = ("bike0", "rider0", "rider6", "cam", "slots")
PT_STORES = (0x800A82B8, 0x800A82CC, 0x800A82E4, 0x800A8328, 0x800A833C, 0x800A8350)
SURF_STORES = (0x800A8370, 0x800A8380)


def shift(dx, dz=0):
    """DATA edits (not code): move the player bike by (dx, dz) (16.16) in every field that holds
    its position - the world position +0x1D4, the box centre +0xB8, the eight box corners from
    +0xC4 and the contact point +0x1F8. Returned as (address, delta) pairs."""
    fields = [0x1D4, 0x0B8, 0x1F8] + [0x0C4 + 12 * k for k in range(8)]
    return [(PLAYER + f, dx) for f in fields] + [(PLAYER + f + 8, dz) for f in fields if dz]


RUNS = [
    # name, code patches, data shifts, the LOD literal the copy carries, frames, pad
    # 1. the player on the road, the caller's gate forced: the query runs, finds its record and
    #    exhausts the lists (the road itself is not in them)
    ("q-idle", [PATCH_GATE], [], 1280, 100, "0xFFFF"),
    ("q-throttle", [PATCH_GATE], [], 1280, 40, "0xBFFF"),
    # 2. NO code patch: the player moved off the road; the game sets +0x184 bit 0 by itself
    ("off-left", [], shift(-0x00100000), 1280, 100, "0xFFFF"),
    ("off-left-throttle", [], shift(-0x00100000), 1280, 40, "0xBFFF"),
    ("off-left-far", [], shift(-0x00240000, -0x00080000), 1280, 100, "0xFFFF"),
    ("off-map", [], shift(-0x01F40000), 1280, 20, "0xFFFF"),
    # 3. the same off-road start with the query's own LOD literal replaced, for the coarse branch
    ("off-left-coarse", [PATCH_LOD], shift(-0x00100000), -32768, 100, "0xFFFF"),
]
PROBES = [(0x800A7C0C, "gq_in"), (0x800A8490, "gq_ret"), (PIP, "pip_in"), (0x800B6F38, "pip_ret")]


def prepare(name, patches, shifts=()):
    src = os.path.join(STATE_DIR, "rr-race")
    dst = os.path.join(WORK, "state-" + name)
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    shutil.copytree(src, dst)
    p = os.path.join(dst, "ram.bin")
    ram = bytearray(open(p, "rb").read())
    for addr, old, new in patches:
        o = addr & 0x1FFFFF
        assert struct.unpack_from("<I", ram, o)[0] == old, f"unexpected word at {addr:#x}"
        struct.pack_into("<I", ram, o, new)
    for addr, delta in shifts:
        o = addr & 0x1FFFFF
        struct.pack_into("<i", ram, o, s32(struct.unpack_from("<i", ram, o)[0] + delta))
    open(p, "wb").write(ram)
    return dst, bytes(ram)


def run_trace(name, patches, shifts, frames, pad):
    state, ram = prepare(name, patches, shifts)
    out = os.path.join(WORK, "tr-" + name)
    cmd = [RRVERIFY, "trace", "--state", state, "--frames", str(frames), "--no-cop2", "--no-gpu",
           "--pad", pad, "--out", out]
    for a, n, w in WATCHES:
        cmd += ["--watch", f"{a:#x}:{n}:{w}"]
    for a, n in PROBES:
        cmd += ["--probe", f"{a:#x}:{n}"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    stop = [ln.split(None, 1)[1].strip() for ln in r.stdout.splitlines()
            if ln.startswith(("stopped", "buffer swaps"))]
    return out, stop, ram


def watched(addr, n=1):
    return any(a <= addr and addr + n <= a + ln for a, ln, w in WATCHES if w != "slots")


def replay(name, lod, out, ram, cfg, bench, verbose):
    mem = Mem(ram)
    events = []
    with open(os.path.join(out, "watch.csv")) as f:
        for row in csv.DictReader(f):
            events.append((int(row["seq"]), int(row["pc"], 16), row["kind"], row["watch"],
                           int(row["address"], 16), int(row["size"]), int(row["value"], 16)))
    probes = []
    with open(os.path.join(out, "probes.csv")) as f:
        for row in csv.DictReader(f):
            probes.append(row)
    probes.sort(key=lambda r: int(r["seq"]))
    events.sort()
    # the query reads slot +0x00 (in use) and +0x04 (the body); count writes to exactly those
    slot_key_writes = [ev for ev in events if ev[3] == "slots" and ev[2] == "write"
                       and (ev[4] - CELL_SLOTS) % SLOT_STRIDE < 8]
    calls, cur = [], None
    for p in probes:
        nm = p["name"]
        if nm == "gq_in":
            cur = {"in": p, "pip": [], "pending": None}
        elif cur is None:
            continue
        elif nm == "pip_in":
            cur["pending"] = (int(p["ra"], 16), int(p["a2"], 16))
        elif nm == "pip_ret" and cur["pending"] is not None:
            ra, n = cur["pending"]
            cur["pip"].append((ra, n, int(p["v0"], 16)))
            cur["pending"] = None
        elif nm == "gq_ret":
            cur["ret"] = p
            calls.append(cur)
            cur = None
    stats = Counter()
    ei = 0
    cfg_run = dict(cfg)
    cfg_run["lod"] = lod   # the literal the copy actually carries
    for c in calls:
        seq0, seq1 = int(c["in"]["seq"]), int(c["ret"]["seq"])
        while ei < len(events) and events[ei][0] < seq0:
            sq, pc, kind, w, addr, size, val = events[ei]
            if kind == "write" and w in STATEFUL:
                mem.put(addr, size, val)
            ei += 1
        e = int(c["in"]["a0"], 16)
        ref = int(c["in"]["s0"], 16)
        a2 = int(c["in"]["a2"], 16)
        a3 = int(c["in"]["a3"], 16)
        hint = int(c["in"]["s2"], 16)
        site = (int(c["in"]["ra"], 16) - 8) & 0xFFFFFFFF
        stats[f"site {site:#x}"] += 1
        if not watched(e, 0x220):
            stats["entity not watched"] += 1
            continue
        m = ground_query(mem, e, ref, hint, cfg_run)
        win = [ev for ev in events if seq0 <= ev[0] <= seq1 and ev[2] == "write"]
        pt, nrm, surf = [None] * 3, [None] * 3, None
        for sq, pc, kind, w, addr, size, val in win:
            if pc in PT_STORES and a2 <= addr < a2 + 12:
                pt[(addr - a2) // 4] = val
            if a3 <= addr < a3 + 6 and size == 2:
                nrm[(addr - a3) // 2] = val & 0xFFFF   # the watch logs halfwords sign-extended
            if addr == e + 0x216 and pc in SURF_STORES:
                surf = val
        full = watched(a2, 12) and watched(a3, 6)
        g_pt = None if pt == [None] * 3 else pt
        g_n = None if nrm == [None] * 3 else nrm
        g_ret = int(c["ret"]["v0"], 16)
        ok = m["ret"] == g_ret and m["surface"] == surf and m["calls"] == c["pip"]
        if full:
            ok = ok and m["point"] == g_pt and m["normal"] == g_n
            stats["buffers compared"] += 1
        stats["calls"] += 1
        stats["match"] += ok
        kind = ("hit" if surf is not None else "miss -1" if g_ret == 0xFFFFFFFF else
                "lists exhausted" if (g_ret >> 24) == 0xFF else "other")
        stats[kind] += 1
        stats["pip calls"] += len(c["pip"])
        if kind == "hit":
            stats["hit fine" if (g_ret >> 11) & 1 else "hit coarse"] += 1
            stats["hit quad" if any(ra == 0x800A8244 for ra, _, _ in c["pip"]) else "hit tri"] += 1
            same = (hint & 0xFFF) == (g_ret & 0xFFF) and ((hint >> 14) & 0x3FF) == ((g_ret >> 14) & 0x3FF)
            stats["hit via cached record" if same else "hit via search"] += 1
        if not ok and stats["shown"] < 3:
            stats["shown"] += 1
            print(f"  MISMATCH {name} seq {seq0} site {site:#x}: guest ret {g_ret:#x} pt {g_pt} n {g_n} "
                  f"s {surf} pip {c['pip'][:6]}\n           model ret {m['ret']:#x} pt {m['point']} "
                  f"n {m['normal']} s {m['surface']} pip {m['calls'][:6]}")
    stats["slot key writes"] = len(slot_key_writes)
    if verbose:
        print(f"  {name}: " + ", ".join(f"{k} {v}" for k, v in sorted(stats.items()) if k != "shown"))
    bench.check(stats["calls"] > 0 and stats["match"] == stats["calls"],
                f"{name}: model == interpreter on {stats['match']}/{stats['calls']} calls")
    return stats


def check_trace(bench, cfg, verbose=True):
    if not os.path.exists(RRVERIFY):
        bench.check(False, f"{RRVERIFY} missing - the live comparison cannot run")
        return
    os.makedirs(WORK, exist_ok=True)
    agg = Counter()
    for name, patches, shifts, lod, frames, pad in RUNS:
        out, stop, ram = run_trace(name, patches, shifts, frames, pad)
        if verbose:
            print(f"  {name}: {'; '.join(stop)}")
        agg.update(replay(name, lod, out, ram, cfg, bench, verbose))
    if verbose:
        print("  total: " + ", ".join(f"{k} {v}" for k, v in sorted(agg.items()) if k != "shown"))
    bench.check(agg["hit coarse"] > 0 and agg["hit fine"] > 0, "both LOD branches hit")
    bench.check(agg["hit quad"] > 0 and agg["hit tri"] > 0, "both primitive shapes hit")
    bench.check(agg["hit via cached record"] > 0 and agg["hit via search"] > 0,
                "both the cached record and the searched one")
    bench.check(agg["lists exhausted"] > 0, "the lists-exhausted return")
    # slot +0x00/+0x04 are written only by the streamer, in the runs that end on a CD-ROM access;
    # the replay applies those writes, so they are reported, not excluded
    bench.check(agg["miss -1"] > 0, "the -1 return (no cell / no record under the point)")


def check_capture(bench, cfg, verbose=True):
    """Re-query each off-road capture at the player's own contact point, with his own +0x218 as
    the hint; report how often the answer reproduces +0x218 exactly."""
    tot = Counter()
    for name, f in ram_files():
        mem = Mem(open(f, "rb").read())
        e = mem.w(PLAYER0_BIKE_PTR)
        if not (mem.w(e + 0x184) & 1):
            continue
        hint = mem.w(e + 0x218)
        m = ground_query(mem, e, e + 0x1F8, hint, cfg)
        same = m["ret"] == hint
        tot["captures"] += 1
        tot["reproduced"] += same
        tot["surface == +0x216"] += (m["surface"] == mem.bu(e + 0x216))
        if verbose:
            print(f"  {name}: +0x218 {hint:#010x}  model {m['ret']:#010x}  "
                  f"+0x216 {mem.bu(e + 0x216)} model {m['surface']}")
    bench.check(tot["captures"] >= 10 and tot["reproduced"] * 10 >= tot["captures"] * 8,
                f"capture consistency: {tot['reproduced']}/{tot['captures']} off-road frames reproduce +0x218")


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["info", "cells", "offroad", "trace", "capture", "verify"])
    ap.add_argument("--mutate", action="store_true")
    a = ap.parse_args(argv)
    cfg = config(a.mutate)
    cfg["mutate"] = a.mutate
    bench = Bench()
    if a.cmd in ("info", "verify"):
        print("skeleton:")
        check_skeleton(bench)
    if a.cmd in ("cells", "verify"):
        print("cells:")
        check_cells(bench, cfg)
    if a.cmd in ("offroad", "verify"):
        print("offroad:")
        check_offroad(bench, cfg)
    if a.cmd in ("capture", "verify"):
        print("capture:")
        check_capture(bench, cfg)
    if a.cmd in ("trace", "verify"):
        print("trace:")
        check_trace(bench, cfg)
    print(f"ground: {bench.n} checks, {bench.fail} failures")
    return 1 if (a.cmd == "verify" and bench.fail and not a.mutate) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
