"""Scout probe for the IMPACT OUTCOME and the RIDER STANCE layer of Road Rash: Jailbreak (USA, SLUS_01053).

The subject is what a contact port needs below the pair pass: the impact outcome solver `RASHCDG 0x800AF3B0`
and the speed hand-over `0x80080D1C` under it, the rider stance event `0x800C4550` with its gate `0x800C3E9C`,
`0x800C29F0`, `0x80090D84` and `0x80091468`, and the combat entry points `0x800C035C` / `0x800C2348`.

This probe re-derives their behaviour independently of the C++ tree:

  * SKELETON: image hashes, true extents (prologue to the last `jr ra`), the `jal` word at every call site
    the transcription names, the jump tables, the literal words the arithmetic rests on, a COP2 / jalr
    census;
  * LIVE: the transcriptions (tools\\scout\\impact_models\\, one module per function) are run as MODELS against the real functions
    executing in our interpreter, on snapshot copies that differ from the player's savestates by DATA only
    (RUNS below). The harness is pairs.py's - per call, every write
    outside the own frame in order, the exact call sequence and the return value, callees replayed - plus two
    checks pairs.py does not make: stack arguments against the guest's own words, and every register argument
    at the callee's entry probe;
  * FACTS: what the runs show the unmodified game doing (a car hit, a bump, a crash and the knock-off chain,
    the launch cases, a punch, a knock-out).

Reads ONLY work\\disc_us and work\\oracle\\state; writes ONLY under work\\impact (snapshot copies, trace
outputs). Uses <build>\\rrverify.exe read-only. Never copies game bytes into the repo.

    python tools\\scout\\impact.py info             # skeleton only
    python tools\\scout\\impact.py live [--only a,b] [--keep]   # the runs, without the exit status
    python tools\\scout\\impact.py verify           # impact: <n> checks, <m> failures
    python tools\\scout\\impact.py verify --mutate  # each mutated claim must fail its own check
"""

from __future__ import annotations

import argparse
import bisect
import os
import shutil
import struct
import subprocess
import sys
from collections import Counter

import warnings  # noqa: E402
warnings.filterwarnings("ignore", category=SyntaxWarning)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pairs as P  # noqa: E402

ROOT = P.ROOT
STATE_DIR = P.STATE_DIR
WORK = os.path.join(ROOT, "work", "impact")
RRVERIFY = P.RRVERIFY
OVL_BASE = P.OVL_BASE
SHA1 = P.SHA1
MUTATE = P.MUTATE

u32, s32, iabs = P.u32, P.s32, P.iabs
word_at = P.word_at


# ---------------------------------------------------------------------------
# extents and call sites over BOTH images (pairs.py only scans RASHCDG, and cuts a function at the next
# `jal` target, which is wrong for functions such as 0x80091468 that contain unreferenced code or are
# followed by a function only reached through a table)
# ---------------------------------------------------------------------------

def _img_of(a):
    im = P.images()
    return im["G"] if im["G"].contains(a) else im["SLUS"]


