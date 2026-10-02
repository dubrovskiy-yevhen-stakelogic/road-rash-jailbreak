"""Scout probe for THE RUNTIME ROAD LAYER - the functions the per-bike step's pass H and the
contact frame's road re-bind sit on (Road Rash: Jailbreak, USA, SLUS_01053).


What it proves.  Twenty-four resident functions are transcribed below as executable Python models,
one statement per original load/store/call, in the original's order:

    0x800374D4 RoadRebind          0x80037450 RoadRebindBody     0x80037338 RoadTrackPass (pass H)
    0x8003701C RoadTrack           0x80037104 RoadProbeAhead     0x800396A8 RoadReseat
    0x8003662C RoadPosition        0x8003E150 RoadClassPass (H)  0x8003DFF4 RoadClassify
    0x8003DDB0 NodeZone            0x8003DE28 RoadClass          0x8003EE68 RoadClassNode
    0x8003EF34 RoadCrossSection    0x8003E67C RoadEdgeClass      0x8003DF54 RoadsideRun
    0x8003F1F0 RoadsideRunOfSlice  0x8003AE24 RouteCheckPass (H) 0x8003AF9C RouteBind
    0x8003A9D8 RoadsideZones       0x8003DCB8 SegmentStraddle    0x8003B520 ProgressPass (H)
    0x8003B61C ProgressScalar      0x8003BE1C TurnSubObject      0x8003F408 RouteRecord

plus the reciprocal-square-root table builder 0x8002E080, whose whole output is compared with the
table the game left in RAM in every snapshot.

Each model is run against EVERY invocation of the original inside two captured traces of the
oracle interpreter (`rrverify trace` with a whole-RAM watch, the call log and pc probes; see
`capture`), and against synthetic invocations executed by PyCpu (the small R3000 executor of
roadq.py, extended here with the 1 KiB scratchpad and one GTE leaf).  For one invocation the model
starts from the exact guest RAM at the original's entry and the entry register file.  Every callee
the model calls is SUPPLIED BY THE TRACE: the model's call is matched against the original's next
call from the same body - target and every argument register - and the callee's stores and v0 are
replayed.  Compared: the ordered stores of the body (minus the prologue's callee-saved spills), the
set of its loads, the calls, and v0.  Scratchpad loads (the active-entity list at 0x1F800000) are
answered from a reconstruction and are not compared, because the oracle's watch does not cover the
scratchpad; the list itself is reconstructed from probes on its two writers.

PyCpu is itself checked first: every natural invocation of every modelled function is re-executed
from the oracle's entry state and its whole store stream (callees included) must equal the oracle's.

Reads ONLY work\\disc_us (SLUS_010.53), the snapshots under work\\oracle\\state and the captures
under work\\roadrt (all gitignored).  Writes ONLY to stdout.  No game bytes are copied anywhere.

Usage (from the project root):

    python tools\\scout\\roadrt.py capture            # print the two rrverify commands
    python tools\\scout\\roadrt.py verify             # the check (last line: roadrt: ...)
    python tools\\scout\\roadrt.py verify --coverage  # plus the per-branch coverage
    python tools\\scout\\roadrt.py verify --synthetic 3000 --seed 7
    python tools\\scout\\roadrt.py verify --mutate    # the negative control: every mutant must FAIL
    python tools\\scout\\roadrt.py verify --only b61c --verbose
"""

from __future__ import annotations

import argparse
import bisect
import json
import os
import random
import struct
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import exe as E  # noqa: E402  (sibling scout module; read-only use)
import roadq as Q  # noqa: E402  (PyCpu, the replay machine and the arithmetic helpers)

u, s, s16, mid, lohi = Q.u, Q.s, Q.s16, Q.mid, Q.lohi
Mismatch, CpuStop = Q.Mismatch, Q.CpuStop
REG = Q.REG

ROOT = E.ROOT
SLUS_SHA1 = "67ed165a2c517d4e6106fb0dfa324d66dd9a76f1"

CAPTURES = {
    # name: (snapshot dir, capture dir, rrverify arguments besides the probes)
    "quick": ("work\\oracle\\state\\quick", "work\\roadrt\\cap_quick", "--frames 30"),
    "race": ("work\\oracle\\state\\rr-race", "work\\roadrt\\cap_race", "--frames 36"),
}
SNAPSHOTS = ["quick", "rr-race", "rr-pack", "rr-grid", "retro-shell"]

# The roots whose whole resident callee tree is probed.
ROOTS = [0x800374D4, 0x80037338, 0x8003E150, 0x8003AE24, 0x8003B520, 0x8003DE28]
# The two writers of the scratchpad active-entity list: the reset in the step
# (a0 = 0x1F800004 stored to 0x1F800000) and the append in RASHCDG 0x8007F0BC's tail.
LIST_RESET, LIST_APPEND = 0x80078AC0, 0x8007FA1C
LIST_PROBES = [LIST_RESET, LIST_APPEND]

SCR = 0x1F800000
POOL0 = 0x8005B3A0
GAME_STATE = 0x8005B2F8
ROUTE = 0x800D6170          # +0x12 s16 route-record count (-1 = no route), +0x18 finish, +0x24 records
GRAPH = 0x8005B240

# key: (entry, frame size, returns a value)
FUNCS = {
    "374d4": (0x800374D4, 24, True), "37450": (0x80037450, 32, True),
    "37338": (0x80037338, 32, False), "701c": (0x8003701C, 40, False),
    "7104": (0x80037104, 64, False), "396a8": (0x800396A8, 88, False),
    "662c": (0x8003662C, 32, False), "e150": (0x8003E150, 32, False),
    "dff4": (0x8003DFF4, 72, False), "ddb0": (0x8003DDB0, 40, True),
    "de28": (0x8003DE28, 32, True), "ee68": (0x8003EE68, 56, True),
    "ef34": (0x8003EF34, 40, True), "e67c": (0x8003E67C, 0, True),
    "df54": (0x8003DF54, 32, False), "f1f0": (0x8003F1F0, 0, False),
    "ae24": (0x8003AE24, 32, False), "af9c": (0x8003AF9C, 24, False),
    "a9d8": (0x8003A9D8, 96, False), "dcb8": (0x8003DCB8, 16, True),
    "b520": (0x8003B520, 40, False), "b61c": (0x8003B61C, 80, True),
    "be1c": (0x8003BE1C, 56, True), "f408": (0x8003F408, 0, True),
}

# Every callee of the models, with the number of argument REGISTERS its callers set and it reads.
CALLEES = {
    0x800396A8: ("RoadReseat", 1), 0x8003701C: ("RoadTrack", 1), 0x80037104: ("RoadProbeAhead", 1),
    0x80037450: ("RoadRebindBody", 1), 0x8003DFF4: ("RoadClassify", 1),
    0x8003AF9C: ("RouteBind", 3), 0x8003B61C: ("ProgressScalar", 1), 0x8003A9D8: ("RoadsideZones", 1),
    0x8003B4B0: ("RouteFindLeg", 2), 0x8003B024: ("RouteBindFirst", 4),
    0x8003B1C4: ("RouteBindStep", 4), 0x8003F408: ("RouteRecord", 2),
    0x8003BE1C: ("TurnSubObject", 4), 0x80036B14: ("RoadSliceSearch", 3),
    0x800B6AAC: ("AiProject", 3), 0x80036800: ("RoadProject", 4), 0x8003662C: ("RoadPosition", 3),
    0x8002E698: ("DotLcm", 2), 0x800394F0: ("NextObjectMissing", 2), 0x8003DDB0: ("NodeZone", 1),
    0x8003DE28: ("RoadClass", 4), 0x8003DF54: ("RoadsideRun", 3), 0x8003EE68: ("RoadClassNode", 4),
    0x8003EF34: ("RoadCrossSection", 3), 0x8003E67C: ("RoadEdgeClass", 4),
    0x8001E0B4: ("memcpy", 3), 0x8001E100: ("MemSet32", 3), 0x8003DCB8: ("SegmentStraddle", 2),
    0x8001FC58: ("Rand", 0), 0x80039AFC: ("NodeRecord", 1), 0x8003EB58: ("NodeZoneTest", 4),
    0x80039B60: ("JunctionIndex", 1), 0x80039C90: ("FindRoadPiece", 2), 0x8003BFE8: ("RoadShortcut", 1),
    0x8003F204: ("RoadsideRunNode", 2), 0x8003F1F0: ("RoadsideRunOfSlice", 1),
    0x8003E754: ("RoadClassCore", 4), 0x8002EAD8: ("MulAdd", 4),
}

MUTANTS = {
    "e67c.side": "the edge class reads the right half of the cross-section for a negative lateral",
    "e67c.inner_le": "the tarmac test is |lat| <= |inner| instead of <",
    "ef34.no_clamp": "the along-distance is never clamped to the first / last slice",
    "ef34.keep_prev": "the previous XSIH record is reused without its +4 >= 0 test",
    "de28.clear_sign": "the open-road arm clears +0x178 when its +4 is >= 0, like the node arm",
    "dff4.no_adopt": "RoadClassify never adopts the cursor the node arm found",
    "b61c.no_shift": "the node arm adds the along distance without >> 4",
    "b61c.raw_return": "a missing route object returns +0x170 >> 4 instead of +0x170",
    "b61c.no_clamp": "a negative distance is not clamped to 0",
    "b520.forward": "the marker bit is set when the distance crosses a threshold UPWARD",
    "ae24.sign": "the wrong-way bit is set when the leg and bike directions AGREE",
    "a9d8.speed_shift": "the flicker period is indexed by speed >> 19, not >> 20",
    "a9d8.no_break": "the zone scan does not stop at the first zone that sets bits 5..6",
    "dcb8.use_y": "the straddle test uses Y where the original uses Z",
    "701c.always": "the +0x16C +-5 rewrite happens even when the sub-object tag is unchanged",
    "662c.no_reverse": "the reversal +-1 is not added to +0x16C",
    "396a8.last_slice": "a re-seat through an arm with dir > 0 takes the LAST slice",
    "37338.bit7": "the pass sets +0x184 bit 7 when +0x33C is set, not when it is zero",
    "374d4.bit5": "the re-bind sets +0x184 bit 5 when flag == 0",
    "f408.no_self": "RouteRecord does not return the object itself when its key matches",
    "be1c.dir_sign": "TurnSubObject does not negate the GPDT direction for a PDT_ dir <= 0",
    "7104.base": "the probe point is built from +0x1F8 in the crash case",
}
MUT: set[str] = set()


# ---------------------------------------------------------------------------
# Static structure
# ---------------------------------------------------------------------------

# The one indirect jump in the closure: 0x8003A9D8's zone dispatch `jr v0` at 0x8003AB78 through
# the 8-entry table at 0x80010E14 (index = zone kind - 1, bounded by `sltiu v0,v1,8`).
JUMP_TABLES = {0x8003AB78: (0x80010E14, 8)}