def extent(f):
    """[f, end): scan forward to the first `jr ra` at or past every forward branch / `j` target seen so far,
    and include its delay slot"""
    a, far = f, f
    img = _img_of(f)
    while img.contains(a):
        w = word_at(a)
        op = w >> 26
        if op in (1, 4, 5, 6, 7, 0x14, 0x15, 0x16, 0x17):
            far = max(far, a + 4 + 4 * (s32((w & 0xFFFF) << 16) >> 16))
        elif op == 2:
            far = max(far, ((a + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2))
        if w == 0x03E00008 and a >= far:
            return f, a + 8
        a += 4
    raise ValueError(f"no end for {f:#x}")


_CS = {}


def call_sites(target):
    """every `jal target` in RASHCDG and in SLUS text"""
    if not _CS:
        for key in ("G", "SLUS"):
            for a, w in P.images()[key].words():
                if (w >> 26) == 3:
                    _CS.setdefault(((a + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2), []).append(a)
    return _CS.get(target, [])


def jals_in(lo, hi):
    return [a for a in range(lo, hi, 4) if (word_at(a) >> 26) == 3]


def callees(f):
    lo, hi = extent(f)
    return Counter(P.jal_target(a) for a in jals_in(lo, hi))


# ---------------------------------------------------------------------------
# runs: snapshot copies that differ from a savestate by DATA only
# ---------------------------------------------------------------------------

WATCHES = list(P.WATCHES)          # entities, collision globals, stack, 0x8005B000..


def watched_in(watches):
    def w(a, n):
        return any(lo <= a and a + n <= lo + ln for lo, ln, _ in watches)
    return w


def prepare(name, src, edits):
    """copy savestate `src` to work\\impact\\state-<name>, apply `edits`: [(addr, size, value)] absolute
    stores, or (addr, 'add', delta) 32-bit additions"""
    dst = os.path.join(WORK, "state-" + name)
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    shutil.copytree(os.path.join(STATE_DIR, src) if not os.path.isabs(src) else src, dst)
    p = os.path.join(dst, "ram.bin")
    ram = bytearray(open(p, "rb").read())
    for a, n, v in edits:
        o = a & 0x1FFFFF
        if n == "add":
            struct.pack_into("<I", ram, o, u32(struct.unpack_from("<I", ram, o)[0] + v))
        else:
            struct.pack_into({4: "<I", 2: "<H", 1: "<B"}[n], ram, o, v & ((1 << (8 * n)) - 1))
    open(p, "wb").write(ram)
    return dst, bytes(ram)


# callees too hot to probe at their entry (thousands of calls a frame); their arguments are checked by
# re-executing the jal's delay slot as pairs.py does
HOT = {0x8001FC90, 0x80010028, 0x8002E698, 0x8002EE50, 0x8002EAD8, 0x800B6AAC, 0x80020018, 0x8002E468, 0x8002EB78,
       0x8002ECB8, 0x8004CF74, 0x8001FF3C, 0x8002E548, 0x8002E570, 0x8002E810, 0x8002F0F4, 0x8002E604, 0x8002E6F8}


def probes_for(funcs, callee_entries=False):
    ps = set()
    for f in funcs:
        lo, hi = extent(f)
        ps.add(f)
        ps.update(a + 8 for a in jals_in(lo, hi))
        ps.update(c + 8 for c in call_sites(f))
        if callee_entries:
            ps.update(t for t in (P.jal_target(a) for a in jals_in(lo, hi)) if t not in HOT)
    return sorted(ps)


def run_trace(name, state, frames, funcs=(), pad="0xFFFF", watches=None, probes=(), calls=True, cop2=False,
              out=None, explore=False, callee_entries=False):
    out = out or os.path.join(WORK, "tr-" + name)
    cmd = [RRVERIFY, "trace", "--state", state, "--frames", str(frames), "--no-gpu", "--pad", str(pad),
           "--out", out]
    if not cop2:
        cmd.append("--no-cop2")
    if calls:
        cmd.append("--calls")
    if explore:
        cmd.append("--explore")
    for a, n, w in (WATCHES if watches is None else watches):
        cmd += ["--watch", f"{a:#x}:{n}:{w}"]
    for p in sorted(set(probes_for(funcs, callee_entries)) | set(probes)):
        cmd += ["--probe", f"{p:#x}:p{p:x}"]
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
    stop = [ln.split(None, 1)[1].strip() for ln in r.stdout.splitlines()
            if ln.startswith(("stopped", "buffer swaps"))]
    return out, stop, r.stdout


def load_trace(out, ram):
    return P.Trace(out, ram)


def _stack_index(tr):
    if not hasattr(tr, "_stk"):
        d = {}
        for sq, pc, a, n, v in tr.writes:
            if n == 4 and 0x801FE000 <= a < 0x80200000:
                d.setdefault(a, []).append((sq, v))
        tr._stk = d
    return tr._stk


def _strict_call(m):
    """two checks pairs.Machine.call does not make: (1) every STACK argument against the
    guest's own word at the call (pairs compares it with the model's own store, which cannot fail); (2) every
    register argument against the callee's ENTRY probe when the run has one, which also covers the arguments a
    delay slot builds from a caller-saved temporary"""
    orig, idx = m.call, _stack_index(m.tr)
    m.entry_checked = 0

    def call(target, a0=None, a1=None, a2=None, a3=None, stack=()):
        if m.ci < len(m.gcalls):
            seq, frm, tgt, args, sp = m.gcalls[m.ci]
            for k, v in enumerate(stack):
                if v is None:
                    continue
                ad = u32(m.sp + 16 + 4 * k)
                L = idx.get(ad, [])
                i = bisect.bisect_right(L, (seq + 1, 0xFFFFFFFF)) - 1
                g = L[i][1] if i >= 0 else struct.unpack_from("<I", m.tr.ram0, ad & 0x1FFFFF)[0]
                if g != u32(v):
                    raise P.Mismatch(f"call #{m.ci + 1} {target:#x}: stack arg {k} model {u32(v):#x} guest {g:#x}")
            ent = m.tr.probes.get(target)
            if ent and tgt == target:
                j = bisect.bisect_right([e[0] for e in ent], seq)
                if j < len(ent) and ent[j][1]["sp"] == sp:
                    regs = ent[j][1]
                    for k, mv in enumerate((a0, a1, a2, a3)):
                        if mv is not None and u32(mv) != regs["a%d" % k]:
                            raise P.Mismatch(f"call #{m.ci + 1} {target:#x} from {frm:#x}: a{k} model {u32(mv):#x} "
                                             f"guest {regs['a%d' % k]:#x} (at the callee's entry)")
                    m.entry_checked += 1
                    return orig(target, None, None, None, None, stack)     # checked exactly: skip the re-execution
        return orig(target, a0, a1, a2, a3, stack)
    m.call = call


ARGSTAT = Counter()


def run_model(tr, model, entry, frame, watched, rets=None, cover=None, strict=False):
    """pairs.run_model with this file's extent and both images' call sites: every call of `entry` in the
    trace -> (seq, ok, why). strict=True adds _strict_call's two argument checks."""
    lo, hi = extent(entry)
    rets = rets if rets is not None else [c + 8 for c in call_sites(entry)]
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
            P.put(base, W[bi][2], W[bi][3], W[bi][4])
            bi += 1
        m = P.Machine(tr, bytearray(base), seq, ex[0], regs, lo, hi, frame)
        m.cover = cover if cover is not None else set()
        if strict:
            _strict_call(m)
        try:
            v0 = model(m, regs["a0"], regs["a1"], regs["a2"], regs["a3"])
            i0, i1 = bisect.bisect_right(tr.wseq, seq), bisect.bisect_left(tr.wseq, ex[0])
            g = [(a, n, v) for sq, pc, a, n, v in W[i0:i1]
                 if lo <= pc < hi and not any(s0 < sq < s1 and pc != d for s0, s1, d in m.windows)
                 and not (m.sp <= a < regs["sp"])]
            mw = [w for w in m.mwrites if not (m.sp <= w[0] < regs["sp"]) and watched(w[0], w[1])]
            if m.ci != len(m.gcalls):
                c = m.gcalls[m.ci]
                raise P.Mismatch(f"model made {m.ci} calls, guest {len(m.gcalls)} (next {c[2]:#x} from {c[1]:#x})")
            if mw != g:
                i = next((i for i, (x, y) in enumerate(zip(mw, g)) if x != y), min(len(mw), len(g)))
                raise P.Mismatch(f"write #{i}: model {[(hex(a), n, hex(v)) for a, n, v in mw[i:i + 2]]} "
                                 f"guest {[(hex(a), n, hex(v)) for a, n, v in g[i:i + 2]]} ({len(mw)} vs {len(g)})")
            if v0 is not None and u32(v0) != ex[1]["v0"]:
                raise P.Mismatch(f"v0 model {u32(v0):#x} guest {ex[1]['v0']:#x}")
            ARGSTAT["calls"] += m.ci
            ARGSTAT["entry"] += getattr(m, "entry_checked", 0)
            ARGSTAT["unchecked"] += m.unchecked
            out.append((seq, True, f"{len(g)} writes, {m.ci} calls"))
        except P.Mismatch as e:
            out.append((seq, False, str(e)))
    return out


def sat16(v):
    return -0x8000 if v < -0x8000 else 0x7FFF if v > 0x7FFF else v


def gte_op(r11, r22, r33, ir1, ir2, ir3):
    """COP2 command 0x0178000C = OP sf=1 lm=0, the only GTE command in the functions read here, always
    issued as `ctc2 R11R12 <- t4 ($0); ctc2 R22R23 <- t5 ($2); ctc2 R33 <- t6 ($4); mtc2 IR1..IR3 <- t4..t6
    ($9..$11); cop2 OP; mfc2 IR1..IR3 -> t4..t6`. Inputs are the 16-bit halves as the GTE sees them (signed);
    returns the three IR values as mfc2 delivers them (sign-extended 16-bit, as u32).
        IR1 = sat16((R22*IR3 - R33*IR2) >> 12), IR2 = sat16((R33*IR1 - R11*IR3) >> 12),
        IR3 = sat16((R11*IR2 - R22*IR1) >> 12)"""
    h = lambda v: ((v & 0xFFFF) ^ 0x8000) - 0x8000
    r11, r22, r33, ir1, ir2, ir3 = (h(x) for x in (r11, r22, r33, ir1, ir2, ir3))
    m1 = (r22 * ir3 - r33 * ir2) >> 12
    m2 = (r33 * ir1 - r11 * ir3) >> 12
    m3 = (r11 * ir2 - r22 * ir1) >> 12
    return tuple(u32(sat16(x)) for x in (m1, m2, m3))


# ---------------------------------------------------------------------------
# the live runs: every edit is DATA in a snapshot copy; the code is never touched
# ---------------------------------------------------------------------------

PLAYER, CAR = P.PLAYER, P.CAR
FX = lambda v: int(round(v * 65536))
FRAME_TICK = 0x8008AB00                       # RaceTick entry: one probe hit per game frame


def _rd(ram, a, f="<i"):
    return struct.unpack_from(f, ram, a & 0x1FFFFF)[0]


def car_edits(ram, along, lateral, speed=None):
    """the traffic car put on the player's road slice (pairs.car_on_player), `along` / `lateral` units
    from him; optionally the player's speed +0x1E0 and its copy +0x240 set to `speed` (NOT a placement:
    a speed edit, said plainly)"""
    ed = [(a, 4, v) for a, _, v in P.car_on_player(ram, FX(along), FX(lateral))]
    if speed is not None:
        ed += [(PLAYER + 0x1E0, 4, FX(speed)), (PLAYER + 0x240, 4, FX(speed))]
    return ed


# --- the edit builders. Each takes the source RAM and returns [(addr, size, value)] / (addr, 'add', delta).
#     "placement" = a box / road-state move; every other kind is named NOT a placement in the run's note.

QUICK, RR, PACK = "quick", "rr-race", "rr-pack"
VOL0, BIKE6, RIDER6 = P.VOL0, P.BIKE6, P.RIDER6
BIKE = lambda k: PLAYER + 1096 * k
PROP = lambda k: 0x800D1818 + 596 * k            # rr-pack's live pool-4 props
BOXF = P.BOX_FIELDS                              # +0x1D4, +0xB8, +0x1F8, +0x310, the eight corners


def conv(ed):
    """pairs.py (addr, delta|None, value) -> (addr, size|'add', value)"""
    return [(a, "add", d) if d is not None else (a, 4, v) for a, d, v in ed]


def spd(e, v):
    """NOT a placement: speed +0x1E0 and its copy +0x240 set"""
    return [(e + 0x1E0, 4, u32(FX(v))), (e + 0x240, 4, u32(FX(v)))]


def orw(ram, a, bits, n=4):
    """NOT a placement: a forced flag, word (or byte) at a OR-ed with bits"""
    return [(a, n, _rd(ram, a, {4: "<I", 2: "<H", 1: "<B"}[n]) | bits)]


def row(ram, e, off):
    return [_rd(ram, e + off + 2 * i, "<h") / 4096 for i in range(3)]


def pairs_run(name):
    """a run of pairs.py 21.1, rebuilt from its own recipe (pairs.edits_for)"""
    return lambda ram: conv(P.edits_for(name, ram))


def car(along=0.0, lat=0.0, pspd=None, cspd=None, extra=None):
    """rr-race: the traffic car on the player's road slice (a placement in road coordinates); speeds are not"""
    def f(ram):
        ed = conv(P.car_on_player(ram, FX(along), FX(lat)))
        if pspd is not None:
            ed += spd(PLAYER, pspd)
        if cspd is not None:
            ed += [(CAR + 0x1E0, 4, u32(FX(cspd)))]
        return ed + (extra(ram) if extra else [])
    return f


def bikes(dx, dz, pl_v=None, other=P.BIKE15):
    """quick: the player's box put (dx, dz) off bike `other` (pairs.py bb-side generalised)"""
    def f(ram):
        ed = conv(P.move_box(ram, PLAYER, _rd(ram, other + 0xB8) + FX(dx), _rd(ram, other + 0xC0) + FX(dz)))
        return ed + (spd(PLAYER, pl_v) if pl_v is not None else [])
    return f


def box_q(ox, oz, hx=2.0, hz=2.0, hy=3.0, dy=0.0, extra=None):
    """quick: pool-6 slot 0 (a class-1 pole) rewritten as a class-0 box in the spawner's layout (population.md
    4.1; its centre Y moved by dy) - NOT a placement for the record - and the player put (ox, oz) off its centre
    at his first pass"""
    def f(ram):
        vx, vz = _rd(ram, VOL0 + 0x0C), _rd(ram, VOL0 + 0x14)
        ed = conv(P.move_box(ram, PLAYER, vx + FX(ox) - P.FIRST_STEP[0], vz + FX(oz) - P.FIRST_STEP[1]))
        c = [_rd(ram, VOL0 + 12 + 4 * k) for k in range(3)]
        c[1] += FX(dy)
        ed += [(VOL0 + 8, 4, 0), (VOL0 + 0x84, 4, FX(hx)), (VOL0 + 0x88, 4, FX(hz)), (VOL0 + 0x8C, 4, FX(hy))]
        if dy:
            ed.append((VOL0 + 0x10, 4, u32(c[1])))
        for k, (sx, sz) in enumerate([(-1, -1), (1, -1), (1, 1), (-1, 1)]):
            top = [c[0] + sx * FX(hx), c[1], c[2] + sz * FX(hz)]
            low = [top[0], top[1] - 0x8000 - FX(hy), top[2]]
            ed += [(VOL0 + 0x18 + 12 * k + 4 * i, 4, u32(top[i])) for i in range(3)]
            ed += [(VOL0 + 0x18 + 12 * (k + 4) + 4 * i, 4, u32(low[i])) for i in range(3)]
        return ed + (extra(ram) if extra else [])
    return f


def pole_q(dx, dz=0.0, pl_v=None, extra=None):
    """quick: the player put (dx, dz) off the class-1 pole of pool-6 slot 0 at his first pass"""
    def f(ram):
        vx, vz = _rd(ram, VOL0 + 0x0C), _rd(ram, VOL0 + 0x14)
        ed = conv(P.move_box(ram, PLAYER, vx + FX(dx) - P.FIRST_STEP[0], vz + FX(dz) - P.FIRST_STEP[1]))
        return ed + (spd(PLAYER, pl_v) if pl_v is not None else []) + (extra(ram) if extra else [])
    return f


def prop_near(k, along, lat, pl240=None, pspd=None, way="rev"):
    """rr-pack: prop k's box put `along` ahead / `lat` beside the player (placement); optionally the player's
    +0x240, and a prop speed `pspd` towards `way` (NOT placements: a velocity edit)"""
    def f(ram):
        h = row(ram, PLAYER, 0x1C2)
        px, pz = _rd(ram, PLAYER + 0xB8), _rd(ram, PLAYER + 0xC0)
        e = PROP(k)
        x, z = px + FX(h[0] * along - h[2] * lat), pz + FX(h[2] * along + h[0] * lat)
        dx, dz = x - _rd(ram, e + 0xB8), z - _rd(ram, e + 0xC0)
        fl = [0x0B8, 0x1F8] + [0x0C4 + 12 * j for j in range(8)]
        ed = [(e + o, "add", dx) for o in fl] + [(e + o + 8, "add", dz) for o in fl]
        if pl240 is not None:
            ed.append((PLAYER + 0x240, 4, FX(pl240)))
        if pspd is not None:
            src, sg = {"rev": (0x1C2, -1), "fwd": (0x1C2, 1), "left": (0x1B0, -1), "right": (0x1B0, 1)}[way]
            ed += [(e + 0x1C2 + 2 * i, 2, u32(sg * _rd(ram, PLAYER + src + 2 * i, "<h"))) for i in range(3)]
            ed.append((e + 0x1E0, 4, FX(pspd)))
        return ed
    return f


def rider6_ahead(t, stance=None, mount=None):
    """quick: the downed rider 6 put t first-pass steps ahead of the player (placement); its state +0x220 /
    mount +0x25C forced (NOT placements)"""
    def f(ram):
        tx = _rd(ram, PLAYER + 0xB8) + int(P.FIRST_STEP[0] * t)
        tz = _rd(ram, PLAYER + 0xC0) + int(P.FIRST_STEP[1] * t)
        ed = conv(P.move_box(ram, RIDER6, tx, tz))
        if stance is not None:
            ed.append((RIDER6 + 0x220, 2, stance))
        if mount is not None:
            ed.append((RIDER6 + 0x25C, 4, mount))
        return ed
    return f


def rider6_under(flag=0x40000000, extra=None):
    """quick: the downed rider 6's box (x, y, z) put at the player's first-pass spot, Y - 1.0 (placement),
    and its +0x228 |= flag (NOT a placement: nothing in quick sets it; 0x80091468 does)"""
    def f(ram):
        d = [_rd(ram, PLAYER + 0xB8) + P.FIRST_STEP[0] - _rd(ram, RIDER6 + 0xB8),
             _rd(ram, PLAYER + 0xBC) - 0x10000 - _rd(ram, RIDER6 + 0xBC),
             _rd(ram, PLAYER + 0xC0) + P.FIRST_STEP[1] - _rd(ram, RIDER6 + 0xC0)]
        ed = [(RIDER6 + fo + 4 * i, "add", d[i]) for fo in BOXF for i in range(3)]
        return ed + orw(ram, RIDER6 + 0x228, flag) + (extra(ram) if extra else [])
    return f


def knockoff_requests(spec, bit16=False):
    """quick: FORCED knock-off requests (rider +0x228 |= 0x8000 and the extra bits) and crash bits on AI bikes
    (bike +0x238 |= f) - NOT placements"""
    def f(ram):
        ed = []
        for k, (r, fc) in spec.items():
            rr = _rd(ram, BIKE(k) + 0x354, "<I")
            ed += orw(ram, rr + 0x228, r | (0x10000 if bit16 else 0))
            if fc:
                ed += orw(ram, BIKE(k) + 0x238, fc)
        return ed
    return f


KO_SPEC = {1: (0x8000, 0), 2: (0x28000, 0), 3: (0x48000, 0), 4: (0x8000, 0x10000), 5: (0x28000, 0x400),
           7: (0x8000, 0x20), 8: (0x8000, 0x60), 9: (0x8000, 0x100), 10: (0x8000, 0x80), 11: (0x8000, 0x200),
           12: (0x8000, 0x40280), 14: (0x8000, 0x4), 15: (0x8000, 0x40)}


def police(ram):
    """bb-side + FORCED: race type byte gs+4 = 35 (odd: a police race), the player's riderDef class nibble 2
    (a cop), rider 15's knock-off request - NOT placements"""
    gs = _rd(ram, P.GS, "<I")
    pd = _rd(ram, PLAYER + 0x43C, "<I")
    r15 = _rd(ram, P.BIKE15 + 0x354, "<I")
    return (conv(P.edits_for("bb-side", ram)) + [(gs + 4, 1, 35), (pd + 1, 1, (_rd(ram, pd + 1, "<B") & 0xF0) | 2)]
            + orw(ram, r15 + 0x228, 0x8000))


def place_bike(ram, k, dlat, dalong, rel=PLAYER):
    """bike k's box put dlat along the player's +0x1B0 row and dalong along his +0x210 row from his box centre"""
    lat, fwd = row(ram, rel, 0x1B0), row(ram, rel, 0x210)
    tx = _rd(ram, rel + 0xB8) + int(round((dlat * lat[0] + dalong * fwd[0]) * 65536))
    tz = _rd(ram, rel + 0xC0) + int(round((dlat * lat[2] + dalong * fwd[2]) * 65536))
    return conv(P.move_box(ram, BIKE(k), tx, tz))


def cop_beside(forward, riderdef_hp=None, armed=False):
    """rr-race: cop bike 16 released (+0x3A0 |= 0x10: NOT a placement, a forced flag), its road state words
    +0x144..+0x177 and +0x1EC copied from the player's (a placement in road coordinates), and the PLAYER's box
    put 6.2 units along -(his +0x1B0 row) and `forward` along +0x210 (placement): he rides up beside the cop.
    riderdef_hp: the cop's riderDef+0x0F set (NOT a placement); armed: the player's weapon block set (NOT)"""
    def f(ram):
        cop = BIKE(16)
        ed = [(cop + o, 4, _rd(ram, PLAYER + o, "<I")) for o in range(0x144, 0x178, 4)]
        ed += [(cop + 0x1EC, 4, _rd(ram, PLAYER + 0x1EC, "<I"))] + orw(ram, cop + 0x3A0, 0x10)
        lat, fwd = row(ram, PLAYER, 0x1B0), row(ram, PLAYER, 0x210)
        tx = _rd(ram, PLAYER + 0xB8) + int(round((-6.2 * lat[0] + forward * fwd[0]) * 65536))
        tz = _rd(ram, PLAYER + 0xC0) + int(round((-6.2 * lat[2] + forward * fwd[2]) * 65536))
        ed += conv(P.move_box(ram, PLAYER, tx, tz))
        if riderdef_hp is not None:
            ed.append((_rd(ram, cop + 0x43C, "<I") + 0x0F, 1, riderdef_hp))
        if armed:
            pd = _rd(ram, PLAYER + 0x43C, "<I")
            ed += [(pd + 0x2C, 2, 0x0605), (pd + 0x2E, 1, 2), (pd + 0x2F, 1, 5), (pd + 0x30, 2, 0x500)]
        return ed
    return f


def ai_fight(ram):
    """quick: AI bike 15 put 1.0 along the player's +0x1B0 and -0.3 along +0x210 (placement); a PLANTED AI
    command {op 16, target slot 0} at its +0x3BC and the player's speed 51.0 (NOT placements)"""
    return place_bike(ram, 15, 1.0, -0.3) + [(P.BIKE15 + 0x3BC, 4, 0x10)] + spd(PLAYER, 0x330000 / 65536)


def weapon(w):
    """NOT a placement: the player's current weapon riderDef+0x2E set"""
    return lambda ram: [(_rd(ram, PLAYER + 0x43C, "<I") + 0x2E, 1, w)]


# (name, source, edit builder or None, frames, pad, explore, note)
RUNS = [
    # --- 23: a car hit, a bump, a crash (rr-race); the pairs.py runs rebuilt from their own recipes
    ("hit", RR, pairs_run("tr-over0"), 20, "0xFFFF", False, "pairs tr-over0: the car on the player's own spot"),
    ("hit-side", RR, pairs_run("tr-over-side"), 20, "0xFFFF", False, "pairs tr-over-side: the car 1.6 to his side"),
    ("bump", RR, car(-3.0, 0.0, pspd=40.0), 20, "0xBFFF", False, "the car 3 ahead; player speed 40 (NOT a placement); Cross"),
    ("crash", RR, car(-6.0, 0.0, pspd=60.0), 20, "0xBFFF", False, "the car 6 ahead; player speed 60 (NOT a placement); Cross"),
    ("crash-90", RR, car(-6.0, 0.0, pspd=90.0), 20, "0xBFFF", False, "as crash, speed 90"),
    ("crash-4", RR, car(-4.0, 0.0, pspd=60.0), 20, "0xBFFF", False, "the car 4 ahead; speed 60"),
    ("car-side", RR, car(1.0, -1.6), 12, "0xFFFF", False, "the car 1 behind, 1.6 to the left"),
    ("car-exch", RR, car(0.0, 0.8, cspd=30.0), 20, "0xFFFF", False, "the car 0.8 aside; car speed 30 (NOT a placement)"),
    ("car-v50", RR, car(cspd=50.0), 20, "0xFFFF", False, "tr-over0 with car speed 50 (NOT a placement)"),
    ("land", RR, car(1.0, -1.6, extra=lambda r: [(PLAYER + 0x268, 4, FX(1.0))]), 12, "0xFFFF", False,
     "car-side with the player's +0x268 = 1.0 (NOT a placement)"),
    ("land-fast", RR, car(-6.0, 0.0, pspd=60.0, extra=lambda r: [(PLAYER + 0x268, 4, FX(1.0))]), 12, "0xBFFF", False,
     "crash with the player's +0x268 = 1.0 (NOT a placement)"),
    ("car-f100", RR, car(extra=lambda r: orw(r, PLAYER + 0x238, 0x100)), 20, "0xFFFF", False,
     "tr-over0 with flagsC |= 0x100 (FORCED)"),
    # --- quick: the pole, the box, the bike pair
    ("pole", QUICK, pairs_run("pole"), 20, "0xFFFF", False, "pairs pole"),
    ("pole-fast", QUICK, pole_q(0.3, pl_v=96.0), 20, "0xFFFF", False, "pairs pole, player speed 96 (NOT a placement)"),
    ("pole-3", QUICK, pole_q(0.3, pl_v=3.0), 20, "0xFFFF", False, "pairs pole, player speed 3 (NOT a placement)"),
    ("box", QUICK, pairs_run("box"), 20, "0xFFFF", False, "pairs box (the pole record rewritten as a 2x2x3 box)"),
    ("box-low", QUICK, box_q(1.2, 0.0, hy=0.3, dy=0.3), 20, "0xFFFF", False, "a 2x2x0.3 box, centre 0.3 up: a ride-over"),
    ("box-low5", QUICK, box_q(1.2, 0.0, hy=0.5, dy=-0.3), 20, "0xFFFF", False, "a 2x2x0.5 box, centre 0.3 down"),
    ("bb-side", QUICK, pairs_run("bb-side"), 20, "0xFFFF", False, "pairs bb-side"),
    ("bb-f100", QUICK, lambda r: conv(P.edits_for("bb-side", r)) + orw(r, PLAYER + 0x238, 0x100), 20, "0xFFFF", False,
     "bb-side with flagsC |= 0x100 (FORCED)"),
    ("bb-rear90", QUICK, bikes(0.0, 2.0, pl_v=90.0), 20, "0xFFFF", False, "the player 2.0 behind bike 15, speed 90"),
    # --- rr-pack's live props (the interpreter's --explore: unmodelled CD/MDEC registers read 0)
    ("prop-slow", PACK, prop_near(3, 1.2, 0.0, pl240=5.0), 20, "0xFFFF", True, "prop 3 1.2 ahead; player +0x240 = 5"),
    ("prop-far", PACK, prop_near(3, 2.5, 0.0), 20, "0xFFFF", True, "prop 3 2.5 ahead"),
    ("prop-kick", PACK, prop_near(3, 2.0, 0.2, pspd=10.0), 20, "0xFFFF", True,
     "prop 3 2.0 ahead, sliding at 10 towards the player (NOT a placement)"),
    ("prop-side", PACK, prop_near(3, 0.0, 0.55, pspd=10.0, way="left"), 20, "0xFFFF", True,
     "prop 3 0.55 beside, sliding at 10 (NOT a placement)"),
    ("prop-fwd", PACK, prop_near(3, -1.0, 0.0, pspd=60.0, way="fwd"), 20, "0xFFFF", True,
     "prop 3 1.0 behind, sliding forward at 60 into the player (NOT a placement)"),
    # --- 24: riders on the road, knock-offs, the launch cases, the police arm
    ("ride-over", QUICK, rider6_under(), 20, "0xFFFF", False, "the downed rider 6 under the player (+0x228 FORCED)"),
    ("ride-over-p", QUICK, rider6_under(extra=lambda r: [(PLAYER + 0x358, 4, BIKE(1))]), 20, "0xFFFF", False,
     "ride-over with the player's +0x358 := bike 1 (a PLANTED passenger pointer)"),
    ("ko-forced", QUICK, knockoff_requests(KO_SPEC), 10, "0xFFFF", False, "knock-off requests on 13 AI riders (FORCED)"),
    ("ko-forced16", QUICK, knockoff_requests(KO_SPEC, bit16=True), 10, "0xFFFF", False, "as ko-forced, + bit 16 (FORCED)"),
] + [
    (f"launch{st}", QUICK, rider6_ahead(t, st), 10, "0xFFFF", False,
     f"rider 6 {t} steps ahead, its stance FORCED to {st}: the player's bike hits it")
    for t, st in ((2.0, 38), (1.5, 39), (1.0, 40), (1.0, 41), (0.5, 42), (1.5, 90))
] + [
    ("police", QUICK, police, 10, "0xFFFF", False, "bb-side + a police race, the player a cop (FORCED)"),
    ("stance-l1", RR, None, 30, "0xFBFF", False, "rr-race, L1 held: the punch combo stances"),
    # --- 25: combat
    ("punch-miss", RR, None, 30, "0xF7FF", False, "rr-race, R1 held: a punch at the crashed bike 6"),
    ("punch-cop", RR, cop_beside(-0.3), 24, "0xF7FF", False, "the player rides up beside the released cop 16 and punches"),
    ("knock-cop", RR, cop_beside(-0.3, riderdef_hp=10), 30, "0xF7FF", False, "punch-cop, the cop's riderDef+0x0F = 10"),
    ("swing-cop", RR, cop_beside(-0.3, armed=True), 24, "0xF7FF", False, "punch-cop, the player armed (weapon 2)"),
    ("punch-last", RR, lambda r: cop_beside(-0.3)(r) + [(0x800CCB6C, 2, 6), (0x800CCB70, 4, _rd(r, _rd(r, P.GS, "<I") + 0x10))],
     12, "0xF7FF", False, "punch-cop with a PLANTED last opponent (slot 6, stamped now): too far, the picker runs"),
    ("ai-punch", QUICK, ai_fight, 14, "0xBFFF", False, "AI bike 15 beside the player with a PLANTED op 16"),
] + [
    (f"decode{a}w{w}", RR, (weapon(w) if w is not None else None), 8, pad, False, f"rr-race, action {a}" +
     (f", weapon {w} (NOT a placement)" if w is not None else ""))
    for a, w, pad in ((2, None, "0xFBFF"), (3, None, "0xFDFF"), (4, None, "0xFDEF"), (8, None, "0xFDBF"),
                      (7, 5, "0xF7BF"), (5, 7, "0xF7EF"), (1, 0, "0xF7FF"), (6, 4, "0xFBEF"))
]

WATCH_ALL = WATCHES + [(0x800D6000, 0x4000, "hud"), (0x801EE000, 0xA000, "anim"), (0x801FA000, 0x1000, "chan"),
                       (0x80000000, 0x400, "low")]


# ---------------------------------------------------------------------------
# the live comparison
# ---------------------------------------------------------------------------

class Bench(P.Bench):
    pass


def models():
    import impact_models as IM          # imported late: the models import this module's helpers
    return IM.load()


def do_run(spec, funcs, keep=False):
    """build the snapshot copy of one RUNS entry, run it, return (Trace, stop lines, edits, state dir, out dir)"""
    name, src, build, frames, pad, explore, note = spec
    srcdir = os.path.join(STATE_DIR, src)
    ram0 = open(os.path.join(srcdir, "ram.bin"), "rb").read()
    edits = build(ram0) if build else []
    st, ram = prepare("run-" + name, srcdir, edits)
    out, stop, so = run_trace("run-" + name, st, frames, funcs, pad=pad, watches=WATCH_ALL, explore=explore,
                              callee_entries=True, probes=FACT_PROBES)
    return load_trace(out, ram), stop, edits, st, out


NOT_RUN = {0x800C37B0}                   # StancePath: no acceptance-mode-0 call with a failing mask in any run


def check_live(bench, verbose=True, cover=False, only=None, keep=False, facts=None):
    mods = models()
    funcs = [f for f, _, _, _ in mods]
    agg = {f: Counter() for f in funcs}
    watched = watched_in(WATCH_ALL)
    seen = {}
    for spec in RUNS:
        if only and spec[0] not in only:
            continue
        tr, stop, edits, st, out = do_run(spec, funcs)
        line = []
        for f, frame, model, nm in mods:
            res = run_model(tr, model, f, frame, watched, strict=True)
            ok = sum(1 for r in res if r[1] is True)
            bad = [r for r in res if r[1] is False]
            agg[f]["calls"] += ok + len(bad)
            agg[f]["match"] += ok
            agg[f]["truncated"] += sum(1 for r in res if r[1] is None)
            if res:
                line.append(f"{nm} {ok}/{ok + len(bad)}")
            for r in bad[:2]:
                print(f"    MISMATCH {nm} {spec[0]} seq {r[0]}: {r[2]}")
        if facts:
            seen[spec[0]] = facts(spec[0], tr)
        if verbose:
            print(f"  {spec[0]} ({spec[1]}, {len(edits)} edits, {spec[3]} frames, pad {spec[4]}): "
                  f"{(stop or ['?'])[0][:48]}")
            print("    " + ", ".join(line))
        if not keep:
            shutil.rmtree(out, ignore_errors=True)
            shutil.rmtree(st, ignore_errors=True)
    for f, frame, model, nm in mods:
        a = agg[f]
        if only and not a["calls"]:
            continue
        if f in NOT_RUN:                     # a transcription only: the claim is that no run enters it
            bench.check(a["calls"] == 0, f"{nm} {f:#x}: entered in no run ({a['calls']} calls) - transcription only")
            continue
        bench.check(a["calls"] > 0 and a["match"] == a["calls"],
                    f"{nm} {f:#x}: model == interpreter on {a['match']}/{a['calls']} calls")
        if verbose:
            print(f"  {nm:18s} {f:#x}: {a['match']}/{a['calls']} calls ({a['truncated']} cut off by the end of a run)")
    if verbose:
        print(f"  calls made by the models: {ARGSTAT['calls']}; register arguments compared at the callee's entry "
              f"probe on {ARGSTAT['entry']}, by re-executing the delay slot on the rest; arguments left uncompared "
              f"(a delay slot reading a caller-saved temporary, hot callees only): {ARGSTAT['unchecked']}")
    return seen


# ---------------------------------------------------------------------------
# skeleton: the claims that are read straight off the images
# ---------------------------------------------------------------------------

def cfg(orig, mutated):
    return mutated if MUTATE["on"] else orig


# (entry, bytes) - every one from the prologue to the last `jr ra` (extent())
EXTENTS = {
    0x800AF3B0: 4448, 0x800AF224: 396, 0x800AF0A0: 388, 0x800B11B4: 236, 0x80083928: 1544, 0x80083F30: 1588,
    0x80083864: 196, 0x80080D1C: 4192, 0x80080B10: 524, 0x800B0510: 1204, 0x800B5EB4: 1752, 0x800B3838: 664,
    0x800B16F4: 644, 0x800AD9BC: 696, 0x800B2F94: 944, 0x800C4550: 136, 0x800C3E9C: 1464, 0x800C37B0: 416,
    0x800C4500: 80, 0x800C4454: 172, 0x800C2FF4: 272, 0x800BFD24: 80, 0x800BFC5C: 200, 0x800C29F0: 1196,
    0x800BFE58: 172, 0x80090D84: 1764, 0x80091468: 4100, 0x800C2178: 160, 0x800C2348: 580, 0x800C1DD4: 604,
    0x800C035C: 1620, 0x800C09B0: 568, 0x800C159C: 532, 0x80078DB4: 3436,
}

# (site, callee): every call the transcription names as the path from a contact to a speed, a crash, a knock-off,
# a launch, a stance and a punch
SKELETON = [
    # 23: the impact outcome
    (0x800AF170, 0x800AF3B0), (0x800AF384, 0x800AF3B0), (0x800B148C, 0x800AF3B0), (0x800AC98C, 0x800AF224),
    (0x800AD68C, 0x800AF224), (0x800B0C98, 0x800AF224), (0x800B11D8, 0x800AF224), (0x800A794C, 0x800AF0A0),
    (0x800AEFD8, 0x800AF0A0), (0x800A7974, 0x800B11B4), (0x800B1094, 0x800B11B4),
    (0x800AF670, 0x800B3838), (0x800AF680, 0x80017B30), (0x800AF948, 0x800B16F4), (0x800AFD7C, 0x800B5EB4),
    (0x800AFF0C, 0x800AD9BC), (0x800AFFE0, 0x800A8DF0), (0x800B00F4, 0x800A8DF0), (0x800B0000, 0x800B0510),
    (0x800B0024, 0x800B2F94), (0x800B0128, 0x80083928), (0x800B0388, 0x80083F30), (0x800B04CC, 0x800B658C),
    (0x80083E7C, 0x80080D1C), (0x8008448C, 0x80080D1C), (0x80083764, 0x80080D1C), (0x80083EA0, 0x80080B10),
    (0x80083DE0, 0x80083864), (0x80083A38, 0x80084564), (0x80083A4C, 0x800849D8), (0x800ADBC8, 0x80084BE8),
    # 24: the knock-off, the launch, the stance layer
    (0x8007DCD8, 0x80078DB4), (0x8007DCE4, 0x80078DB4),
    (0x80090BBC, 0x80090D84), (0x80072A00, 0x80090D84), (0x80072A80, 0x80090D84), (0x80074E04, 0x80090D84),
    (0x800912A0, 0x80091468), (0x800A994C, 0x80091468), (0x80090CDC, 0x80091468), (0x80072A5C, 0x80091468),
    (0x80072ADC, 0x80091468), (0x800C334C, 0x80091468), (0x800C360C, 0x80091468), (0x800C3794, 0x80091468),
    (0x80090E18, 0x800C4550), (0x80090E6C, 0x800C4550), (0x80090ECC, 0x800C4550), (0x80090F00, 0x800C4550),
    (0x80090F34, 0x800C4550), (0x80090F60, 0x800C4550), (0x80092408, 0x800C4550), (0x800A95C0, 0x800C4550),
    (0x800C4574, 0x800C3E9C), (0x800C4594, 0x800C4500), (0x800C45A4, 0x800C4454), (0x800C45B4, 0x800C2FF4),
    (0x800C4070, 0x800C37B0), (0x800C4538, 0x800BFD24), (0x800C448C, 0x800BFC5C), (0x800AD788, 0x800C29F0),
    (0x800C4158, 0x80012858), (0x800C41B0, 0x80068D20), (0x800C42A8, 0x8005C140), (0x800C4318, 0x8005C0B0),
    (0x800C4390, 0x8005C018), (0x800C43F0, 0x8005BF6C), (0x800C41F8, 0x8005BEF4),
    # 25: combat
    (0x8001D368, 0x800C2348), (0x800C2558, 0x800C1DD4), (0x800C2018, 0x800BCA68), (0x800BA780, 0x800C035C),
    (0x800B96F0, 0x800C035C), (0x800C0470, 0x800C09B0), (0x800C05E8, 0x800C09B0), (0x800C0890, 0x800C159C),
    (0x800C0960, 0x800C159C), (0x800C0978, 0x800C17B0), (0x800C0938, 0x800BFF04), (0x800C1B78, 0x800BF674),
]

# the words the arithmetic and the flags rest on
WORDS = [
    (0x800AF5DC, 0xAE600240, "sw zero,576(s3) - a crawling bike's +0x240 := 0 (the solver's own speed write)"),
    (0x800AFDE4, 0xAE720340, "sw s2,832(s3) - +0x340 := the shape: the bike is ON TOP of its partner"),
    (0x800AFEF0, 0xAE7E01E0, "sw s8,480(s3) - the landing-on-a-car speed"),
    (0x80083E30, 0xAE620238, "sw v0,568(s3) - HitOutcome ORs the hit CLASS into flagsC"),
    (0x80084440, 0xAE420238, "sw v0,568(s2) - ImpactTurn ORs the hit CLASS into flagsC"),
    (0x80083EDC, 0xAE620238, "sw v0,568(s3) - flagsC |= 0x1000 / 0x800, what 0x80071BCC acts on"),
    (0x80081D1C, 0xAE330240, "sw s3,576(s1) - HitSpeed: THE new +0x240"),
    (0x8007916C, 0xAEE30228, "sw v1,552(s7) - the rider's +0x228 |= 0x8000: the knock-off request of a class-8 hit"),
    (0x80090DC8, 0xA060000F, "sb zero,15(v1) - the knock-off zeroes riderDef+0x0F"),
    (0x80091520, 0x00400008, "jr v0 - 0x80091468's jump table dispatch"),
    (0x800C2534, 0xA202003C, "sb v0,60(s0) - the decoder's command byte riderDef+0x3C"),
    (0x800C1A58, 0xA044000F, "sb a0,15(v0) - ApplyHit's new health"),
    (0x800C19C0, 0xA0A0000F, "sb zero,15(a1) - ApplyHit's knock-out"),
]

# 0x80091468's jump table: 54 words at 0x8005B7A4, index stance - 38
JT_1468 = {38: 0x80091528, 39: 0x800918B8, 89: 0x800918B8, 40: 0x80091B64, 88: 0x80091B64, 41: 0x80091F44,
           90: 0x80091F44, 42: 0x800916D8, 91: 0x800916D8}
JT_DEFAULT = 0x8009215C

# COP2 words (OP commands) per function; every other function read here has none, and none has a jalr
COP2 = {0x80080D1C: (40, 4), 0x80080B10: (10, 1), 0x80091468: (60, 6)}


def frame_of(f):
    """the function's own frame: -(simm of the first `addiu sp,sp,-N` in its first 4 words), else 0 (a leaf)"""
    for a in range(f, f + 16, 4):
        w = word_at(a)
        if (w >> 16) == 0x27BD:
            return -(s32((w & 0xFFFF) << 16) >> 16)
    return 0


def check_skeleton(bench, verbose=True):
    im = P.images()
    bench.check(im["SLUS"].sha1 == SHA1["SLUS"], "SLUS sha1")
    bench.check(im["G"].sha1 == SHA1["G"], "RASHCDG sha1")
    for f, size in EXTENTS.items():
        want = cfg(size, 3152) if f == 0x800AF3B0 else size          # mutated: a shorter extent
        lo, hi = extent(f)
        bench.check(hi - lo == want, f"extent of {f:#x}: {hi - lo} bytes (claimed {want})")
    for f, frame, model, nm in models():
        bench.check(frame_of(f) == frame, f"{nm} {f:#x}: frame {frame} == the prologue's {frame_of(f)}")
    for site, callee in SKELETON:
        want = cfg(callee, 0x80083928) if site == 0x8008448C else callee   # mutated: the pole's hand-over via 0x80083928
        bench.check(P.jal_target(site) == want, f"jal at {site:#x} -> {want:#x}")
    n = len(call_sites(0x800C4550))
    bench.check(n == 50, f"StanceEvent 0x800C4550 has {n} call sites over RASHCDG + SLUS (50)")
    for a, w, what in WORDS:
        bench.check(word_at(a) == w, f"word at {a:#x}: {what}")
    got = {i + 38: word_at(0x8005B7A4 + 4 * i) for i in range(54)}
    want = {st: JT_1468.get(st, JT_DEFAULT) for st in range(38, 92)}
    if MUTATE["on"]:
        want[88] = 0x800918B8                                        # mutated: 88 shares 39's case
    bench.check(got == want and word_at(0x800914FC) == 0x2C620036,
                "0x80091468 jump table 0x8005B7A4: 38 A, 39/89 B, 40/88 C, 41/90 D, 42/91 E, 43..87 default")
    fs = [f for f, _, _, _ in models()] + [0x80078DB4]
    census, jalr = {}, 0
    for f in fs:
        lo, hi = extent(f)
        ws = [word_at(a) for a in range(lo, hi, 4)]
        c = sum(1 for w in ws if (w >> 26) in (0x12, 0x32, 0x3A))
        o = sum(1 for w in ws if w == 0x4B78000C)
        if c:
            census[f] = (c, o)
        jalr += sum(1 for w in ws if (w >> 26) == 0 and (w & 0x3F) == 9)
    bench.check(census == COP2 and jalr == 0,
                f"COP2 census of {len(fs)} functions: {[(hex(k), v) for k, v in census.items()]} (only OP), jalr {jalr}")
    if verbose:
        print(f"  {len(EXTENTS)} extents, {len(SKELETON)} call sites, {len(WORDS)} words, 1 jump table, "
              f"{len(fs)} functions censused")


# ---------------------------------------------------------------------------
# facts: what the unmodified game does in the runs
# ---------------------------------------------------------------------------

FACT_PROBES = [0x80071D24, 0x8008AB00, 0x800C3E9C]


def fixmul(a, b):
    return u32((s32(a) * s32(b)) >> 16)


def dotlcm(ram, a, b):
    """SLUS 0x8002E698 (bench row dot_lcm): MVMVA of two s16[3], MAC1 >> 8"""
    return u32(sum(_rd(ram, a + 2 * i, "<h") * _rd(ram, b + 2 * i, "<h") for i in range(3)) >> 8)


def run_facts(name, tr):
    """the measurements one run contributes (kept small: the trace is deleted after the run)"""
    W = tr.writes
    out = {}
    wr = lambda pc: [(sq, a, v) for sq, p, a, n, v in W if p == pc]
    frames = sorted(s for s, _ in tr.probes.get(0x8008AB00, []))
    fr = lambda sq: bisect.bisect_right(frames, sq)
    out["speed"] = [(fr(sq), a, v) for sq, a, v in wr(0x80081D1C)]
    out["class"] = [(fr(sq), a, v) for sq, a, v in wr(0x80083E30) + wr(0x80084440)]
    out["ko_req"] = [(fr(sq), a, v) for sq, a, v in wr(0x8007916C)]
    out["hp0"] = [(fr(sq), a, v) for sq, a, v in wr(0x80090DC8)]
    rider = _rd(tr.ram0, PLAYER + 0x354, "<I")
    out["stance"] = [(fr(sq), v) for sq, p, a, n, v in W if p == 0x800C306C and a == rider + 0x220]
    out["mount"] = [(fr(sq), v) for sq, p, a, n, v in W if p == 0x800C3068 and a == rider + 0x25C]
    out["crashlaunch"] = [fr(s) for s, r in tr.probes.get(0x80071D24, []) if r["a0"] == PLAYER]
    ko_sites = set()
    for s, r in tr.probes.get(0x80090D84, []):
        for site in call_sites(0x80090D84):
            if tr.post(site + 8, s, r["sp"]):
                ko_sites.add(site)
                break
    out["ko_sites"] = sorted(ko_sites)
    out["launch_in"] = []
    if tr.probes.get(0x80091468):
        snap = P.snapshots(tr, [s for s, _ in tr.probes[0x80091468]])
        out["launch_in"] = [_rd(snap[s], r["a0"] + 0x220, "<H") for s, r in tr.probes[0x80091468]]
    out["cmd"] = [v for sq, a, v in wr(0x800C2534)]
    out["taunt"] = sum(1 for s, r in tr.probes.get(0x80017BA0, []) if r["a2"] == 78 and 0x800C2348 <= r["ra"] < 0x800C258C)
    out["hp"] = [(a, v) for sq, a, v in wr(0x800C1A58)] + [(a, 0) for sq, a, v in wr(0x800C19C0)]
    out["flash"] = [(a, v) for sq, a, v in wr(0x800BF82C)]
    out["fight_from"] = sorted({r["ra"] for s, r in tr.probes.get(0x800C035C, [])})
    out["swings"] = [v for sq, a, v in wr(0x800C08B4)]
    out["gate"] = [(r["a0"] & 0xFFFF, _rd(tr.ram0, r["a1"] + 0x220, "<H")) for s, r in tr.probes.get(0x800C3E9C, [])[:1]]
    ret = [c for c in tr.calls if c[2] == 0x800C3E9C]
    out["gate_v0"] = []
    for s, r in tr.probes.get(0x800C3E9C, []):
        p = tr.post(0x800C4574 + 8, s, r["sp"])
        if p:
            out["gate_v0"].append((r["a0"] & 0xFFFF, p[1]["v0"]))
    if name == "hit" and tr.probes.get(0x80083928):
        s, r = tr.probes[0x80083928][0]
        ram = P.snapshots(tr, [s])[s]
        e, o = r["a0"], r["a1"]
        head = e + (0x360 if _rd(ram, e + 0x238, "<I") & 0x02000000 else 0x1C2)
        c = dotlcm(ram, head, o + 0x1C2)
        M = u32(_rd(ram, e + 0x13C) + _rd(ram, o + 0x13C))
        inv = (0x80000000 // u32((s32(M) >> 1) + (s32(u32(M - 2)) >> 31))) & 0xFFFFFFFF
        me, mo = fixmul(_rd(ram, e + 0x13C), inv), fixmul(_rd(ram, o + 0x13C), inv)
        vo = fixmul(_rd(ram, o + 0x1E0), iabs(c))
        v = s32(u32(fixmul(u32(me - mo), _rd(ram, e + 0x1E0)) + fixmul(u32(mo << 1), vo)))
        out["exchange"] = (max(max(v, 0), 0x10000), _rd(ram, e + 0x13C), _rd(ram, o + 0x13C), s32(c))
    return out


def sub(xs, ys):
    """ys occurs in xs as a contiguous run"""
    return any(xs[i:i + len(ys)] == ys for i in range(len(xs) - len(ys) + 1))


def check_facts(bench0, seen, verbose=True):
    mut = MUTATE["on"]

    class _B:                                   # print every fact, not only the failed ones
        def check(self, ok_, what):
            if ok_ and verbose:
                print("  ok", what)
            return bench0.check(ok_, what)
    bench = _B()
    fx = lambda v: f"{s32(v) / 65536:.4f}"
    ok = lambda name: name in seen
    # 1. the car hit: the new +0x240 is the 1-D elastic exchange with the weights +0x13C, and nothing else
    if ok("hit"):
        f = seen["hit"]
        got = [v for _, a, v in f["speed"] if a == PLAYER + 0x240]
        want = f.get("exchange", (None,))[0]
        if mut and want is not None:
            want = fixmul(want, 0x10000) + 1                         # mutated: off by one unit
        bench.check(bool(got) and got[0] == want and not f["class"],
                    f"hit: +0x240 := {[fx(v) for v in got]} at 0x80081D1C == the exchange "
                    f"{fx(want) if want is not None else '?'} (m {f.get('exchange', (0, 0, 0, 0))[1:3]} "
                    f"cos {f.get('exchange', (0, 0, 0, 0))[3]}); no class bit")
    # 2. the bump: class 1, rider state 26, the rider stays on
    if ok("bump"):
        f = seen["bump"]
        cls = [v & 0x3F for _, a, v in f["class"] if a == PLAYER + 0x238]
        bench.check(cls == [1] and 26 in [v for _, v in f["stance"]] and set(v for _, v in f["mount"]) <= {1}
                    and not f["ko_req"],
                    f"bump: class {cls}, stances {[v for _, v in f['stance']]}, mounts {[v for _, v in f['mount']]}")
    # 3. the crash: class 8 -> the knock-off request at 0x8007916C -> the presentation pass applies it -> launch
    if ok("crash"):
        f = seen["crash"]
        cls = [v & 0x3F for _, a, v in f["class"] if a == PLAYER + 0x238]
        st = [v for _, v in f["stance"]]
        mt = [v for _, v in f["mount"]]
        want_site = cfg(0x80090BBC, 0x80072A80)                      # mutated: 0x80072994 applies the knock-off
        good = (cls == [8] and len(f["ko_req"]) >= 1 and f["hp0"] and f["ko_sites"] == [want_site]
                and sub(st, [28, 40, 47]) and sub(mt, [1, 2, 3]) and f["crashlaunch"]
                and f["ko_req"][0][0] <= f["hp0"][0][0] < f["crashlaunch"][0])
        bench.check(good, f"crash: class {cls}, knock-off request (frame, +0x228) {[(a, hex(v)) for a, _, v in f['ko_req']]}, "
                          f"health 0 {[a for a, _, _ in f['hp0']]} from {[hex(s) for s in f['ko_sites']]}, stances {st}, "
                          f"mounts {mt}, CrashLaunch at frame {f['crashlaunch']}, speed {[fx(v) for _, _, v in f['speed']]}")
    # 4. the post: GLANCE at a right angle gives exactly 0
    if ok("pole"):
        f = seen["pole"]
        got = [v for _, a, v in f["speed"] if a == PLAYER + 0x240]
        bench.check(got == [cfg(0, 0x10000)], f"pole: +0x240 := {[fx(v) for v in got]} at 0x80081D1C")
    # 5. the launch cases: the stance 0x80091468 is entered with selects the jump-table case
    for st in (38, 39, 40, 41, 42, 90):
        n = f"launch{st}"
        if ok(n):
            bench.check(st in seen[n]["launch_in"], f"{n}: 0x80091468 entered with stances {seen[n]['launch_in']}")
    # 6. combat: the decoder's command bytes, the hits
    DEC = {"decode2wNone": [36], "decode3wNone": [71], "decode4wNone": [75], "decode8wNone": [77],
           "decode7w5": [148], "decode1w0": [cfg(142, 32)], "decode6w4": [148], "decode5w7": []}
    for n, want in DEC.items():
        if ok(n):
            got = seen[n]["cmd"][:1]
            bench.check(got == want and (want or seen[n]["taunt"] > 0),
                        f"{n}: the decoder writes {got} (taunts {seen[n]['taunt']})")
    if ok("punch-cop"):
        f = seen["punch-cop"]
        bench.check(f["cmd"][:1] == [32] and [v for _, v in f["hp"]] == [50] and f["fight_from"] == [0x800BA788],
                    f"punch-cop: command {f['cmd'][:1]}, cop health := {[v for _, v in f['hp']]}, FightUpdate "
                    f"returns to {[hex(x) for x in f['fight_from']]} (AiRunCommands' opcode-16 arm)")
    if ok("knock-cop"):
        f = seen["knock-cop"]
        bench.check([v for _, v in f["hp"]][-1:] == [0] and f["flash"] == [(0x800D6224, 1)],
                    f"knock-cop: health {[v for _, v in f['hp']]}, dash flash {[(hex(a), v) for a, v in f['flash']]}")
    if ok("ai-punch"):
        f = seen["ai-punch"]
        pd = 0x800D5758
        bench.check([(a, v) for a, v in f["hp"]] == [(pd + 0x0F, 85)],
                    f"ai-punch: the player's health := {[(hex(a), v) for a, v in f['hp']]}")
    if ok("swing-cop"):
        bench.check(seen["swing-cop"]["swings"][:1] == [4],
                    f"swing-cop: swings left riderDef+0x2F := {seen['swing-cop']['swings']} (0x800C08B4)")
    ff = sorted({x for f in seen.values() for x in f["fight_from"]})
    if ff:
        bench.check(ff == [0x800BA788], f"every FightUpdate call of every run returns to {[hex(x) for x in ff]} "
                                        f"(AiRunCommands' opcode-16 arm), the player's as the AI's")
    # 7. the stance gate: rr-race's every-frame event 11 on a rider in 11 is refused (the no-repeat rule)
    if ok("hit-side"):
        g = seen["hit-side"]["gate_v0"]
        bench.check(len(g) > 10 and all(ev == 11 and v == 0 for ev, v in g),
                    f"hit-side: {len(g)} StanceEvent calls, events {sorted(set(ev for ev, _ in g))}, all refused")


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["info", "live", "verify"])
    ap.add_argument("--mutate", action="store_true")
    ap.add_argument("--only", default=None, help="comma-separated run names")
    ap.add_argument("--keep", action="store_true", help="keep the snapshot copies and traces under work/impact")
    a = ap.parse_args(argv)
    MUTATE["on"] = a.mutate
    bench = Bench()
    print("skeleton:")
    check_skeleton(bench)
    if a.cmd in ("live", "verify"):
        print("live:")
        only = set(a.only.split(",")) if a.only else None
        seen = check_live(bench, only=only, keep=a.keep, facts=run_facts)
        print("facts:")
        check_facts(bench, seen)
    print(f"impact: {bench.n} checks, {bench.fail} failures")
    return 1 if (a.cmd == "verify" and bench.fail and not a.mutate) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