def cfg(img: E.Image, entry: int):
    """Instructions reachable from `entry` inside one function: (pcs, calls, jalr pcs, jr-ra pcs)."""
    seen, work, calls, jalr, rets = set(), [entry], [], [], []
    while work:
        pc = work.pop()
        while pc not in seen:
            seen.add(pc)
            w = img.word(pc)
            op, fn, rs, rt = w >> 26, w & 63, (w >> 21) & 31, (w >> 16) & 31
            simm = (w & 0xFFFF) - (0x10000 if w & 0x8000 else 0)
            term = False
            if op == 0 and fn == 8:
                seen.add(pc + 4)
                if rs == 31:
                    rets.append(pc)
                elif pc in JUMP_TABLES:
                    tb, n = JUMP_TABLES[pc]
                    work.extend(img.word(tb + 4 * i) for i in range(n))
                else:
                    raise ValueError(f"unmodelled indirect jump at 0x{pc:08X}")
                term = True
            elif op == 0 and fn == 9:
                jalr.append(pc)
            elif op == 3:
                calls.append((pc, ((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)))
            elif op == 2:
                seen.add(pc + 4)
                work.append(((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2))
                term = True
            elif op in (4, 5, 6, 7) or (op == 1 and rt in (0, 1)):
                work.append(pc + 4 + (simm << 2))
                if op == 4 and rs == 0 and rt == 0:
                    seen.add(pc + 4)
                    term = True
            if term:
                break
            pc += 4
    return sorted(seen), calls, jalr, rets


def closure(img: E.Image, roots):
    out, work = {}, list(roots)
    while work:
        f = work.pop()
        if f in out or not img.contains(f):
            continue
        out[f] = cfg(img, f)
        work.extend(t for _, t in out[f][1])
    return out


def bodies(img: E.Image) -> dict:
    """key -> (entry, end-exclusive, jr-ra pcs), the extent from our own CFG walk."""
    out = {}
    for k, (entry, _, _) in FUNCS.items():
        pcs, _, _, rets = cfg(img, entry)
        out[k] = (entry, pcs[-1] + 4, rets)
    return out


def probe_set(img: E.Image) -> set:
    cl = closure(img, ROOTS)
    ps = set(LIST_PROBES)
    for f, (pcs, calls, jalr, rets) in cl.items():
        ps.add(f)
        ps.update(rets)
        for pc, t in calls:
            ps.add(pc + 8)
            ps.add(t)
        for pc in jalr:
            ps.add(pc + 8)
    return ps


# ---------------------------------------------------------------------------
# The capture
# ---------------------------------------------------------------------------

class Capture:
    def __init__(self, name: str, bods: dict, verbose=False):
        snap, cap, _ = CAPTURES[name]
        self.name = name
        self.ram0 = open(os.path.join(ROOT, snap, "ram.bin"), "rb").read()
        self.scr0 = open(os.path.join(ROOT, snap, "scratchpad.bin"), "rb").read()
        d = os.path.join(ROOT, cap)
        ranges = sorted((b[0], b[1]) for b in bods.values())
        lo_all, hi_all = ranges[0][0], max(r[1] for r in ranges)
        starts = [r[0] for r in ranges]

        def in_body(p):
            i = bisect.bisect_right(starts, p) - 1
            return i >= 0 and p < ranges[i][1]
        self.wseq, self.wpc, self.waddr, self.wsize, self.wval = [], [], [], [], []
        self.body_reads = []
        with open(os.path.join(d, "watch.csv"), "r") as f:
            f.readline()
            for line in f:
                seq, pc, kind, _, addr, size, val = line.split(",")
                if kind == "write":
                    self.wseq.append(int(seq))
                    self.wpc.append(int(pc, 16))
                    self.waddr.append(int(addr, 16) & 0x1FFFFF)
                    self.wsize.append(int(size))
                    self.wval.append(int(val, 16))
                else:
                    p = int(pc, 16)
                    if lo_all <= p < hi_all and in_body(p):
                        self.body_reads.append((int(seq), p, int(addr, 16) & 0x1FFFFF,
                                                int(size), int(val, 16)))
        self.rseq = [r[0] for r in self.body_reads]
        raw = open(os.path.join(d, "calls.bin"), "rb").read()
        self.calls = [struct.unpack_from("<QIIIIIII", raw, o) for o in range(0, len(raw), 40)]
        self.cseq = [c[0] for c in self.calls]
        self.probes = defaultdict(list)
        with open(os.path.join(d, "probes.csv"), "r") as f:
            f.readline()
            for line in f:
                p = line.rstrip("\n").split(",")
                regs = {REG[i]: int(p[3 + i], 16) for i in range(32)}
                self.probes[int(p[1], 16)].append((int(p[0]), regs))
        for v in self.probes.values():
            v.sort(key=lambda t: t[0])
        # the scratchpad list writes, from the two writer probes
        ev = []
        for seq, r in self.probes.get(LIST_RESET, []):
            ev.append((seq, 0, r["a0"]))
        for seq, r in self.probes.get(LIST_APPEND, []):
            ev.append((seq, r["v0"] & 0x3FF, r["s1"]))
            ev.append((seq, 0, u(r["v0"] + 4)))
        ev.sort(key=lambda t: t[0])
        self.scr_ev = ev
        self.scr_seq = [e[0] for e in ev]

    def probe_after(self, pc: int, seq: int):
        lst = self.probes.get(pc, [])
        i = bisect.bisect_right([t[0] for t in lst], seq)
        return lst[i] if i < len(lst) else None

    def scratch_at(self, seq: int) -> bytearray:
        b = bytearray(self.scr0)
        for sq, off, v in self.scr_ev[:bisect.bisect_left(self.scr_seq, seq)]:
            b[off:off + 4] = u(v).to_bytes(4, "little")
        return b


class Machine(Q.Machine):
    """roadq's replay machine, with the scratchpad and this probe's callee table."""

    def __init__(self, cap, img, scr, body, entry_seq, exit_seq, regs, cov):
        super().__init__(cap, img, body, entry_seq, exit_seq, regs, cov)
        self.scr = scr

    def _rd(self, a: int, n: int) -> int:
        a = u(a)
        if (a & 0x1FFFFFFF) >= 0x1F800000:
            o = a & 0x3FF
            return int.from_bytes(self.scr[o:o + n], "little")
        return super()._rd(a, n)

    def _wr(self, a: int, n: int, v: int):
        if (u(a) & 0x1FFFFFFF) >= 0x1F800000:
            raise Mismatch(f"model stores to the scratchpad 0x{u(a):08X}")
        super()._wr(a, n, v)

    def call(self, target: int, *args) -> int:
        name, arity = CALLEES[target]
        if self.ci >= len(self.calls):
            raise Mismatch(f"model calls {name} but the original made no further call")
        ev = self.calls[self.ci]
        self.ci += 1
        seq, frm, tgt = ev[0], ev[1], ev[2]
        if tgt != target:
            raise Mismatch(f"call #{self.ci}: model {name} 0x{target:08X}, original 0x{tgt:08X} "
                           f"from 0x{frm:08X}")
        ent = self.cap.probe_after(target, seq)
        if ent is None:
            raise Mismatch(f"no entry probe for 0x{target:08X}")
        for i in range(arity):
            got, want = u(args[i]), ent[1]["a%d" % i]
            if got != want:
                raise Mismatch(f"call #{self.ci} {name} from 0x{frm:08X}: a{i} model 0x{got:08X} "
                               f"original 0x{want:08X}")
        ret = self.cap.probe_after(frm + 8, seq)
        if ret is None:
            raise Mismatch(f"no return probe at 0x{frm + 8:08X}")
        cap = self.cap
        while self.wi < len(cap.wseq) and cap.wseq[self.wi] < ret[0]:
            j = self.wi
            self.wi += 1
            if self.body[0] <= cap.wpc[j] < self.body[1]:
                continue
            n, o = cap.wsize[j], cap.waddr[j]
            if o >= 0x200000:
                continue
            self.b[o:o + n] = (cap.wval[j] & ((1 << (8 * n)) - 1)).to_bytes(n, "little")
        return ret[1]["v0"]


def absv(x):
    """(x + (x >> 31)) ^ (x >> 31) with an arithmetic shift: |INT32_MIN| stays INT32_MIN."""
    x = s(x)
    m = x >> 31
    return s(u(x + m) ^ u(m))


def slt(a, b):
    return 1 if s(a) < s(b) else 0


def sltu(a, b):
    return 1 if u(a) < u(b) else 0


def sra(x, n):
    return u(s(x) >> n)


# ---------------------------------------------------------------------------
# The models.  Each takes the machine `m` and returns v0 (or None).  `m.c(label)` marks a branch.
# ---------------------------------------------------------------------------

def list_walk(m, body):
    """The pass loop shared by 0x80037338 / 0x8003E150 / 0x8003AE24 / 0x8003B520:
    for (p = 0x1F800004; p < *(0x1F800000); p += 4) body(*p)."""
    s1 = SCR + 4
    end = m.lw(SCR)
    s0 = m.lw(s1)
    while sltu(s1, end):
        body(s0)
        s1 += 4
        end = m.lw(SCR)
        s0 = m.lw(s1)


def m_37338(m):
    def body(e):
        m.c("entity")
        v0 = m.lw(e + 828)
        if ("37338.bit7" in MUT) != (v0 == 0):
            m.c("tag not latched: set bit 7")
            m.sw(e + 388, m.lw(e + 388) | 0x80)
            m.sw(e + 564, m.lw(e + 564) | 0x04000000)
        else:
            m.c("tag latched: clear bit 7")
            v0 = m.lw(e + 388)
            v1 = m.lw(e + 564)
            m.sw(e + 388, v0 & u(~0x80))
            m.sw(e + 564, v1 & 0xFBFFFFFF)
        if m.lw(e + 560) & 0x08000000:
            m.c("no re-seat")
            m.sw(e + 388, m.lw(e + 388) & u(~0x20))
        else:
            m.c("re-seat")
            m.call(0x800396A8, e)
            m.sw(e + 388, m.lw(e + 388) | 0x20)
        m.call(0x8003701C, e)
        if m.lw(e + 564) & 0x40000:
            m.c("direction from the contact")
            m.sw(e + 364, m.lw(m.lw(e + 832) + 192))
        m.call(0x80037104, e)
    list_walk(m, body)


def m_374d4(m):
    R = m.R
    s0 = R["a0"]
    if ("374d4.bit5" in MUT) != (R["a1"] != 0):
        m.c("flag: re-seat")
        m.call(0x800396A8, s0)
        v0 = m.lw(s0 + 388) | 0x20
    else:
        m.c("no flag")
        v0 = m.lw(s0 + 388) & u(~0x20)
    m.sw(s0 + 388, v0)
    return m.call(0x80037450, s0)


def m_37450(m):
    s1 = m.R["a0"]
    m.call(0x8003701C, s1)
    if (m.lhu(s1 + 172) >> 5) == 0:
        m.c("pool 0: probe ahead")
        m.call(0x80037104, s1)
    m.call(0x8003DFF4, s1)
    m.call(0x8003AF9C, s1 + 172, 1, 0)
    v0 = m.call(0x8003B61C, s1 + 172)
    m.sw(s1 + 324, v0)                              # the delay slot of the next jal
    m.call(0x8003A9D8, s1)
    return (m.lw(s1 + 388) >> 6) & 1


def m_701c(m):
    s1 = m.R["a0"]
    v0 = m.lw(s1 + 336)
    s3 = m.lw(s1 + 340)
    s0 = m.lhu(v0 + 6)
    s2 = m.call(0x80036B14, s1 + 172, s1 + 328, s1 + 184)
    v0 = m.lw(s1 + 336)
    v1 = m.lw(s1 + 364)
    s0 = s16(s0)
    v0 = m.lh(v0 + 6)
    a1 = u(-1) if (v0 == s0 and "701c.always" not in MUT) else 0
    if s(v1) > 0:
        a0, v0 = 5, u(v1 - 5)
    else:
        a0, v0 = u(-5), u(v1 + 5)
    if a1:
        m.c("same sub-object tag")
    else:
        m.c("new tag: +-5")
    m.sw(s1 + 364, u(a0 + (a1 & v0)))
    m.call(0x80036800, s1 + 184, s2, s1 + 344, s1 + 348)
    m.call(0x8003662C, s1 + 450, s1 + 328, s1 + 360)
    v0 = m.lw(s1 + 388)
    m.sw(s1 + 388, (v0 & u(~0x40)) if s2 == s3 else (v0 | 0x40))


def m_7104(m):
    s2 = m.R["a0"]
    fr = u(m.R["sp"] - 64)
    m.sw(fr + 16, m.lw(s2 + 328))
    m.sw(fr + 20, m.lw(s2 + 332))
    m.sw(fr + 24, m.lw(s2 + 336))
    v0 = m.lw(s2 + 340)
    m.sw(fr + 40, 0)
    m.sw(fr + 44, 0)
    m.sw(fr + 32, 0)
    m.sw(fr + 36, 0)
    m.sw(fr + 28, v0)
    crash = 1 if (m.lw(s2 + 568) & 0x600) else 0
    a3 = s2 + 528
    if crash != ("7104.base" in MUT):
        m.c("crashing: from the box centre, one half length")
        v0 = m.lh(s2 + 528)
        a1 = m.lw(s2 + 308)
        a0 = m.lw(s2 + 184)
        m.sw(s2 + 244, u(mid(u(v0 << 4), a1) + a0))
        a2 = s2 + 184
    else:
        m.c("riding: from the contact point, two half lengths")
        a1 = u(m.lw(s2 + 308) << 1)
        v0 = m.lh(s2 + 528)
        a0 = m.lw(s2 + 504)
        m.sw(s2 + 244, u(mid(u(v0 << 4), a1) + a0))
        a2 = s2 + 504
    v0 = m.lh(a3 + 2)
    a0 = m.lw(a2 + 4)
    m.sw(s2 + 248, u(mid(u(v0 << 4), a1) + a0))
    v0 = m.lh(a3 + 4)
    a0 = m.lw(a2 + 8)
    m.sw(s2 + 252, u(mid(u(v0 << 4), a1) + a0))
    m.call(0x80036B14, s2 + 172, fr + 16, s2 + 244)
    v0 = m.lw(fr + 28)
    a0 = m.lw(s2 + 856)
    m.sw(s2 + 256, v0)
    if a0 == 0:
        return
    if m.lw(s2 + 1088) == 0:
        return
    m.c("passenger")
    s1 = a0
    if crash:
        a0 = s2 + 184
    else:
        m.call(0x8002EAD8, s2 + 504, s2 + 528, m.lw(s2 + 308), s1 + 504)
        a0 = s1 + 504
    v0 = m.lw(s2 + 304)
    a2 = m.lw(s1 + 304)
    m.call(0x8002EAD8, a0, s2 + 516, u(v0 + a2), s1 + 184)
    if m.lw(s2 + 560) & 0x08000000:
        v0 = m.lw(s1 + 388) & u(~0x20)
    else:
        m.call(0x800396A8, s1)
        v0 = m.lw(s1 + 388) | 0x20
    m.sw(s1 + 388, v0)
    m.call(0x8003701C, s1)


def m_396a8(m):
    s3 = m.R["a0"]
    fr = u(m.R["sp"] - 88)
    v0 = m.lw(s3 + 336)
    v1 = m.lhu(s3 + 362)
    s6 = m.lhu(v0 + 6)
    s4 = s3 + 328

    def tail():
        a1 = 0
        v1 = m.lw(s3 + 336)
        v0 = s16(s6)
        v1 = m.lh(v1 + 6)
        a0 = m.lw(s3 + 364)
        if v1 == v0:
            a1 = u(-1)
        if s(a0) > 0:
            vv, v0 = 5, u(a0 - 5)
        else:
            vv, v0 = u(-5), u(a0 + 5)
        m.sw(s3 + 364, u(vv + (a1 & v0)))

    if v1 == 0:
        m.c("open road: RoadShortcut")
        m.call(0x8003BFE8, s3)
        return tail()
    s1 = m.lw(s4 + 4)
    v0 = m.lh(s1 + 2)
    s5 = m.lw(s3 + 328)
    if v0 != 1:
        m.c("node, not in the core")
        return tail()
    if m.lh(s1) != 0:
        m.c("core, flags set")
        return tail()
    t1 = m.call(0x80039AFC, m.lw(s5))
    if t1 == 0:
        m.c("no node record")
        return
    a3 = 0xFFFF0000
    s0 = 0
    n = m.lh(t1 + 2)
    t0 = 0
    a2 = t1 + 8
    best = (0, 0, 0)
    while t0 < n:
        a0 = u(m.lw(s3 + 184) - m.lw(a2 + 12))
        m.sw(fr + 16, a0)
        v0 = u(m.lw(s3 + 188) - m.lw(a2 + 16))
        m.sw(fr + 20, v0)
        a1 = u(m.lw(s3 + 192) - m.lw(a2 + 20))
        far = 0x5A8000 < absv(a0)
        m.sw(fr + 24, a1)
        if far or 0x5A8000 < absv(a1):
            metric = 0x7FFF0000
        else:
            ax = absv(u(m.lw(s3 + 184) - m.lw(a2 + 12)))
            az = absv(u(m.lw(s3 + 192) - m.lw(a2 + 20)))
            lo = ax if ax < az else az
            half = s(u(lo + (u(lo) >> 31))) >> 1
            metric = u(ax + az - half)
        if a3 == 0xFFFF0000 or s(metric) < s(a3):
            s0 = a2
            best = (m.lw(fr + 16), m.lw(fr + 20), m.lw(fr + 24))
            a3 = metric
            m.sw(fr + 32, best[0])
            m.sw(fr + 36, best[1])
            m.sw(fr + 40, best[2])
        n = m.lh(t1 + 2)
        t0 += 1
        a2 += 24
    if s0 == 0:
        m.c("no arm")
        return
    A = (m.lw(fr + 32), u(m.lh(s0 + 6) << 4))
    B = (m.lw(fr + 36), u(m.lh(s0 + 8) << 4))
    C = (m.lw(fr + 40), u(m.lh(s0 + 10) << 4))
    lo, hi = lohi(*C)
    m.sw(fr + 48, lo)
    m.sw(fr + 52, hi)
    dot = s(u(mid(*C) + u(mid(*B) + mid(*A))))
    if not dot < -131:
        m.c("still inside the core")
        return tail()
    m.c("left through an arm")
    s1 = m.call(0x80039C90, s5, m.lh(s0 + 4))
    if s1 == 0:
        m.c("arm road has no piece")
        return
    a0 = m.lw(s5 + 52)
    s2 = u(m.lw(s5 + 48) + 28 * m.lh(s1 + 20))
    first = m.lh(s0 + 2) > 0
    if "396a8.last_slice" in MUT:
        first = not first
    v1 = m.lh(s2 + 8)
    idx = v1 if first else v1 - 1 + m.lh(s2 + 10)
    s0 = u(a0 + 52 * idx)
    m.call(0x80036800, s3 + 184, s0, s4 + 16, s4 + 20)
    m.sw(s4 + 4, s1)
    m.sw(s4 + 8, s2)
    m.sw(s4 + 12, s0)
    m.sw(s4 + 24, 0)
    m.sw(s4 + 28, 0)
    return tail()


def m_662c(m):
    R = m.R
    s0, s1 = R["a1"], R["a2"]
    v1 = m.lw(s0 + 4)
    v0 = m.lhu(v1 + 2)
    v1 = m.lhu(v1 + 12)
    m.sw(s1, u((v0 << 16) | v1))
    a1 = m.lw(s0 + 12)
    dot = m.call(0x8002E698, R["a0"], a1 + 14)
    a0 = m.lw(s1 + 4)
    rev = 0 if "662c.no_reverse" in MUT else 1
    if s(dot) >= 0:
        m.c("along the tangent")
        a1 = 3 if absv(a0) == 5 else 1
        v0 = u(a1 + (rev if s(a0) < 1 else 0))
    else:
        m.c("against the tangent")
        a1 = u(-3) if absv(a0) == 5 else u(-1)
        v0 = u(a1 - (rev if s(a0) >= 0 else 0))
    m.sw(s1 + 4, v0)
    v0 = m.lw(s0 + 12)
    a1 = u(m.lw(v0 + 40) + m.lw(s0 + 20))
    m.sw(s1 + 8, a1)
    if m.lh(m.lw(s0 + 4) + 2) != 0:
        m.c("junction core piece")
        return
    a0 = m.lw(s0)
    if m.lh(a0 + 16) == 1 and m.lw(a0 + 12) == 0:
        m.c("stub of a junction core object")
        v1 = a1 if s(a1) >= 0 else 0
        m.sw(s1 + 8, v1)
        a0 = m.lw(s0 + 4)
        if m.lw(a0 + 24) != 0:
            a0 = m.lw(a0 + 28)
            if s(a0) < s(v1):
                m.sw(s1 + 8, a0)
    v1 = m.lh(m.lw(s0 + 12))
    if v1 != 0 and v1 != m.lh(m.lw(s0 + 8) + 10) - 1:
        return
    m.c("sub-object end")
    if m.call(0x800394F0, s0, m.lw(s1 + 4)) == 0:
        return
    m.c("next object missing: clamp")
    a0 = m.lw(m.lw(s0 + 4) + 28)
    v1 = m.lw(s1 + 8)
    if s(a0) < s(v1):
        v1 = a0
    m.sw(s1 + 8, v1)
    m.sw(s1 + 8, v1 if s(v1) >= 0 else 0)


def m_e150(m):
    def body(e):
        m.c("entity")
        m.call(0x8003DFF4, e)
        a0 = m.lw(e + 856)
        if a0:
            m.c("passenger")
            m.call(0x8003DFF4, a0)
        if m.lw(e + 568) & 0x800:
            m.c("flagsC bit 11 -> bit 6")
            m.sw(e + 388, m.lw(e + 388) | 0x40)
    list_walk(m, body)


def m_dff4(m):
    s1 = m.R["a0"]
    fr = u(m.R["sp"] - 72)
    s3 = m.call(0x8003DDB0, s1)
    v0 = m.call(0x8003DE28, s1, 0, fr + 16, s3)
    s2 = v0 & 0xFF
    if (m.lhu(s1 + 172) >> 5) < 2:
        v1 = m.lw(s1 + 388)
        go = True
        if (v1 & 0x20) == 0:
            if m.lw(s1 + 372) == 0 or (v1 & 1) == 0:
                go = False
        if go and m.lhu(s1 + 362) == 1:
            a0 = m.lh(m.lw(s1 + 332) + 2)
            if a0 == 1 and m.lw(fr + 28) != 0 and m.lw(s1 + 336) != m.lw(fr + 24):
                v0 = m.lw(fr + 20)
                if v0 != 0 and m.lh(v0 + 2) == a0 and "dff4.no_adopt" not in MUT:
                    m.c("adopt the node cursor")
                    m.call(0x8001E0B4, s1 + 328, fr + 16, 32)
                    m.call(0x8003662C, s1 + 450, s1 + 328, s1 + 360)
    # Redundant in effect: bit 0 can only be set here together with +0x174, and then RoadClass
    # has already returned 1; bit 6 set means OR-ing it again changes nothing.  A mutant that drops
    # this test survives every case, so it is not in MUTANTS.
    if m.lw(s1 + 388) & 0x41:
        s2 = 1
    m.call(0x8003DF54, s1, 0, s3)
    v1 = m.lw(s1 + 388)
    m.sw(s1 + 388, v1 | (u(-s2) & 0x40))


def m_ddb0(m):
    s1 = m.R["a0"]
    fr = u(m.R["sp"] - 40)
    if s1 == 0:
        return u(-1)
    if m.lhu(s1 + 362) != 1:
        m.c("not in a node")
        return u(-1)
    v0 = m.call(0x80039AFC, m.lw(m.lw(s1 + 328)))
    if v0 == 0:
        return u(-1)
    m.c("in a node")
    m.sw(fr + 16, u(-1))
    return m.call(0x8003EB58, s1 + 184, v0, 0, 0)


def m_de28(m):
    R = m.R
    s0, s1 = R["a0"], R["a2"]
    s2 = 0
    if m.lw(s0 + 372):
        s2 = m.lw(s0 + 388) & 1
    if R["a1"] == 1:
        m.c("mode 1: reset")
        for o in (372, 376, 380, 384):
            m.sw(s0 + o, 0)
    if m.lhu(s0 + 362) == 1:
        m.c("in a node")
        v0 = m.lw(s0 + 376)
        if v0 and m.lh(v0 + 4) >= 0:
            m.sw(s0 + 376, 0)
        v1 = m.call(0x8003EE68, s0, s0 + 372, s1, R["a3"])
    else:
        m.c("on a road")
        v0 = m.lw(s0 + 376)
        if v0:
            neg = m.lh(v0 + 4) < 0
            if neg != ("de28.clear_sign" in MUT):
                m.sw(s0 + 376, 0)
        v1 = m.call(0x8003EF34, s0 + 328, s0 + 360, s0 + 372)
        if s1:
            m.sw(s1 + 12, 0)
    if m.lw(s0 + 372):
        if (m.lw(s0 + 388) & 1) or s2:
            m.c("off the road now or before: 1")
            v1 = 1
    return v1 & 0xFF


def m_ee68(m):
    R = m.R
    s0, s2, s3, s5 = R["a0"], R["a1"], R["a2"], R["a3"]
    fr = u(R["sp"] - 56)
    s4 = 0
    v0 = m.lh(m.lw(s0 + 332) + 2)
    a0 = m.lw(s0 + 328)
    s1 = 0
    if v0 == 1:
        node = m.call(0x80039AFC, m.lw(a0))
        if node:
            m.c("core: RoadClassCore")
            m.sw(fr + 16, s5)
            s4 = m.call(0x8003E754, s0, node, s2, s3)
            s1 = m.lw(s2)
    if s1 == 0:
        m.c("no cross-section")
        v0 = m.lw(s2 + 16)
        m.sw(s2, 0)
        m.sw(s2 + 16, v0 & u(-16))
        if s3:
            m.sw(s3 + 12, 0)
    return s4 & 0xFF


def m_ef34(m):
    R = m.R
    a0, a1, s2 = R["a0"], R["a1"], R["a2"]
    t0 = t4 = s1 = s3 = 0
    t2 = m.lw(a0)
    v0 = m.lw(a0 + 4)
    t1 = m.lw(a0 + 8)
    v0 = m.lh(v0 + 2)
    a2 = m.lw(a0 + 12)
    if v0 == 0:
        t0 = m.lh(t1 + 16)
        if t0 < 0:
            t0 = 0
    s0 = m.lw(s2 + 4)
    v1 = m.lh(t1 + 8) + m.lh(t1 + 10)
    a3 = m.lw(a1 + 8)
    last = u(m.lw(t2 + 52) + 52 * v1 - 52)
    idx = m.lh(a2)
    t3 = m.lw(a0 + 16)
    clamp = False
    if idx == 0 and s(a3) < s(m.lw(a2 + 40)):
        clamp = True
    elif a2 == last:
        if s(u(m.lw(a2 + 40) + m.lw(a2 + 32))) < s(a3):
            clamp = True
    if clamp:
        m.c("along clamped to the slice")
        lo = m.lw(a2 + 40)
        hi = u(lo + m.lw(a2 + 32))
        a3 = m.lw(a1 + 8)
        if "ef34.no_clamp" not in MUT:
            # upper bound first; the order only matters for a negative chord, which the shipped
            # data never has (a mutant swapping it survives every case - it is equivalent)
            if s(hi) < s(a3):
                a3 = hi
            if not s(lo) < s(a3):
                a3 = lo
    if t0 == 0 or m.lw(s2) == 0:
        s0 = 0
    if s0:
        v0 = m.lw(s0 + 8)
        al = m.lw(a1 + 8)
        if s(al) < s(v0) or s(m.lw(s0 + 12)) < s(al):
            s0 = 0
        elif "ef34.keep_prev" not in MUT and m.lh(s0 + 4) < 0:
            s0 = 0
        else:
            m.c("previous XSIH reused")
            s1 = m.lw(s2)
    if t0 > 0 and s0 == 0:
        m.c("XSIH search")
        a2 = s(a3) >> 16
        first = m.lh(t1 + 18)
        base = m.lw(t2 + 56)
        for i in range(t0):
            rec = u(base + 16 * (first + i))
            if a2 < m.lh(rec + 10) or m.lh(rec + 14) < a2:
                continue
            s0 = rec
            g = m.lh(s0 + 4)
            s1 = 0
            if g >= 0:
                s1 = u(m.lw(t2 + 60) + 264 * g)
            else:
                m.c("XSIH without a group")
            break
        else:
            m.c("no XSIH covers it")
        s3 = 1
    if s1:
        m.sw(s2 + 52, m.lw(s1 + 4))
        m.sw(s2 + 40, m.lw(s1 + 144))
        m.sh(s2 + 48, m.lhu(s1 + 140))
        m.sw(s2 + 28, m.lw(s1 + 16))
        m.sh(s2 + 36, m.lhu(s1 + 12))
        side = 1 if s(t3) < 0 else 2
        t4 = m.call(0x8003E67C, s1, t3, side, s2 + 20)
    v1 = m.lw(s2 + 16)
    m.sw(s2, s1)
    m.sw(s2 + 4, s0)
    m.sw(s2 + 8, 0)
    m.sw(s2 + 12, 0)
    m.sw(s2 + 16, (v1 & u(-16)) | t4)
    return s3


def m_e67c(m):
    R = m.R
    a1 = absv(R["a1"])
    side2 = R["a2"] == 2
    if "e67c.side" in MUT and s(R["a1"]) < 0:
        side2 = not side2
    sec = u(R["a0"] + (136 if side2 else 8))
    a3 = R["a3"]
    t1 = m.lw(sec + 8)
    v0 = m.lh(sec + 6)
    a2 = m.lw(sec + 72)
    t0 = t1
    if v0 > 0 and a1 < absv(a2):
        m.c("class 5")
        v1 = m.lhu(sec + 82)
        m.sh(a3, 5)
        m.sh(a3 + 2, v1)
        return 0
    if m.lh(sec + 2) > 0:
        t0 = m.lw(sec + 24)
    inner = a1 <= absv(t0) if "e67c.inner_le" in MUT else a1 < absv(t0)
    if inner:
        m.c("tarmac")
        m.sh(a3 + 2, 1)
        m.sh(a3, 4)
        return 0
    if a1 < absv(t1):
        m.c("shoulder")
        v1 = m.lhu(sec + 18)
        m.sh(a3, 1)
        m.sh(a3 + 2, v1)
        return 0
    m.c("off the road")
    m.sh(a3 + 2, 0)
    m.sh(a3, 0)
    return 1


def m_df54(m):
    R = m.R
    s0, s2 = R["a0"], R["a2"]
    if R["a1"] == 1:
        m.c("mode 1: clear")
        m.call(0x8001E100, s0 + 492, 0, 12)
    if m.lhu(s0 + 362) == 1:
        m.c("in a node")
        v1 = m.lw(s0 + 496)
        m.sw(s0 + 492, 0)
        if v1 == 0xFFFFFFFF:
            m.sw(s0 + 496, 0)
        m.call(0x8003F204, s0, s2)
    else:
        m.c("on a road")
        v1 = m.lw(s0 + 492)
        m.sw(s0 + 496, 0)
        if v1 == 0xFFFFFFFF:
            m.sw(s0 + 492, 0)
        m.call(0x8003F1F0, s0)


def m_f1f0(m):
    a0 = m.R["a0"]
    m.sw(a0 + 492, u(m.lw(a0 + 340) + 44))


def m_ae24(m):
    def body(e):
        m.c("entity")
        m.call(0x8003AF9C, e + 172, 1, 0)
        v1 = m.lw(e + 856)
        if v1:
            m.sw(v1 + 428, m.lw(e + 428))
        v1 = m.lw(e + 1084)
        m.sb(v1, m.lbu(v1) & 0x7F)
        if m.lh(ROUTE + 18) == -1:
            m.c("no route")
        elif m.lw(m.lw(e + 852) + 604) < 3:
            gs = m.lw(GAME_STATE)
            h = m.lhu(e + 172)
            ok = h < m.lw(gs + 48) or (m.lbu(m.lw(e + 1084) + 1) & 0xF) != 2
            if ok:
                a1 = m.lw(e + 360)
                if (a1 >> 16) == 0:
                    leg = m.call(0x8003B4B0, m.lw(e + 428), a1 & 0xFFFF)
                    if leg:
                        x = m.lw(leg + 4) ^ m.lw(e + 364)
                        if (s(x) < 0) != ("ae24.sign" in MUT):
                            m.c("wrong way")
                            v1 = m.lw(e + 1084)
                            m.sb(v1, m.lbu(v1) | 0x80)
                        else:
                            m.c("right way")
        m.call(0x8003A9D8, e)
    list_walk(m, body)


def m_af9c(m):
    R = m.R
    a0, a1, a3 = R["a0"], R["a1"], R["a2"]
    if a0 == 0:
        return
    h = m.lhu(a0)
    t0 = h >> 5
    a2 = 0
    if t0 == 0:
        a2 = u(m.lw(POOL0) + 1096 * h)
    if m.lw(a0 + 256) == 0 or a1 == 0:
        m.c("first bind")
        m.call(0x8003B024, a0, a2, t0, a3)
    else:
        m.c("step")
        m.call(0x8003B1C4, a0, t0, a2, a3)


def m_a9d8(m):
    R = m.R
    s1 = R["a0"]
    fr = u(R["sp"] - 96)
    tbl = 0x80010DFC
    a = [m.lw(tbl), m.lw(tbl + 4), m.lw(tbl + 8)]
    for k in range(3):
        m.sw(fr + 16 + 4 * k, a[k])
    a = [m.lw(tbl + 12), m.lw(tbl + 16), m.lw(tbl + 20)]
    for k in range(3):
        m.sw(fr + 28 + 4 * k, a[k])
    v0 = m.lw(s1 + 36)
    a0 = m.lw(s1 + 496)
    s7 = (v0 >> 5) & 3
    m.sw(s1 + 36, v0 & u(~0x60) & u(~0x100) & u(~0x200))
    a1 = 0
    if a0:
        m.c("node zone list")
        s6 = a0
        m.sw(fr + 40, 1)
    else:
        v1 = m.lw(s1 + 492)
        if v1 == 0:
            m.c("no roadside run")
            return
        a1 = v1 if s(m.lw(s1 + 344)) < 0 else u(v1 + 2)
        m.sw(fr + 40, m.lbu(a1 + 1))
        s6 = 0
    run = m.lw(fr + 40)
    if s(run) > 0:
        if s6 == 0 and a1 != 0:
            s6 = u(m.lw(m.lw(s1 + 328) + 100) + 4 * m.lbu(a1))
        n = m.lw(fr + 40)
        s3 = 0
        while n != 0:                                      # the outer loop, one BZDT entry each
            first = m.lh(s6)
            s4 = u(m.lw(m.lw(s1 + 328) + 96) + 40 * first)
            cnt = m.lbu(s6 + 3)
            s5 = 0
            while cnt:
                s2 = s4 + 2
                m.sw(fr + 48, u(-97))
                hit = m.call(0x8003DCB8, s1, s4)
                a2 = m.lw(fr + 48)
                if hit:
                    kind = m.lhu(s2) & 0xF
                    if 1 <= kind <= 8:
                        m.lw(0x80010E14 + 4 * (kind - 1))          # the jump table word
                        zone(m, s1, s2, kind, s7, fr, a2)
                s4 += 40
                if (m.lw(s1 + 36) >> 5) & 3 and "a9d8.no_break" not in MUT:
                    m.c("stop: bits 5..6 set")
                    break
                s5 += 1
                if not s5 < m.lbu(s6 + 3):
                    break
            s6 += 4
            if (m.lw(s1 + 36) >> 5) & 3 and "a9d8.no_break" not in MUT:
                break
            n = m.lw(fr + 40)
            s3 += 1
            if not s3 < s(n):
                break
    for k in range(2):
        a0 = m.lw(s1 + 56 + 8 * k)
        while a0:
            m.c("propagate to a linked object")
            v0 = m.lw(a0 + 36)
            v1 = m.lw(s1 + 36)
            m.sw(a0 + 36, (v0 & u(-97)) | (v1 & 0x60))
            a0 = m.lw(a0 + 56 + 8 * k)


def zone(m, s1, s2, kind, s7, fr, a2):
    """0x8003AB78's eight-way jump (table SLUS 0x80010E14)."""
    s8 = u(-31)
    if kind in (1, 2):
        m.c("zone 1/2: bit 9 from the record")
        v1 = m.lw(s1 + 36)
        a0 = m.lhu(s2)
        m.sw(s1 + 36, (v1 & u(-513)) | (a0 & 0x200))
        return
    if kind == 3:
        m.c("zone 3: bits 8, 9")
        m.sw(s1 + 36, m.lw(s1 + 36) | 0x300)
        return
    if kind == 5:
        m.c("zone 5: bits 5, 8, 9")
        m.sw(s1 + 36, (m.lw(s1 + 36) & a2) | 0x320)
        return
    h = m.lhu(s1 + 172)
    if (h >> 5) != 4 or (h & 0x1F) < 30:
        sp_ = m.lw(s1 + 480)
        if (s(sp_) >> 16) != 0:
            m.c("zone 4/6/7/8: flicker")
            sh = 19 if "a9d8.speed_shift" in MUT else 20
            v0 = s(sp_) >> sh
            i = (0 if s(sp_) < 0 else v0)
            d = 5 - v0
            i += d if d < 0 else 0
            a1 = m.lw(s1 + 36)
            s0 = m.lw(fr + 16 + 4 * i)
            c = (a1 >> 1) & 0xF
            v1 = a1 & a2
            if c < s0:
                m.sw(s1 + 36, v1 | ((1 if s7 else 0) << 6))
            else:
                v0 = a1 & s8
                if s7 == 0:
                    m.sw(s1 + 36, (v0 & a2) | 0x40)
                else:
                    m.c("flicker: random re-arm")
                    m.sw(s1 + 36, v0)
                    m.sw(fr + 48, a2)
                    r = m.call(0x8001FC58)
                    a2 = m.lw(fr + 48)
                    if r & s0:
                        m.sw(s1 + 36, m.lw(s1 + 36) & a2)
                        m.sw(fr + 48, a2)
                        r = m.call(0x8001FC58)
                        hi = u(r) % s0
                        v0 = m.lw(s1 + 36)
                        m.sw(s1 + 36, (v0 & s8) | ((hi & 0xF) << 1))
                        a2 = m.lw(fr + 48)
            v0 = m.lw(s1 + 36)
            m.sw(s1 + 36, (v0 & s8) | (((((v0 >> 1) & 0xF) + 1) & 0xF) << 1))
    a1 = m.lw(s1 + 36) | 0x200
    m.sw(s1 + 36, a1)
    k = m.lhu(s2) & 0xF
    v1 = 0 if k == 6 else (1 if k != 8 else 0)
    m.sw(s1 + 36, (a1 & u(-257)) | (v1 << 8))


def m_dcb8(m):
    R = m.R
    a0, a1 = R["a0"], R["a1"]
    fr = u(R["sp"] - 16)
    zo = 16 if "dcb8.use_y" in MUT else 18
    t0 = u(m.lh(m.lw(a0 + 340) + 14) << 4)
    m.sw(fr, t0)
    a3 = u(m.lh(m.lw(a0 + 340) + zo) << 4)
    m.sw(fr + 4, a3)
    x = m.lw(a0 + 184)
    zoff = 188 if "dcb8.use_y" in MUT else 192
    z = m.lw(a0 + zoff)
    A = mid(t0, u(x - m.lw(a1 + 20)))
    B = mid(a3, u(z - m.lw(a1 + 28)))
    C = mid(t0, u(x - m.lw(a1 + 8)))
    D = (a3, u(z - m.lw(a1 + 16)))
    lo, hi = lohi(*D)
    m.sw(fr + 8, lo)
    m.sw(fr + 12, hi)
    v0 = u(B + A)
    a0v = u(mid(*D) + C)
    r = 1 if s(v0 ^ a0v) < 0 else 0
    if r:
        m.c("straddles")
    else:
        m.c("outside")
    return r


def m_b520(m):
    def body(e):
        m.c("entity")
        s2 = m.lw(e + 324)
        v0 = m.call(0x8003B61C, e + 172)
        a0 = 0x00649581
        m.sw(e + 324, v0)
        while s(v0) < s(a0):
            v1 = m.lw(e + 1084)
            bit = m.lbu(v1 + 68) & 1
            crossed = (s(a0) < s(s2)) if "b520.forward" not in MUT else (s(a0) > s(s2))
            if bit:
                break
            if crossed:
                m.c("marker crossed")
                m.sb(v1 + 68, m.lbu(v1 + 68) | 1)
            else:
                a0 = u(a0 + 0xFFE6DAA0)
            v0 = m.lw(e + 324)
        v1 = m.lw(e + 856)
        if v1:
            m.sw(v1 + 324, m.lw(e + 324))
    list_walk(m, body)


def m_b61c(m):
    R = m.R
    s0 = R["a0"]
    fr = u(R["sp"] - 80)
    v0 = m.lh(ROUTE + 18)
    s2 = m.lw(s0 + 256)
    if v0 == -1:
        m.c("no route")
        return 0
    s1p = s0 + 188
    if s2 == 0:
        m.c("no route object")
        v = m.lw(s0 + 196)
        return u(s(v) >> 4) if "b61c.raw_return" in MUT else v
    a1 = m.lw(s0 + 188)
    a2 = a1 >> 16
    res = None
    if a2 == 1:
        m.c("in a node")
        s2 = m.call(0x8003F408, s2, a1 & 0xFFFF)
        if s2 == 0:
            res = None
        else:
            v0 = m.lw(s2 + 16)
            s1 = m.lw(s2 + 4)
            if s(v0) <= 0:
                res = s1
            else:
                s3 = m.call(0x8003BE1C, s0 + 156, m.lw(s2 + 8), m.lw(s2 + 84), fr + 48)
                if s3 == 0:
                    res = s1
                else:
                    if s3 == m.lw(s0 + 164):
                        m.c("node: same sub-object")
                        a0 = m.lw(s0 + 196)
                    else:
                        m.c("node: search the turn's sub-object")
                        m.sw(fr + 16, m.lw(s0 + 156))
                        v0 = m.lw(s0 + 160)
                        m.sw(fr + 24, s3)
                        m.sw(fr + 20, v0)
                        v0 = m.lw(s0 + 156)
                        v1 = m.lh(s3 + 8)
                        a3 = m.lw(v0 + 52)
                        for o in (40, 44, 36, 32):
                            m.sw(fr + o, 0)
                        a3 = u(a3 + 52 * v1)
                        m.sw(fr + 28, a3)
                        cur = m.call(0x80036B14, s0, fr + 16, s0 + 12)
                        pr = m.call(0x800B6AAC, s0 + 12, cur + 14, cur + 20)
                        a0 = u(m.lw(cur + 40) + pr)
                    sh = 0 if "b61c.no_shift" in MUT else 4
                    if s(m.lw(fr + 48)) > 0:
                        m.c("node: dir > 0")
                        v1 = m.lw(s2 + 8)
                        v0 = m.lw(s3 + 12)
                        res = u(u(s1 - v1) + (s(u(v0 - a0)) >> sh))
                    else:
                        m.c("node: dir <= 0")
                        v0 = m.lw(s2 + 8)
                        res = u(u(s1 - v0) + (s(a0) >> sh))
    else:
        a0 = m.lw(ROUTE + 24)
        v0 = m.lw(s2)
        v1 = m.lw(a0 + 12)
        fin = (v1 == v0 or v1 == 0xFFFFFFFF) and a2 == 0 and (a1 & 0xFFFF) == m.lw(a0)
        if fin:
            m.c("the finish road")
            d = m.lw(a0 + 8)
            if s(d) > 0:
                before = s(m.lw(s1p + 8)) < s(m.lw(a0 + 4))
            elif d == 0:
                before = True
            else:
                before = s(m.lw(a0 + 4)) < s(m.lw(s1p + 8))
            if not before:
                m.c("past the finish")
                res = 0
            else:
                fa = m.lw(m.lw(ROUTE + 24) + 4)
                v1 = m.lw(s1p + 8)
                res = u(absv(u(fa - v1)) >> 4)
        else:
            m.c("a route leg")
            leg = m.call(0x8003B4B0, s2, m.lhu(s1p))
            if leg == 0:
                res = None
            else:
                if s(m.lw(leg + 4)) > 0:
                    v0 = u(m.lw(leg + 8) - m.lw(s1p + 8))
                    nx = m.lh(leg + 14)
                else:
                    v0 = m.lw(s1p + 8)
                    nx = m.lh(leg + 12)
                res = u(s(v0) >> 4)
                if nx != -1:
                    r2 = m.call(0x8003F408, s2, u(nx))
                    if r2:
                        res = u(res + m.lw(r2 + 4))
    if res is None:
        m.c("fallback: +0x170 >> 4")
        res = u(s(m.lw(s0 + 196)) >> 4)
    if s(res) < 0 and "b61c.no_clamp" not in MUT:
        m.c("clamped to 0")
        res = 0
    return res


def m_be1c(m):
    R = m.R
    a0, s7, s6, s5 = R["a0"], R["a1"], R["a2"], R["a3"]
    if a0 == 0 or s6 == 0xFFFFFFFF:
        return 0
    s2 = m.lw(a0 + 4)
    n = m.lh(s2 + 22)
    s3 = m.lw(a0)
    s1 = 0
    while s1 < n:
        s0 = u(m.lw(s3 + 48) + 28 * (m.lh(s2 + 20) + s1))
        if (s(u(m.lw(s0 + 12) + 0x8000)) >> 16) == (s(s7) >> 12):
            m.c("a sub-object of that length")
            a3 = m.call(0x80039B60, m.lw(s3))
            if a3 and m.lh(a3 + 8) > 0:
                a1 = 0
                while True:
                    base = m.lh(a3 + 6)
                    g = m.lw(GRAPH)
                    a2 = u(m.lw(g + 52) + 12 * (base + a1))
                    if m.lh(a2 + 10) == s(s6):
                        gi = m.lh(a2 + 2)
                        gp = u(m.lw(g + 56) + 12 * gi)
                        want = u(m.lw(s3 + 48) + 28 * (m.lh(s2 + 20) + m.lh(gp + 4)))
                        if s0 == want:
                            m.c("found")
                            if m.lh(a2 + 6) > 0 or "be1c.dir_sign" in MUT:
                                m.sw(s5, u(m.lh(gp + 8)))
                            else:
                                m.sw(s5, u(-m.lh(gp + 8)))
                            return s0
                    a1 += 1
                    if not a1 < m.lh(a3 + 8):
                        break
        n = m.lh(s2 + 22)
        s1 += 1
    return 0


def m_f408(m):
    R = m.R
    a0, a1 = R["a0"], R["a1"]
    if m.lh(ROUTE + 18) == -1:
        return 0
    if a0 == 0 or a1 == 0xFFFFFFFF:
        return 0
    if m.lw(a0) == a1 and "f408.no_self" not in MUT:
        m.c("the object itself")
        return a0
    t3 = m.lw(a0 + 16)
    t1 = 0
    if not 0 < s(t3):
        return 0
    for t0 in range(s(t3)):
        if m.lw(a0 + 100 + 4 * t0) == a1:
            n = m.lh(ROUTE + 18)
            if n > 0:
                v1 = m.lw(ROUTE + 36)
                for _ in range(n):
                    if m.lw(v1) == a1:
                        m.c("a neighbour's record")
                        t1 = v1
                        break
                    v1 = u(v1 + 120)
    return t1


def _labels() -> dict:
    """Every coverage label of every model, read out of this file."""
    import inspect
    import re
    out = {}
    for key, fn in MODELS.items():
        src = inspect.getsource(fn)
        if key == "a9d8":
            src += inspect.getsource(zone)
        if key in ("37338", "e150", "ae24", "b520"):
            pass
        out[key] = list(dict.fromkeys(re.findall(r'm\.c\("([^"]+)"\)', src)))
    return out


MODELS = {
    "374d4": m_374d4, "37450": m_37450, "37338": m_37338, "701c": m_701c, "7104": m_7104,
    "396a8": m_396a8, "662c": m_662c, "e150": m_e150, "dff4": m_dff4, "ddb0": m_ddb0,
    "de28": m_de28, "ee68": m_ee68, "ef34": m_ef34, "e67c": m_e67c, "df54": m_df54,
    "f1f0": m_f1f0, "ae24": m_ae24, "af9c": m_af9c, "a9d8": m_a9d8, "dcb8": m_dcb8,
    "b520": m_b520, "b61c": m_b61c, "be1c": m_be1c, "f408": m_f408,
}


LABELS = _labels()


# ---------------------------------------------------------------------------
# The reciprocal-square-root table builder, SLUS 0x8002E080
# ---------------------------------------------------------------------------

def lzcr(x: int) -> int:
    """GTE LZCR: leading bits equal to the sign bit, 1..32."""
    x = u(x)
    p = u(~x) if x & 0x80000000 else x
    for i in range(31, -1, -1):
        if (p >> i) & 1:
            return 31 - i
    return 32


def sqrt_gte(img: E.Image, x: int) -> int:
    """SLUS 0x8004CF74, reading its table out of the player's EXE - including below it."""
    lz = lzcr(x)
    if lz == 32:
        return 0
    e = lz & ~1
    sh = (19 - e) >> 1
    n = u(x << (e - 24)) if e >= 24 else u(s(x) >> (24 - e))
    a = u(0x800560CC + 2 * (s(n) - 64))
    w = u(s16(img.word(a & ~3) >> (16 * ((a >> 1) & 1))))
    return u(w << sh) if sh >= 0 else (w >> -sh)


def build_rsqrt(img: E.Image, mut=()) -> list[int]:
    out = []
    for i in range(1024):
        d = (i >> 1) + (0 if "rsqrt.no_small" in mut else (1 if i < 2 else 0))
        q = 0xFFFFFFFF if d == 0 else 0x80000000 // d        # divu by zero, as the R3000 does
        a0 = u(sqrt_gte(img, q) << 2)
        lz = lzcr(a0) if a0 else 0
        sh = (20 if "rsqrt.shift" in mut else 21) - lz
        if sh > 0:
            a0 >>= sh
        out.append(u((a0 << 5) | u(sh)) & 0xFFFF)
    return out


def rsqrt_check(img: E.Image, mut=()) -> tuple[int, list[str]]:
    t = build_rsqrt(img, mut)
    bad, lines = 0, []
    for name in SNAPSHOTS:
        p = os.path.join(ROOT, "work", "oracle", "state", name, "ram.bin")
        if not os.path.exists(p):
            continue
        ram = open(p, "rb").read()
        ptr = struct.unpack_from("<I", ram, 0x5B560)[0]
        got = struct.unpack_from("<1024H", ram, ptr & 0x1FFFFF)
        diff = sum(1 for a, b in zip(got, t) if a != b)
        bad += diff
        lines.append(f"{name}: *(0x8005B560) = 0x{ptr:08X}, {1024 - diff}/1024 entries equal")
    return bad, lines


# ---------------------------------------------------------------------------
# PyCpu with the scratchpad and the DotLcm leaf
# ---------------------------------------------------------------------------

class Stops:
    def __init__(self, pcs):
        self.pcs = set(pcs)

    def __eq__(self, pc):
        return pc in self.pcs

    def __hash__(self):
        return 0


GTE_DOT = 0x8002E698


class Cpu(Q.PyCpu):
    """roadq's PyCpu with 1 KiB of scratchpad at 0x1F800000 (offset 0x200000 of the buffer) and
    SLUS 0x8002E698 (a GTE MVMVA) executed as the ported DotLcm - its store included."""

    def __init__(self, ram: bytearray, scr: bytes, regs, probe_pcs, bods):
        super().__init__(bytearray(ram) + bytearray(scr), regs, probe_pcs, bods)

    def _phys(self, a, n):
        a &= 0xFFFFFFFF
        if a % n:
            raise CpuStop(f"misaligned {n}-byte access 0x{a:08X}")
        seg = a & 0x1FFFFFFF
        if 0x1F800000 <= seg < 0x1F800400:
            return 0x200000 + (seg & 0x3FF)
        if seg >= 0x800000:
            raise CpuStop(f"access outside RAM 0x{a:08X}")
        return seg & 0x1FFFFF

    _stopped_at = None


def _patch_run():
    """PyCpu.run returns at STOP without saying where; wrap the comparison to remember it."""
    class Rec(Stops):
        def __init__(self, cpu, pcs):
            super().__init__(pcs)
            self.cpu = cpu

        def __eq__(self, pc):
            if pc in self.pcs:
                self.cpu._stopped_at = pc
                return True
            return False

    def go(self, entry, final):
        self._stopped_at = None
        self.STOP = Rec(self, [final, GTE_DOT])
        pc = entry
        while True:
            Q.PyCpu.run(self, pc)
            if self._stopped_at == final:
                return
            r = self.r
            if GTE_DOT in self.probe_pcs:
                self.probes[GTE_DOT].append((self.seq, {REG[i]: r[i] for i in range(32)}))
            v = [s16(self.load(GTE_DOT, r[4] + 2 * k, 2)) for k in range(3)]
            mm = [s16(self.load(GTE_DOT, r[5] + 2 * k, 2)) for k in range(3)]
            val = u(s(u(sum(v[k] * mm[k] for k in range(3)))) >> 8)
            self.store(0x8002E6E8, u(r[29] - 8), 4, val)
            r[2] = val
            r[12] = val
            self.seq += 20
            self._stopped_at = None
            pc = r[31]
    Cpu.go = go


_patch_run()


class SynthCapture:
    """The same interface as a trace capture, filled by PyCpu."""

    def __init__(self, name, ram0: bytes, scr0: bytes, cpu):
        self.name = name
        self.ram0 = ram0
        self.scr0 = scr0
        ws = [w for w in cpu.writes]
        self.wseq = [w[0] for w in ws]
        self.wpc = [w[1] for w in ws]
        self.waddr = [w[2] for w in ws]
        self.wsize = [w[3] for w in ws]
        self.wval = [w[4] for w in ws]
        self.body_reads = cpu.reads
        self.rseq = [x[0] for x in cpu.reads]
        self.calls = cpu.calls
        self.cseq = [c[0] for c in cpu.calls]
        self.probes = cpu.probes
        self.scr_ev, self.scr_seq = [], []
        # scratchpad stores PyCpu made (none are expected), so scratch_at() can apply them
        for w in ws:
            if w[2] >= 0x200000:
                self.scr_ev.append((w[0], w[2] - 0x200000, w[4]))
                self.scr_seq.append(w[0])

    probe_after = Capture.probe_after
    scratch_at = Capture.scratch_at


# ---------------------------------------------------------------------------
# The harness
# ---------------------------------------------------------------------------

def fmt(t):
    if t is None:
        return "-"
    return f"[0x{0x80000000 | t[0]:08X}]{t[1]}=0x{t[2]:X}"


def ram_at(cap, seq, img_cache):
    """Guest RAM just before `seq`: the snapshot plus every recorded store before it."""
    img, wi = img_cache
    while wi < len(cap.wseq) and cap.wseq[wi] < seq:
        n, o = cap.wsize[wi], cap.waddr[wi]
        if o < 0x200000:
            img[o:o + n] = (cap.wval[wi] & ((1 << (8 * n)) - 1)).to_bytes(n, "little")
        wi += 1
    img_cache[1] = wi
    return img


def invocations(cap, key, bods):
    entry, end, rets = bods[key]
    exs = sorted(((t[0], dict(t[1], _pc=r)) for r in rets for t in cap.probes.get(r, [])),
                 key=lambda q: q[0])
    exseq = [t[0] for t in exs]
    for (eseq, regs) in cap.probes.get(entry, []):
        i = bisect.bisect_right(exseq, eseq)
        if i < len(exs):
            yield eseq, regs, exs[i][0], exs[i][1]


EXE_IMG = None


def exit_v0(cap, xregs):
    """v0 as the caller sees it: the probe sits on the `jr ra`, so a `move v0,rX` in its delay
    slot (0x8003E750, 0x8003F4D4) has not run yet."""
    w = EXE_IMG.word(xregs["_pc"] + 4)
    if (w >> 26) == 0 and (w & 63) == 0x21 and ((w >> 11) & 31) == 2 and ((w >> 16) & 31) == 0:
        return xregs[REG[(w >> 21) & 31]]
    return xregs["v0"]


def run_model(cap, key, bods, excl, cov, stats, fails, verbose, only_seq=None):
    entry, end, _ = bods[key]
    body = (entry, end)
    retv = FUNCS[key][2]
    cache = [bytearray(cap.ram0), 0]
    for eseq, regs, xseq, xregs in invocations(cap, key, bods):
        if only_seq is not None and eseq != only_seq:
            continue
        img = ram_at(cap, eseq, cache)
        src = "pycpu" if cap.name == "synth" else "oracle"
        mc = Machine(cap, bytearray(img), cap.scratch_at(eseq), body, eseq, xseq, regs,
                     cov[(key, src)])
        stats[key] += 1
        stats[key + "." + cap.name] += 1
        try:
            v0 = MODELS[key](mc)
            if mc.ci != len(mc.calls):
                raise Mismatch(f"model made {mc.ci} calls, original {len(mc.calls)}")
            tw = []
            j = bisect.bisect_left(cap.wseq, eseq)
            while j < len(cap.wseq) and cap.wseq[j] <= xseq + 1:
                if entry <= cap.wpc[j] < end and cap.wpc[j] not in excl[key] and cap.waddr[j] < 0x200000:
                    n = cap.wsize[j]
                    tw.append((cap.waddr[j], n, cap.wval[j] & ((1 << (8 * n)) - 1)))
                j += 1
            if tw != mc.writes:
                k = next((q for q in range(min(len(tw), len(mc.writes))) if tw[q] != mc.writes[q]),
                         min(len(tw), len(mc.writes)))
                g = mc.writes[k] if k < len(mc.writes) else None
                w = tw[k] if k < len(tw) else None
                raise Mismatch(f"store #{k}: model {fmt(g)} original {fmt(w)} "
                               f"({len(mc.writes)} vs {len(tw)} stores)")
            lo = bisect.bisect_left(cap.rseq, eseq)
            hi = bisect.bisect_right(cap.rseq, xseq + 1)
            tr = {(r[2], r[3], r[4] & ((1 << (8 * r[3])) - 1)) for r in cap.body_reads[lo:hi]
                  if entry <= r[1] < end and r[1] not in excl[key] and r[2] < 0x200000}
            if tr != mc.reads:
                extra = sorted(mc.reads - tr)[:3]
                miss = sorted(tr - mc.reads)[:3]
                raise Mismatch(f"loads differ: model-only {[fmt(x) for x in extra]} "
                               f"original-only {[fmt(x) for x in miss]}")
            stats[key + ".stores"] += len(tw)
            stats[key + ".loads"] += len(tr)
            stats[key + ".calls"] += mc.ci
            want_v0 = exit_v0(cap, xregs)
            if retv and u(v0) != want_v0:
                raise Mismatch(f"v0 model 0x{u(v0):08X} original 0x{want_v0:08X}")
        except Mismatch as ex:
            fails.append((key, cap.name, eseq, str(ex)))
            if verbose and len([f for f in fails if f[0] == key]) <= 4:
                print(f"    FAIL {cap.name} {key} @seq {eseq}: {ex}")


def replay_natural(cap, bods, pset, limit=150) -> tuple[int, int, list]:
    """Re-execute natural invocations of every modelled function with PyCpu from the oracle's
    entry state; the whole store stream (callees included) must equal the oracle's."""
    bl = [(v[0], v[1]) for v in bods.values()]
    n = bad = 0
    msgs = []
    for key in FUNCS:
        cache = [bytearray(cap.ram0), 0]
        done = 0
        for eseq, regs, xseq, _ in invocations(cap, key, bods):
            if done >= limit:
                break
            done += 1
            img = ram_at(cap, eseq, cache)
            cpu = Cpu(img, cap.scratch_at(eseq), dict(regs), set(), bl)
            n += 1
            try:
                cpu.go(bods[key][0], regs["ra"])
            except CpuStop as ex:
                bad += 1
                msgs.append(f"{cap.name} {key} @{eseq}: PyCpu stopped: {ex}")
                continue
            j0 = bisect.bisect_left(cap.wseq, eseq)
            j1 = bisect.bisect_right(cap.wseq, xseq + 1)
            ks = [cap.wseq[j] for j in range(j0, j1) if (cap.wpc[j] & 0x1FFFFFFF) < 0x10000]
            win = (ks[0], ks[-1]) if ks else (1, 0)
            want = [(cap.waddr[j], cap.wsize[j], cap.wval[j] & ((1 << (8 * cap.wsize[j])) - 1))
                    for j in range(j0, j1) if not win[0] <= cap.wseq[j] <= win[1]]
            got = [(w[2], w[3], w[4]) for w in cpu.writes if w[2] < 0x200000]
            if got != want:
                bad += 1
                if len(msgs) < 8:
                    k = next((q for q in range(min(len(got), len(want))) if got[q] != want[q]),
                             None)
                    msgs.append(f"{cap.name} {key} @{eseq}: PyCpu {len(got)} stores, oracle "
                                f"{len(want)}, first difference at #{k}: "
                                f"{fmt(got[k]) if k is not None else ''} vs "
                                f"{fmt(want[k]) if k is not None else ''}")
    return n, bad, msgs


# ---------------------------------------------------------------------------
# Synthetic inputs: the original, executed by PyCpu, on states derived from the `quick` snapshot
# ---------------------------------------------------------------------------

SYNTH_SNAPSHOT = "work\\oracle\\state\\quick"


def synth_cases(n: int, seed: int):
    """Yield (family, entry, ram0, scr0, regs).  Every pointer is a live one out of the snapshot;
    scalars, flags and the cursor's place are randomised."""
    rnd = random.Random(seed)
    base = bytearray(open(os.path.join(ROOT, SYNTH_SNAPSHOT, "ram.bin"), "rb").read())
    scr0 = bytearray(open(os.path.join(ROOT, SYNTH_SNAPSHOT, "scratchpad.bin"), "rb").read())
    cpuj = json.load(open(os.path.join(ROOT, SYNTH_SNAPSHOT, "cpu.json")))
    gp = cpuj["gpr"]["gp"]

    def rw(b, a):
        return int.from_bytes(b[a & 0x1FFFFF:(a & 0x1FFFFF) + 4], "little")

    def rh(b, a):
        return s16(int.from_bytes(b[a & 0x1FFFFF:(a & 0x1FFFFF) + 2], "little"))

    def ww(b, a, v):
        b[a & 0x1FFFFF:(a & 0x1FFFFF) + 4] = u(v).to_bytes(4, "little")

    def wh(b, a, v):
        b[a & 0x1FFFFF:(a & 0x1FFFFF) + 2] = (v & 0xFFFF).to_bytes(2, "little")

    pool0 = rw(base, POOL0)
    nb = rw(base, 0x8005B1F8)
    g = rw(base, GRAPH)
    btt, nbtt = rw(base, g + 0x1C), rh(base, g + 0x28)
    ipt, nipt = rw(base, g + 0x30), rh(base, g + 0x3C)
    pdt = rw(base, g + 0x34)
    resident = [rw(base, btt + 32 * i + 12) for i in range(nbtt) if rw(base, btt + 32 * i + 12)]
    bikes = [pool0 + 1096 * i for i in range(nb)
             if rw(base, pool0 + 1096 * i + 0x148) and rw(base, pool0 + 1096 * i + 0x154)
             and rw(base, pool0 + 1096 * i + 0x43C) and rw(base, pool0 + 1096 * i + 0x354)]
    nroute = rh(base, ROUTE + 18)
    recs = [rw(base, ROUTE + 36) + 120 * i for i in range(max(0, nroute))]
    groups = []
    for obj in resident:
        nsub = rh(base, obj + 0x14)
        for k in range(nsub):
            sub = rw(base, obj + 0x30) + 28 * k
            cnt, first = rh(base, sub + 16), rh(base, sub + 18)
            for i in range(max(0, cnt)):
                gi = rh(base, rw(base, obj + 0x38) + 16 * (first + i) + 4)
                if gi >= 0:
                    groups.append(rw(base, obj + 0x3C) + 264 * gi)
    sp = 0x801FFC00

    def reseat(ram, e):
        obj = rnd.choice(resident)
        npc_ = rh(ram, obj + 0x12)
        pi = rnd.randrange(max(1, npc_))
        piece = rw(ram, obj + 0x2C) + 32 * pi
        subt = rw(ram, obj + 0x30) + 28 * rh(ram, piece + 20)
        cnt = rh(ram, subt + 10)
        slc = rw(ram, obj + 0x34) + 52 * (rh(ram, subt + 8) + rnd.randrange(max(1, cnt)))
        node = turn = 0
        if rh(ram, obj + 16) != 0 and s(rw(ram, obj + 12)) != 1:
            for q in range(nipt):
                if rh(ram, ipt + 12 * q) == s(rw(ram, obj)):
                    node = ipt + 12 * q
                    turn = pdt + 12 * (rh(ram, node + 6) + rnd.randrange(max(1, rh(ram, node + 8))))
        for c_, v_ in enumerate((obj, piece, subt, slc)):
            ww(ram, e + 0x148 + 4 * c_, v_)
        ww(ram, e + 0x160, node if rnd.random() < 0.5 else 0)
        ww(ram, e + 0x164, turn if rnd.random() < 0.5 else 0)
        # the road kind follows the object: a junction object is "in a node" most of the time
        kind = 1 if (rh(ram, obj + 16) == 1 and rnd.random() < 0.6) else 0
        ww(ram, e + 0x168, (kind << 16) | (rw(ram, obj + 8) & 0xFFFF))

    def zone_plant(ram, e):
        """A one-entry zone list at sp+0x180 naming one BSDT record of the cursor's object, with
        its kind nibble and bit 9 drawn at random, and the box centre between its end points."""
        obj, sl = rw(ram, e + 0x148), rw(ram, e + 0x154)
        bzt = rw(ram, obj + 0x64)
        if not bzt:
            return
        first = rh(ram, bzt + 4 * ram[(sl + 44) & 0x1FFFFF])
        rec = rw(ram, obj + 0x60) + 40 * first
        a = (rw(ram, rec) >> 16) & 0xFFFF
        a = (a & ~0x20F) | rnd.randint(1, 8) | rnd.choice([0, 0x200])
        wh(ram, rec + 2, a)
        ent = sp + 0x180
        wh(ram, ent, first)
        ram[(ent + 2) & 0x1FFFFF] = 0
        ram[(ent + 3) & 0x1FFFFF] = rnd.choice([1, 1, 2])
        ww(ram, e + 0x1F0, ent)
        for c in (0, 2):
            ww(ram, e + 0xB8 + 4 * c, u((s(rw(ram, rec + 8 + 4 * c)) + s(rw(ram, rec + 20 + 4 * c))) // 2))

    def jiggle(ram, e):
        """Randomise the scalars the layer reads, and put the box centre near the slice."""
        if rnd.random() < 0.6:
            reseat(ram, e)
        sl = rw(ram, e + 0x154)
        d = rnd.randint(-60, 60) * 65536 + rnd.randrange(65536)
        lat = rnd.choice([rnd.randint(-40, 40) * 65536 + rnd.randrange(65536), rnd.randint(-8, 8) * 65536])
        pt = []
        for c in range(3):
            pos = s(rw(ram, sl + 20 + 4 * c))
            pt.append(pos + ((rh(ram, sl + 14 + 2 * c) * d) >> 12) + ((rh(ram, sl + 2 + 2 * c) * lat) >> 12))
        for c in range(3):
            ww(ram, e + 0xB8 + 4 * c, pt[c])
            ww(ram, e + 0x1F8 + 4 * c, pt[c] + rnd.randint(-65536, 65536))
        ww(ram, e + 0x158, lat)
        ww(ram, e + 0x15C, rnd.randint(-10, 40) * 65536)
        ww(ram, e + 0x170, u(rw(ram, sl + 40) + rnd.randint(-80, 80) * 65536))
        ww(ram, e + 0x16C, rnd.choice([1, u(-1), 2, u(-2), 3, u(-3), 5, u(-5)]))
        f184 = rw(ram, e + 0x184) & ~0x61
        f184 |= rnd.choice([0, 1]) | (0x20 if rnd.random() < 0.3 else 0) | (0x40 if rnd.random() < 0.3 else 0)
        ww(ram, e + 0x184, f184)
        if rnd.random() < 0.3:
            ww(ram, e + 0x174, 0)
        if rnd.random() < 0.3:
            ww(ram, e + 0x178, 0)
        ww(ram, e + 0x1E0, rnd.choice([0, rnd.randint(0, 0x800000), rnd.randint(-0x20000, 0x20000)]))
        ww(ram, e + 0x24, rw(ram, e + 0x24) ^ (rnd.randrange(1 << 10) if rnd.random() < 0.5 else 0))
        r = rnd.random()
        if r < 0.5:
            ww(ram, e + 0x1EC, rw(ram, e + 0x154) + 44)
            ww(ram, e + 0x1F0, 0)
        elif r < 0.6:
            ww(ram, e + 0x1EC, u(-1))
        elif r < 0.7:
            ww(ram, e + 0x1F0, u(-1))
        elif r < 0.8:
            ww(ram, e + 0x1EC, 0)
            ww(ram, e + 0x1F0, 0)
        elif r < 0.95:
            zone_plant(ram, e)
        ww(ram, e + 0x230, rw(ram, e + 0x230) ^ (0x08000000 if rnd.random() < 0.5 else 0))
        ww(ram, e + 0x234, rw(ram, e + 0x234) & ~0x40000)
        if rnd.random() < 0.15:
            # riding a contact: +0x234 bit 18, +0x340 -> a record whose +0xC0 becomes +0x16C
            ww(ram, e + 0x234, rw(ram, e + 0x234) | 0x40000)
            ww(ram, e + 0x340, rnd.choice(bikes))
        if rnd.random() < 0.3:
            # a passenger: the bike's own rider, with a list node
            ww(ram, e + 0x358, rw(ram, e + 0x354))
            if rw(ram, e + 0x440) == 0:
                ww(ram, e + 0x440, e + 0x440)
        else:
            ww(ram, e + 0x358, 0)
        ww(ram, e + 0x238, (rw(ram, e + 0x238) & ~0xE00) | rnd.choice([0, 0x200, 0x400, 0x800]))
        ww(ram, e + 0x33C, rnd.choice([0, 0, rw(ram, e + 0x154)]))
        ww(ram, e + 0x144, rnd.choice([rnd.randint(0, 0x700000), 0x649581 + rnd.randint(-0x4000, 0x4000)]))
        rd = rw(ram, e + 0x43C)
        ram[(rd + 68) & 0x1FFFFF] = (ram[(rd + 68) & 0x1FFFFF] & 0xFE) | rnd.choice([0, 0, 1])
        ram[rd & 0x1FFFFF] ^= rnd.choice([0, 0x80])
        if recs and rnd.random() < 0.8:
            ww(ram, e + 0x1AC, rnd.choice(recs + [rw(ram, e + 0x1AC)]))
        elif rnd.random() < 0.5:
            ww(ram, e + 0x1AC, 0)

    fin = rw(base, ROUTE + 24)
    obj14 = next((o for o in resident if rw(base, o) == 14), None)
    ipt14 = next((ipt + 12 * q for q in range(nipt) if rh(base, ipt + 12 * q) == 14), 0)
    roads_to = sorted({rh(base, pdt + 12 * (rh(base, ipt14 + 6) + i) + 10)
                       for i in range(rh(base, ipt14 + 8))}) if ipt14 else []
    disc_groups = []
    import chunk as C
    for sn in (1, 2):
        for o in C.load_type3(os.path.join(ROOT, "work", "disc_us", "DATA", f"STREAM{sn}.STR")).values():
            if "XSDH" in o.bl:
                off, sz = o.bl["XSDH"]
                disc_groups += [o.raw[off + 8 + i:off + 8 + i + 264] for i in range(0, sz - 8, 264)]

    def route_case(ram, e):
        r = rnd.random()
        if r < 0.1:
            wh(ram, ROUTE + 18, 0xFFFF)                  # no route loaded
        elif r < 0.35 and recs:
            # on the finish road, bound to the record the finish test names
            want = rw(ram, fin + 12)
            ww(ram, e + 0x168, rw(ram, fin) & 0xFFFF)
            ww(ram, e + 0x1AC, next((q for q in recs if rw(ram, q) == want), rnd.choice(recs)))
            ww(ram, e + 0x170, u(rw(ram, fin + 4) + rnd.randint(-200, 200) * 65536))
            if rnd.random() < 0.3:
                ww(ram, fin + 8, rnd.choice([0, u(-1), 1]))
        elif r < 0.7 and obj14 and recs and roads_to:
            # inside junction 7's core, bound to a route record of node 7 whose crossing length
            # and exit road are drawn from the core's own sub-objects and turn table
            rec = next((q for q in recs if rw(ram, q) == 7), rnd.choice(recs))
            ww(ram, e + 0x168, (1 << 16) | 7)
            ww(ram, e + 0x1AC, rec)
            npc_ = rh(ram, obj14 + 0x12)
            piece = rw(ram, obj14 + 0x2C) + 32 * rnd.randrange(npc_)
            ns = rh(ram, piece + 22)
            sub = rw(ram, obj14 + 0x30) + 28 * (rh(ram, piece + 20) + rnd.randrange(max(1, ns)))
            slc = rw(ram, obj14 + 0x34) + 52 * (rh(ram, sub + 8) + rnd.randrange(max(1, rh(ram, sub + 10))))
            for c_, v_ in enumerate((obj14, piece, sub, slc)):
                ww(ram, e + 0x148 + 4 * c_, v_)
            other = rw(ram, obj14 + 0x30) + 28 * (rh(ram, piece + 20) + rnd.randrange(max(1, ns)))
            ww(ram, rec + 8, ((s(rw(ram, other + 12)) + 0x8000) >> 16) << 12)
            ww(ram, rec + 0x54, rnd.choice(roads_to))
            ww(ram, rec + 16, rnd.choice([0, 1, 1, 2]))

    for k in range(n):
        ram = bytearray(base)
        scr = bytearray(scr0)
        regs = {"gp": gp, "sp": sp, "ra": Q.PyCpu.STOP}
        fam = k % 6
        if fam == 0:
            e = rnd.choice(bikes)
            jiggle(ram, e)
            regs.update(a0=u(e), a1=rnd.choice([0, 1]))
            yield ("374d4", 0x800374D4, bytes(ram), bytes(scr), regs)
        elif fam in (1, 2):
            lst = rnd.sample(bikes, rnd.randint(1, min(4, len(bikes))))
            for i, e in enumerate(lst):
                jiggle(ram, e)
                scr[4 + 4 * i:8 + 4 * i] = u(e).to_bytes(4, "little")
            scr[0:4] = u(SCR + 4 + 4 * len(lst)).to_bytes(4, "little")
            ent = rnd.choice([0x80037338, 0x8003E150, 0x8003AE24, 0x8003B520])
            if ent in (0x8003AE24, 0x8003B520):
                route_case(ram, lst[0])
            yield ("pass", ent, bytes(ram), bytes(scr), regs)
        elif fam == 3:
            # a cross-section group copied from the player's disc into free stack RAM
            grp = sp + 0x200
            gb = rnd.choice(disc_groups)
            ram[grp & 0x1FFFFF:(grp & 0x1FFFFF) + 264] = gb
            side = rnd.choice([1, 2])
            h = 8 if side == 1 else 136
            edges = [s(struct.unpack_from("<I", gb, h + o)[0]) for o in (8, 24, 72)]
            e0 = abs(rnd.choice(edges))
            lat = rnd.choice([rnd.randint(-40 * 65536, 40 * 65536), rnd.randint(-2, 2),
                              e0 + rnd.randint(-1, 1), -e0 + rnd.randint(-1, 1)])
            regs.update(a0=u(grp), a1=u(lat), a2=side, a3=u(sp + 0x100))
            yield ("e67c", 0x8003E67C, bytes(ram), bytes(scr), regs)
        elif fam == 4:
            e = rnd.choice(bikes)
            jiggle(ram, e)
            route_case(ram, e)
            regs.update(a0=u(e + 0xAC))
            yield ("b61c", 0x8003B61C, bytes(ram), bytes(scr), regs)
        else:
            e = rnd.choice(bikes)
            jiggle(ram, e)
            regs.update(a0=u(e), a1=rnd.choice([0, 0, 1]), a2=rnd.choice([0, u(sp + 0x100)]),
                        a3=rnd.choice([u(-1), 0, 2]))
            yield ("de28", 0x8003DE28, bytes(ram), bytes(scr), regs)


def run_synthetic(n, seed, bods, pset, excl, which, cov, stats, fails, verbose):
    bl = [(v[0], v[1]) for v in bods.values()]
    rejected = Counter()
    fams = Counter()
    for fam, entry, ram0, scr0, regs in synth_cases(n, seed):
        cpu = Cpu(bytearray(ram0), scr0, dict(regs), pset, bl)
        try:
            cpu.go(entry, Q.PyCpu.STOP)
        except CpuStop as ex:
            rejected[str(ex).split(" 0x")[0]] += 1
            continue
        fams[fam] += 1
        cap = SynthCapture("synth", ram0, scr0, cpu)
        for key in which:
            run_model(cap, key, bods, excl, cov, stats, fails, verbose)
    return rejected, fams


def save_pcs(img: E.Image, entry: int, end: int) -> set[int]:
    """pcs of the prologue's callee-saved spills and of the matching epilogue reloads.  The
    prologue ends at the first branch or jump (and its delay slot) - roadq's fixed 24-instruction
    window would also swallow `sw s0,16(sp)` at 0x8003DDF8, a real stack argument."""
    saved = set()
    a = entry
    while a < end:
        w = img.word(a)
        op, fn = E.OP(w), E.FUNCT(w)
        if op == 0x2B and E.RS(w) == 29 and (16 <= E.RT(w) <= 23 or E.RT(w) in (30, 31)):
            saved.add((E.RT(w), E.SIMM(w), a))
        if op in (1, 2, 3, 4, 5, 6, 7) or (op == 0 and fn in (8, 9)):
            w2 = img.word(a + 4)
            if E.OP(w2) == 0x2B and E.RS(w2) == 29 and (16 <= E.RT(w2) <= 23 or E.RT(w2) in (30, 31)):
                saved.add((E.RT(w2), E.SIMM(w2), a + 4))
            break
        a += 4
    pairs = {(r, o) for r, o, _ in saved}
    pcs = {pc for _, _, pc in saved}
    for a in range(entry, end, 4):
        w = img.word(a)
        if E.OP(w) == 0x23 and E.RS(w) == 29 and (E.RT(w), E.SIMM(w)) in pairs:
            pcs.add(a)
    return pcs


def data_checks() -> list[str]:
    """Facts about the type-3 road objects that the layer's reading leans on, read from the player's
    own STREAM<n>.STR (block layouts as in road_chunk.md 2.2)."""
    import chunk as C
    bad = []
    for sn in (1, 2):
        objs = C.load_type3(os.path.join(ROOT, "work", "disc_us", "DATA", f"STREAM{sn}.STR"))
        grpt, xs4, half6, kinds, bz = Counter(), Counter(), Counter(), Counter(), Counter()
        xs_core = Counter()
        tiles = nsub = ngroups = in_range = nx_all = b200 = 0
        for oid, o in objs.items():
            b = o.raw

            def blk(tag):
                if tag not in o.bl:
                    return b"", 0
                off, sz = o.bl[tag]
                return b[off + 8:off + sz], sz - 8
            d, sz = blk("GRPT")
            for i in range(0, sz, 32):
                grpt[(struct.unpack_from("<h", d, i + 2)[0], struct.unpack_from("<h", d, i)[0] != 0)] += 1
            xs, xsz = blk("XSIH")
            xd, xdsz = blk("XSDH")
            if xdsz % 264:
                bad.append(f"STREAM{sn} object {oid}: XSDH is not a whole number of 264-byte groups")
            ng = xdsz // 264
            ngroups += ng
            for i in range(0, xdsz, 264):
                for h in (8, 136):
                    half6[struct.unpack_from("<h", xd, i + h + 6)[0] > 0] += 1
            pos = 0
            for sb in o.subt:
                nsub += 1
                cnt, first = sb[4] & 0xFFFF, sb[4] >> 16
                tiles += first == pos
                pos = first + cnt
            if pos != xsz // 16:
                bad.append(f"STREAM{sn} object {oid}: SUBT +0x10 runs end at {pos}, XSIH has {xsz // 16}")
            for i in range(0, xsz, 16):
                g = struct.unpack_from("<h", xs, i + 4)[0]
                xs4["-1" if g < 0 else "group"] += 1
                nx_all += 1
                in_range += g < ng
            gd, gsz = blk("GRPT")
            for i in range(0, gsz, 32):
                core = struct.unpack_from("<h", gd, i + 2)[0]
                fs, ns = struct.unpack_from("<2h", gd, i + 20)
                for si in range(fs, fs + ns):
                    sb = o.subt[si]
                    for q in range((sb[4] >> 16), (sb[4] >> 16) + (sb[4] & 0xFFFF)):
                        g = struct.unpack_from("<h", xs, 16 * q + 4)[0]
                        xs_core[(core, g < 0)] += 1
            d, sz = blk("BSDT")
            for i in range(0, sz, 40):
                a = struct.unpack_from("<H", d, i + 2)[0]
                kinds[a & 0xF] += 1
                b200 += (a & 0x200) != 0
            d, sz = blk("BZDT")
            for i in range(0, sz, 4):
                bz[d[i + 3]] += 1
        if tiles != nsub:
            bad.append(f"STREAM{sn}: SUBT +0x10 runs tile XSIH in {tiles} of {nsub}")
        if any(core == 1 and nz for (core, nz) in grpt):
            bad.append(f"STREAM{sn}: a core GRPT record has a nonzero +0x00 halfword")
        if xs_core[(1, False)] or xs_core[(0, True)]:
            bad.append(f"STREAM{sn}: XSIH group -1 is not exactly the core pieces' records: "
                       f"{dict(xs_core)}")
        print(f"  STREAM{sn}.STR: SUBT +0x10 = (first << 16) | count tiles XSIH in {tiles}/{nsub}; "
              f"XSIH +4 {dict(xs4)}, < group count {in_range}/{nx_all}; XSDH {ngroups} groups of 264 B, "
              f"half +6 > 0 on {half6[True]} of {half6[True] + half6[False]} halves; GRPT (+2, +0 != 0) "
              f"{dict(sorted(grpt.items()))}; XSIH under (core, group -1) "
              f"{dict(sorted(xs_core.items()))}; BSDT kinds {dict(sorted(kinds.items()))}, bit 9 on {b200}; "
              f"BZDT count byte {dict(sorted(bz.items()))}")
    return bad


def static_checks(img: E.Image) -> list[str]:
    """A few load-bearing words, read from the player's own EXE."""
    bad = []

    def want(addr, word, what):
        if img.word(addr) != word:
            bad.append(f"0x{addr:08X} {what}: 0x{img.word(addr):08X} != 0x{word:08X}")
    want(0x8003B56C, 0x34849581, "ori a0,a0,0x9581 - the first distance marker 0x649581")
    want(0x8003B558, 0x3673DAA0, "ori s3,s3,0xdaa0 - the marker step -0x192560")
    want(0x8003F1C8, 0x00641824, "and v1,v1,a0 - +0x184 & ~0xF before the class nibble")
    want(0x8003E68C, 0x14C20003, "bne a2,v0 - side 2 selects the +136 half")
    want(0x8003AB68, 0x24E70E14, "addiu a3,a3,3604 - the zone jump table at 0x80010E14")
    want(0x8002E0FC, 0x24020015, "li v0,21 - the rsqrt table's normalising shift")
    for i, w in enumerate((0xF, 0xF, 0xF, 0x7, 0x1, 0x1)):
        want(0x80010DFC + 4 * i, w, f"flicker period table entry {i}")
    for i, t in enumerate((0x8003AB80, 0x8003AB80, 0x8003AD2C, 0x8003ABB4, 0x8003ABA0,
                           0x8003ABB4, 0x8003ABB4, 0x8003ABB4)):
        want(0x80010E14 + 4 * i, t, f"zone jump table, kind {i + 1}")
    return bad


def evaluate(caps, bods, pset, excl, which, args, verbose, quiet=False):
    cov = defaultdict(Counter)
    stats = Counter()
    fails = []
    for cap in caps:
        for key in which:
            run_model(cap, key, bods, excl, cov, stats, fails, verbose)
    rej = fams = Counter()
    if args.synthetic:
        rej, fams = run_synthetic(args.synthetic, args.seed, bods, pset, excl, which, cov, stats,
                                  fails, verbose)
    return cov, stats, fails, rej, fams


def cmd_verify(args) -> int:
    img = E.load_exe()
    if img.sha1 != SLUS_SHA1:
        print(f"FAIL: {img.name} sha1 {img.sha1} != {SLUS_SHA1}")
        return 1
    global EXE_IMG
    EXE_IMG = img
    bods = bodies(img)
    pset = probe_set(img)
    excl = {k: save_pcs(img, b[0], b[1]) for k, b in bods.items()}
    which = [args.only] if args.only else list(FUNCS)
    sbad = static_checks(img) + data_checks()
    for b in sbad:
        print("  static FAIL", b)
    # the rsqrt builder
    rbad, rlines = rsqrt_check(img)
    print("  rsqrt table (SLUS 0x8002E080, modelled with SqrtGte 0x8004CF74 and GTE LZCR):")
    for ln in rlines:
        print("    " + ln)
    caps = []
    for name in CAPTURES:
        d = os.path.join(ROOT, CAPTURES[name][1])
        if not os.path.exists(os.path.join(d, "watch.csv")):
            print(f"  capture '{name}' missing ({d}) - run `roadrt.py capture` first")
            continue
        caps.append(Capture(name, bods, args.verbose))
    for c in caps:
        for f in bods:
            b = bods[f]
            ram = c.ram0
            for a in range(b[0], b[1], 4):
                if int.from_bytes(ram[a & 0x1FFFFF:(a & 0x1FFFFF) + 4], "little") != img.word(a):
                    sbad.append(f"{c.name}: resident code of {f} differs from the EXE at 0x{a:08X}")
                    break
    if args.mutate:
        return cmd_mutate(args, img, caps, bods, pset, excl)
    nat_n = nat_bad = 0
    for c in caps:
        n, bad, msgs = replay_natural(c, bods, pset)
        nat_n += n
        nat_bad += bad
        for mline in msgs:
            print("    PyCpu:", mline)
    print(f"  PyCpu vs oracle: {nat_n} natural invocations re-executed, {nat_bad} with a different "
          f"store stream")
    cov, stats, fails, rej, fams = evaluate(caps, bods, pset, excl, which, args, args.verbose)
    if args.synthetic:
        print(f"  synthetic: {args.synthetic} cases, seed {args.seed}, executed "
              f"{dict(fams)}, rejected by PyCpu {sum(rej.values())} {dict(rej)}")
    total = tf = 0
    for key in which:
        n = stats[key]
        nf = len([f for f in fails if f[0] == key])
        total += n
        tf += nf
        per = ", ".join(f"{c} {stats[key + '.' + c]}" for c in ("quick", "race", "synth"))
        print(f"  {key:6s} 0x{FUNCS[key][0]:08X}  invocations {n:6d} ({per})  mismatches {nf}  "
              f"{'PASS' if nf == 0 and n > 0 else ('FAIL' if nf else 'UNRUN')}")
        print(f"         compared: {stats[key + '.stores']} stores, {stats[key + '.loads']} distinct "
              f"loads, {stats[key + '.calls']} calls")
        if args.coverage:
            for lab in LABELS.get(key, []):
                o, p_ = cov[(key, "oracle")][lab], cov[(key, "pycpu")][lab]
                tag = "" if o else ("   <- PyCpu only" if p_ else "   <- NEVER")
                print(f"           oracle {o:6d}  pycpu {p_:6d}  {lab}{tag}")
    for f in fails[:12]:
        print(f"    FAIL {f[1]} {f[0]} @{f[2]}: {f[3]}")
    unrun = [k for k in which if stats[k] == 0]
    ok = tf == 0 and nat_bad == 0 and not sbad and rbad == 0 and not unrun
    print(f"roadrt: {len(which)} functions, {total} invocations, {tf} mismatches, {nat_bad} "
          f"PyCpu/oracle differences, rsqrt {rbad} entry differences, {len(sbad)} static failures, "
          f"{len(unrun)} unrun -> {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


def cmd_mutate(args, img, caps, bods, pset, excl) -> int:
    caught = 0
    names = list(MUTANTS) + ["rsqrt.no_small", "rsqrt.shift"]
    if args.mutants:
        names = [n for n in names if n in args.mutants.split(",")]
    for mname in names:
        if mname.startswith("rsqrt."):
            bad, _ = rsqrt_check(img, (mname,))
            ok = bad > 0
            print(f"  mutant {mname:22s} {'CAUGHT' if ok else 'MISSED'}  {bad} of 5x1024 entries differ")
            caught += ok
            continue
        MUT.clear()
        MUT.add(mname)
        key = mname.split(".")[0]
        _, stats, fails, _, _ = evaluate(caps, bods, pset, excl, [key], args, False)
        MUT.clear()
        nat = len([f for f in fails if f[1] != "synth"])
        syn = len([f for f in fails if f[1] == "synth"])
        nn = stats[key + ".quick"] + stats[key + ".race"]
        ok = (nat + syn) > 0
        caught += ok
        what = MUTANTS.get(mname, "")
        print(f"  mutant {mname:22s} {'CAUGHT' if ok else 'MISSED'}  natural {nat:4d}/{nn:<5d} "
              f"synthetic {syn:4d}/{stats[key + '.synth']:<5d} ({what})")
    print(f"roadrt --mutate: {caught} of {len(names)} mutants caught")
    return 0 if caught == len(names) else 1


def cmd_capture(_args) -> int:
    img = E.load_exe()
    ps = probe_set(img)
    print(f"{len(ps)} probes: every function of the resident closure of "
          f"{', '.join(f'0x{r:08X}' for r in ROOTS)} (entry, every `jr ra`, every return address),")
    print("plus the two writers of the scratchpad active list. Run from the project root.")
    print()
    parg = " ".join(f"--probe 0x{a:08X}" for a in sorted(ps))
    for name, (snap, cap, extra) in CAPTURES.items():
        print(f"build_m0\\rrverify.exe trace --state {snap} {extra} --no-cop2 --no-gpu "
              f"--max-steps 60000000 --calls --watch 0x80000000:0x200000:ram --out {cap} {parg}")
        print()
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split(chr(10))[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    v = sub.add_parser("verify")
    v.add_argument("--mutate", action="store_true")
    v.add_argument("--mutants", help="comma list: run only these mutants")
    v.add_argument("--only", choices=list(FUNCS))
    v.add_argument("--verbose", action="store_true")
    v.add_argument("--coverage", action="store_true")
    v.add_argument("--synthetic", type=int, default=1200, help="synthetic cases (0 = none)")
    v.add_argument("--seed", type=int, default=0x5EED)
    sub.add_parser("capture")
    a = ap.parse_args()
    return {"verify": cmd_verify, "capture": cmd_capture}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
