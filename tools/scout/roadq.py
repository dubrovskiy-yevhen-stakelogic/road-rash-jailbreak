"""Scout probe for THE ROAD QUERY - "which slice am I on" and the AI's road look-ahead
(Road Rash: Jailbreak, USA, SLUS_01053).


What it proves.  Six resident functions are transcribed below as executable Python models,
one statement per original load/store/call, in the original's order:

    SLUS 0x80036B14  RoadSliceSearch        (1288 bytes)
    SLUS 0x8003697C  AlongFromAnchor        ( 408 bytes)  - its distance helper
    SLUS 0x80037A30  NeighboursForward      (1420 bytes)  - function pointer, direction > 0
    SLUS 0x80037FBC  NeighboursBackward     (1428 bytes)  - function pointer, direction <= 0
    SLUS 0x80039048  PickNeighbour          (1192 bytes)
    SLUS 0x800386DC  RoadLookAhead          (2412 bytes)

Each model is run against EVERY invocation of the original inside a captured trace of the
oracle interpreter (`rrverify trace` with a whole-RAM watch, the call log and pc probes;
see `capture`).  For one invocation the model starts from the exact guest RAM at the
original's entry (the snapshot plus every recorded store before it) and the entry register
file (a probe).  Every callee the model calls is SUPPLIED BY THE TRACE: the model's call is
matched against the original's next call from the same function body - target and every
declared argument register, read at the callee's own entry probe - and the callee's stores
and return value are replayed from the trace.  Then four things are compared:

  * the ordered list of the function body's own stores (address, size, value), excluding
    only the callee-saved register spills of the prologue;
  * the set of the body's own loads (address, size, value), same exclusion;
  * the sequence of calls;
  * v0 at the `jr ra`, where the function returns a value.

So a green row means: given the callees' behaviour, the transcription does exactly what the
original body does, store for store, on real inputs.  It says nothing about the callees.

The captures reach few branches, so the probe also carries PyCpu, a small R3000 integer
executor.  `verify` first re-executes every natural top-level invocation with it and requires
its whole store stream (callees included) to equal the oracle's; then it runs the ORIGINAL bytes
on synthetic inputs built from the `quick` snapshot and checks the models against that run in
exactly the same way.  `--coverage` prints, per model branch, how often the oracle and how often
PyCpu reached it, so what rests on which is visible, not implied.

Reads ONLY work\\disc_us (SLUS_010.53: hash, prologue decoding; DATA\\ROAD<n>.MAP and
STREAM<n>.STR: the table facts), the oracle snapshots
under work\\oracle\\state and the captures under work\\roadq (all gitignored).  Writes ONLY to
stdout.  No game bytes are copied anywhere.

Usage (from the project root):

    python tools\\scout\\roadq.py capture            # print the two rrverify commands
    python tools\\scout\\roadq.py verify             # the check (last line: roadq: ...)
    python tools\\scout\\roadq.py verify --coverage  # plus the per-branch coverage
    python tools\\scout\\roadq.py verify --synthetic 4000 --seed 7   # other synthetic cases
    python tools\\scout\\roadq.py verify --mutate    # the negative control: every mutant must FAIL
    python tools\\scout\\roadq.py verify --only 36b14 --verbose

`--mutate` applies each of the named single-point mutations in MUTANTS below, one at a time,
to the models, and passes only if EVERY mutant produces at least one mismatch.
"""

from __future__ import annotations

import argparse
import bisect
import os
import struct
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import exe as E  # noqa: E402  (sibling scout module; read-only use)

ROOT = E.ROOT
SLUS_SHA1 = "67ed165a2c517d4e6106fb0dfa324d66dd9a76f1"

CAPTURES = {
    # name: (snapshot dir, capture dir, rrverify arguments besides the probes)
    "quick": ("work\\oracle\\state\\quick", "work\\roadq\\cap_quick", "--frames 30"),
    "race": ("work\\oracle\\state\\rr-race", "work\\roadq\\cap_race", "--frames 36"),
}

# function: (entry, end-exclusive, frame size, returns a value)
FUNCS = {
    "36b14": (0x80036B14, 0x8003701C, 224, True),
    "3697c": (0x8003697C, 0x80036B14, 24, True),
    "37a30": (0x80037A30, 0x80037FBC, 88, True),
    "37fbc": (0x80037FBC, 0x80038550, 88, True),
    "39048": (0x80039048, 0x800394F0, 56, True),
    "386dc": (0x800386DC, 0x80039048, 312, False),
}

# Every callee of the six, with the number of argument REGISTERS its caller sets (read out of
# the callers' own code; stack arguments are stores of the body and are compared as such).
CALLEES = {
    0x8001E0B4: ("memcpy", 3), 0x8003697C: ("AlongFromAnchor", 4),
    0x80037A30: ("NeighboursForward", 4), 0x80037FBC: ("NeighboursBackward", 4),
    0x80039048: ("PickNeighbour", 4), 0x800394F0: ("NextObjectMissing", 2),
    0x8003C840: ("NextObjectFwd", 3), 0x8003C948: ("NextObjectBwd", 3),
    0x80039AFC: ("NodeRecord", 1), 0x80039B60: ("JunctionIndex", 1),
    0x80039BB0: ("TurnsFrom", 4), 0x80039C38: ("TurnTarget", 1),
    0x8003A37C: ("NodeArm", 2), 0x80039C90: ("FindRoadPiece", 2),
    0x8003F3B4: ("F3B4", 1), 0x8003F5D0: ("F5D0", 1), 0x8003CCA0: ("AiJunctionChoice", 3),
    0x8001FC58: ("Rand", 0), 0x8001FC90: ("FixMul", 2), 0x80010028: ("FixDiv", 2),
    0x8002EAD8: ("MulAdd", 4), 0x8002E6F8: ("Blend32", 4),
}

REG = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5",
       "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1",
       "gp", "sp", "fp", "ra"]

SLICE = 52                      # SLCT record
POOL0 = 0x8005B3A0              # -> pool-0 slot 0, stride 1096
GAME_STATE = 0x8005B2F8         # -> game_state; +0x30 human players, +0x04 flags
BIKE_COUNT = 0x8005B1F8
POOL3 = 0x800CF660              # traffic, stride 512

MUTANTS = {
    "36b14.threshold": "the loop's settle test uses 132 instead of 131",
    "36b14.first_diff_vs_prev": "the first settle test compares against the first candidate, not 0",
    "36b14.exit_commit": "the reverse-direction exit commit is dropped",
    "3697c.anchor": "a direction flip anchors at the slice START instead of its end",
    "3697c.no_shift": "the tangent is used without the << 4 promotion",
    "37a30.last_is_step": "the forward step test is index <= count-1 instead of < count-1",
    "37fbc.dir_sign": "the backward neighbour's direction is not negated on the link < 0 arm",
    "39048.y_writes_dir": "the single-candidate arm also writes *dirOut when the sub-object is equal",
    "39048.no_memory": "the pool-0 arm ignores the remembered junction choice at +0x3B3",
    "386dc.racing_line": "the look-ahead uses SLCT +0x27 where the original uses +0x26",
    "386dc.no_restore": "the caller's cursor +0x0C is not restored on exit",
    "386dc.half": "the first-half test is s6 <= chord/2 instead of s6 < chord/2",
    "39048.remember_always": "the junction choice is remembered even when +0x3B4 is already set",
    "386dc.dead_end_restore": "the dead-end path restores the caller's cursor +0x0C like the normal exit",
    "36b14.human_flag": "the missing-object exit sets gp+2292 for every entity, not only humans",
}
MUT: set[str] = set()


def u(x: int) -> int:
    return x & 0xFFFFFFFF


def s(x: int) -> int:
    x &= 0xFFFFFFFF
    return x - 0x100000000 if x & 0x80000000 else x


def s16(x: int) -> int:
    x &= 0xFFFF
    return x - 0x10000 if x & 0x8000 else x


def mid(a: int, b: int) -> int:
    """(lo >> 16) | (hi << 16) of the signed 64-bit product: bits 16..47, as u32."""
    return ((s(a) * s(b)) >> 16) & 0xFFFFFFFF


def lohi(a: int, b: int) -> tuple[int, int]:
    p = s(a) * s(b)
    return p & 0xFFFFFFFF, (p >> 32) & 0xFFFFFFFF


# ---------------------------------------------------------------------------
# The capture
# ---------------------------------------------------------------------------

class Capture:
    def __init__(self, name: str, bodies: dict[str, tuple[int, int]], verbose=False):
        snap, cap, _ = CAPTURES[name]
        self.name = name
        self.ram0 = open(os.path.join(ROOT, snap, "ram.bin"), "rb").read()
        assert len(self.ram0) == 0x200000
        d = os.path.join(ROOT, cap)
        in_body = lambda pc: any(a <= pc < b for a, b in bodies.values())  # noqa: E731
        self.wseq, self.wpc, self.waddr, self.wsize, self.wval = [], [], [], [], []
        self.body_reads = []    # (seq, pc, addr, size, value)
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
                    if 0x80036B14 - 0x200 <= p < 0x800394F0 and in_body(p):
                        self.body_reads.append((int(seq), p, int(addr, 16) & 0x1FFFFF,
                                                int(size), int(val, 16)))
        self.rseq = [r[0] for r in self.body_reads]
        raw = open(os.path.join(d, "calls.bin"), "rb").read()
        self.calls = [struct.unpack_from("<QIIIIIII", raw, o) for o in range(0, len(raw), 40)]
        self.cseq = [c[0] for c in self.calls]
        self.probes = defaultdict(list)   # pc -> [(seq, regs dict)]
        with open(os.path.join(d, "probes.csv"), "r") as f:
            f.readline()
            for line in f:
                p = line.rstrip("\n").split(",")
                regs = {REG[i]: int(p[3 + i], 16) for i in range(32)}
                self.probes[int(p[1], 16)].append((int(p[0]), regs))
        for v in self.probes.values():
            v.sort(key=lambda t: t[0])

    def probe_after(self, pc: int, seq: int):
        lst = self.probes.get(pc, [])
        i = bisect.bisect_right([t[0] for t in lst], seq)
        return lst[i] if i < len(lst) else None


class Mismatch(Exception):
    pass


class Machine:
    """One invocation of one original function, replayed through a model."""

    def __init__(self, cap: Capture, img: bytearray, body: tuple[int, int], entry_seq: int,
                 exit_seq: int, regs: dict, cov: Counter):
        self.cap, self.b, self.body = cap, img, body
        self.entry_seq, self.exit_seq, self.R = entry_seq, exit_seq, regs
        self.reads: set = set()
        self.writes: list = []
        self.cov = cov
        lo = bisect.bisect_left(cap.cseq, entry_seq)
        hi = bisect.bisect_right(cap.cseq, exit_seq)
        self.calls = [c for c in cap.calls[lo:hi] if body[0] <= c[1] < body[1]]
        self.ci = 0
        self.wi = bisect.bisect_left(cap.wseq, entry_seq)   # next trace write to replay

    # --- memory, as the body sees it
    def _rd(self, a: int, n: int) -> int:
        o = a & 0x1FFFFF
        v = int.from_bytes(self.b[o:o + n], "little")
        self.reads.add((o, n, v))
        return v

    def lw(self, a):
        return self._rd(a, 4)

    def lh(self, a):
        return s16(self._rd(a, 2))

    def lhu(self, a):
        return self._rd(a, 2)

    def lb(self, a):
        v = self._rd(a, 1)
        return v - 0x100 if v & 0x80 else v

    def lbu(self, a):
        return self._rd(a, 1)

    def _wr(self, a: int, n: int, v: int):
        o = a & 0x1FFFFF
        v &= (1 << (8 * n)) - 1
        self.b[o:o + n] = v.to_bytes(n, "little")
        self.writes.append((o, n, v))

    def sw(self, a, v):
        self._wr(a, 4, v)

    def sh(self, a, v):
        self._wr(a, 2, v)

    def sb(self, a, v):
        self._wr(a, 1, v)

    def c(self, label: str):
        self.cov[label] += 1

    # --- a call: matched against the original's, callee effects replayed from the trace
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
            got = u(args[i])
            want = ent[1]["a%d" % i]
            if got != want:
                raise Mismatch(f"call #{self.ci} {name} from 0x{frm:08X}: a{i} model 0x{got:08X} "
                               f"original 0x{want:08X}")
        ret = self.cap.probe_after(frm + 8, seq)
        if ret is None:
            raise Mismatch(f"no return probe at 0x{frm + 8:08X}")
        # replay every store that is not the body's own, up to the return
        cap = self.cap
        while self.wi < len(cap.wseq) and cap.wseq[self.wi] < ret[0]:
            j = self.wi
            self.wi += 1
            if self.body[0] <= cap.wpc[j] < self.body[1]:
                continue
            n = cap.wsize[j]
            o = cap.waddr[j]
            self.b[o:o + n] = (cap.wval[j] & ((1 << (8 * n)) - 1)).to_bytes(n, "little")
        return ret[1]["v0"]


# ---------------------------------------------------------------------------
# The models.  Each takes the machine `m` and returns v0 (or None).  Comments give the
# original's addresses; `m.c(label)` marks a branch for the coverage report.
# ---------------------------------------------------------------------------

def dot_row2(m, slc, p, q, spill):
    """The inlined 3-term dot product of 0x80036800/0x8003697C/0x80036B14:
    sum over k of ((m_row2[k] << 4) * (p[k] - q[k])) >> 16, each term truncated to 32 bits.
    `q` is a callable giving q[k] (it reads memory in the original's order)."""
    shift = 0 if "3697c.no_shift" in MUT else 4
    v1 = m.lh(slc + 14); v0 = m.lw(p + 0); a0 = q(0)
    qx = (u(v1 << shift), u(v0 - a0))
    v1 = m.lh(slc + 16); v0 = m.lw(p + 4); a0 = q(4)
    qy = (u(v1 << shift), u(v0 - a0))
    a0 = q(8); v1 = m.lh(slc + 18); v0 = m.lw(p + 8)
    qz = (u(v1 << shift), u(v0 - a0))
    lo, hi = lohi(*qz)
    spill(lo, hi)
    return u(mid(*qz) + u(mid(*qy) + mid(*qx)))


def m_3697c(m):
    """0x8003697C AlongFromAnchor(oldDir, newDir, slice, point) -> distance along row 2."""
    R = m.R
    fr = u(R["sp"] - 24)
    a0, a1, t3, t4 = R["a0"], R["a1"], R["a2"], R["a3"]
    start = s(a1) > 0 or a0 == a1                                   # 0x80036984 / 0x8003698C
    if "3697c.anchor" in MUT:
        start = True
    if start:
        m.c("anchor=slice start")
        m.sw(fr + 0, m.lw(t3 + 20)); m.sw(fr + 4, m.lw(t3 + 24)); m.sw(fr + 8, m.lw(t3 + 28))
    else:
        m.c("anchor=slice end (direction flipped to <= 0)")
        shift = 0 if "3697c.no_shift" in MUT else 4
        ry = m.lh(t3 + 16); chord = m.lw(t3 + 32)                  # 0x800369B8
        py = (u(ry << shift), chord)
        lo, hi = lohi(*py); m.sw(fr + 16, lo); m.sw(fr + 20, hi)
        rx = m.lh(t3 + 14)
        px = (u(rx << shift), chord)
        a0v = m.lw(t3 + 24); t0 = m.lw(t3 + 20)
        m.sw(fr + 4, u(mid(*py) + a0v))
        rz = m.lh(t3 + 18)
        pz = (u(rz << shift), chord)
        m.sw(fr + 0, u(mid(*px) + t0))
        lo, hi = lohi(*pz); m.sw(fr + 16, lo); m.sw(fr + 20, hi)
        m.sw(fr + 8, u(mid(*pz) + m.lw(t3 + 28)))
    # 0x80036A68: dot(row2 << 4, point - anchor) >> 16
    return dot_row2(m, t3, t4, lambda k: m.lw(fr + k),
                    lambda lo, hi: (m.sw(fr + 16, lo), m.sw(fr + 20, hi)))


def m_36b14(m):
    """0x80036B14 RoadSliceSearch(p = e+0xAC, cursor, point) -> slice."""
    R = m.R
    sp = u(R["sp"] - 224)
    s4, s5, point = R["a1"], R["a0"], R["a2"]
    gp = R["gp"]
    m.sw(sp + 232, point)                                           # 0x80036B44 (a2 home)
    m.sw(sp + 84, 0)
    m.sw(sp + 24, 0)                                                # local cursor "not copied"
    m.sw(sp + 32, m.lw(s4 + 8))
    m.sw(sp + 36, m.lw(s4 + 12))
    s3 = m.lw(s4 + 12)
    m.sw(sp + 168, 0)
    s0 = dot_row2(m, s3, point, lambda k: m.lw(s3 + 20 + k),
                  lambda lo, hi: (m.sw(sp + 176, lo), m.sw(sp + 180, hi)))
    s8 = 0

    def tag(s3v):
        v0 = m.lw(s5 + 216)
        if v0 & 0x80:
            if m.lh(s3v + 50) != 0:
                m.c("tag: slice +0x32 != 0 stored at e[+0x33C]")
                h = m.lhu(s5 + 0)
                base = m.lw(POOL0)
                m.sw(u(base + 1096 * h + 828), s3v)

    def commit_local():
        # 0x80036D60: copy the local cursor back, or just its slice when the sub-object held
        v1 = m.lw(s4 + 8); v0 = m.lw(sp + 32)
        if v1 == v0:
            m.c("commit: slice only")
            m.sw(s4 + 12, m.lw(sp + 36))
        else:
            m.c("commit: whole cursor (memcpy)")
            m.call(0x8001E0B4, s4, u(sp + 24), 32)

    def neighbour(s1):
        """0x80036C8C/0x80036E2C (the copy) is done by the caller; this is the fn-pointer call
        and the pick.  Returns the new slice."""
        v0 = m.lw(sp + 168)
        fn = 0x80037A30 if s(v0) > 0 else 0x80037FBC
        m.c("neighbours: " + ("forward 0x80037A30" if fn == 0x80037A30 else
                              "backward 0x80037FBC"))
        m.sw(sp + 16, 3)
        n = m.call(fn, s5, u(sp + 24), u(sp + 56), u(sp + 152))
        m.sw(sp + 16, u(sp + 24))
        m.sw(sp + 20, u(sp + 168))
        return m.call(0x80039048, s5, u(sp + 56), u(sp + 152), n)

    if s0 == 0:                                                     # 0x80036C1C
        m.c("point exactly on the slice start: no search")
        v1 = m.lw(s5 + 216)
        m.sw(s5 + 216, v1 & u(-129))
        return s3
    d = 1 if s(s0) > 0 else u(-1)
    m.sw(sp + 168, d)
    v1 = m.lw(sp + 168)
    v0 = m.lw(sp + 36)
    s1 = s3
    m.sw(sp + 152, v1)
    a0 = m.lh(v0 + 0)
    s2 = v1
    stepped = False
    if a0 != 0:
        cnt = m.lh(m.lw(sp + 32) + 10)
        if a0 != cnt - 1:
            m.c("first step: inside the sub-object")
            s1 = u(s3 + SLICE * s(s2))
            m.sw(sp + 36, s1)
            stepped = True
    if not stepped:
        m.c("first step: sub-object end, ask the neighbours")
        if m.lw(sp + 24) == 0:
            m.call(0x8001E0B4, u(sp + 24), s4, 32)
            m.sw(sp + 36, s1)
        s1 = neighbour(s1)
    # 0x80036D18
    s0 = m.call(0x8003697C, s2, m.lw(sp + 168), s1, m.lw(sp + 232))
    if "36b14.first_diff_vs_prev" in MUT:
        s8 = s0
    while True:                                                     # 0x80036D38
        v0 = m.lw(sp + 168)
        accept = (s(v0) > 0 and s(s0) >= 0) or (s(v0) < 0 and s(s0) < 0)
        if not accept:
            m.c("loop: candidate not passed -> exit")
            break
        m.c("loop: candidate passed -> commit")
        commit_local()
        s3 = m.lw(s4 + 12)  # (0x80036D90, after the e[+0x184] load)
        # the original loads e[+0x184] first, then s3; the set of loads is what is compared
        tag(s3)
        v1 = m.lh(m.lw(sp + 36) + 0)
        s2 = m.lw(sp + 168)
        stepped = False
        if v1 != 0:
            cnt = m.lh(m.lw(sp + 32) + 10)
            if v1 != cnt - 1:
                m.c("loop step: inside the sub-object")
                s1 = u(s1 + SLICE * s(s2))
                m.sw(sp + 36, s1)
                stepped = True
        if not stepped:
            if m.lw(sp + 24) == 0:
                m.call(0x8001E0B4, u(sp + 24), s4, 32)
                m.sw(sp + 36, s1)
            if m.call(0x800394F0, u(sp + 24), m.lw(sp + 168)) != 0:
                m.c("loop: next object missing -> exit")
                gs = m.lw(GAME_STATE)
                h = m.lhu(s5 + 0)
                if h < m.lw(gs + 48) or "36b14.human_flag" in MUT:
                    m.c("loop: next object missing, human player -> gp+2292 = 1")
                    m.sw(u(gp + 2292), 1)
                break
            s1 = neighbour(s1)
        s0 = m.call(0x8003697C, s2, m.lw(sp + 168), s1, m.lw(sp + 232))
        x = u(s0 - s8)
        sg = u(s(x) >> 31)
        ad = s(u(x + sg) ^ sg)
        s8 = s0
        lim = 132 if "36b14.threshold" in MUT else 131
        if not ad >= lim:
            m.c("loop: distance settled (|d - d_prev| < 131) -> exit")
            break
    # 0x80036F24
    if s(s2) < 0 and s(m.lw(sp + 168)) < 0 and s(s0) > 0 and "36b14.exit_commit" not in MUT:
        a0 = m.lw(sp + 36)
        if s(s0) < s(m.lw(a0 + 32)):
            m.c("exit: reverse search, point inside the last candidate -> commit it")
            v1 = m.lw(s4 + 8); v0 = m.lw(sp + 32)
            if v1 == v0:
                m.sw(s4 + 12, a0)
            else:
                m.call(0x8001E0B4, s4, u(sp + 24), 32)
            s3 = m.lw(s4 + 12)
            tag(s3)
    v1 = m.lw(s5 + 216)
    m.sw(s5 + 216, v1 & u(-129))
    return s3


def _first_or_last(m, subt, first, slct):
    v1 = m.lh(subt + 8)
    if first:
        return u(slct + SLICE * v1)
    return u(slct + SLICE * (v1 - 1 + m.lh(subt + 10)))


def m_neigh(m, fwd: bool):
    """0x80037A30 (fwd) / 0x80037FBC (bwd): the neighbour candidates at a sub-object end."""
    R = m.R
    sp = u(R["sp"] - 88)
    a0, a1, a2, a3 = R["a0"], R["a1"], R["a2"], R["a3"]
    tag = "A30" if fwd else "FBC"
    if not fwd:
        m.sw(sp + 88, a0)
    m.sw(sp + 92, a1); m.sw(sp + 96, a2); m.sw(sp + 100, a3)
    Cnext = 0x8003C840 if fwd else 0x8003C948
    s8 = 0; s7 = 0
    if fwd:
        s0 = m.lw(a1 + 0); s2 = m.lw(a1 + 4); s4 = m.lw(a1 + 8); s1 = m.lw(a1 + 28)
        t0 = m.lw(a1 + 12); m.sw(sp + 32, t0)
        cnt = m.lh(s4 + 10); idx = m.lh(t0 + 0)
        lim = cnt if "37a30.last_is_step" in MUT else cnt - 1
        obj, piece, subt, turn, slc = s0, s2, s4, s1, 0
        step = idx < lim
    else:
        s1 = m.lw(a1 + 0); s3 = m.lw(a1 + 4); s6 = m.lw(a1 + 8)
        t0 = m.lw(a1 + 12); m.sw(sp + 32, t0)
        idx = m.lh(t0 + 0); s2 = m.lw(a1 + 28)
        obj, piece, subt, turn, slc = s1, s3, s6, s2, 0
        step = idx != 0
    ent = a0            # e + 0xAC (only passed through to 0x8003C840/0x8003C948)
    if step:
        m.c(tag + ": step inside the sub-object")
        piece, slc, s7 = 0, u(t0 + (SLICE if fwd else -SLICE)), 1
    else:
        kind = m.lh(obj + 16)
        old = obj
        if kind == 0:
            m.c(tag + ": road-piece object, go to the next object")
            obj = m.call(Cnext, ent if fwd else m.lw(sp + 88), obj, u(-1))
            if obj != 0:
                if m.lh(obj + 16) == 0:
                    m.c(tag + ": next is a road piece: its first GRPT record")
                    piece = m.lw(obj + 44)
                else:
                    m.c(tag + ": next is a junction: FindRoadPiece(owner)")
                    piece = m.call(0x80039C90, obj, m.lw(old + 8))
                if piece != 0:
                    s7 = 1
        else:
            node = m.call(0x80039AFC, m.lw(obj + 0))
            if node != 0:
                if m.lw(obj + 12) == 1:
                    m.c(tag + ": junction approach half (half == 1)")
                    if fwd:
                        arm = m.call(0x8003A37C, node, m.lw(piece + 12))
                        if arm != 0:
                            obj = m.call(Cnext, ent, obj, m.lw(piece + 12))
                            if obj != 0:
                                if m.lh(obj + 16) == 0:
                                    piece = m.lw(obj + 44)
                                else:
                                    piece = m.call(0x80039C90, obj, m.lh(arm + 4))
                                if piece != 0:
                                    s7 = 1
                    else:
                        arm = m.call(0x8003A37C, node, m.lw(piece + 12))
                        if arm != 0:
                            obj = m.call(Cnext, m.lw(sp + 88), obj, m.lw(piece + 12))
                        if obj != 0 and arm != 0:
                            if m.lh(obj + 16) == 0:
                                piece = m.lw(obj + 44)
                            else:
                                piece = m.call(0x80039C90, obj, m.lh(arm + 4))
                            if piece != 0:
                                s7 = 1
                elif m.lh(piece + 2) == 0:
                    arm = m.call(0x8003A37C, node, m.lw(piece + 12))
                    if arm != 0:
                        if (m.lh(arm + 2) > 0) == fwd:
                            m.c(tag + ": junction stub, arm leads on: next object")
                            ent_a0 = ent if fwd else m.lw(sp + 88)
                            a2v = m.lw(piece + 12)
                            piece = 0
                            obj = m.call(Cnext, ent_a0, obj, a2v)
                            if obj != 0:
                                piece = m.call(0x80039C90, obj, m.lh(arm + 4))
                            if piece != 0:
                                s7 = 1
                        else:
                            m.c(tag + ": junction stub, arm leads into the junction: turn table")
                            jn = m.call(0x80039B60, m.lw(obj + 0))
                            m.sw(sp + 36, jn)
                            if jn != 0:
                                cnt = m.call(0x80039BB0, jn, m.lh(arm + 4), u(sp + 16),
                                             m.lw(sp + 104))
                                if (s(cnt) > 0 and s(cnt) < s(m.lw(sp + 104)) and s8 < s(cnt)):
                                    dirs = m.lw(sp + 100)
                                    for i in range(s(cnt)):
                                        link = m.lw(u(sp + 16 + 4 * i))
                                        m.sw(sp + 40, cnt)
                                        tgt = m.call(0x80039C38, link)
                                        cnt = m.lw(sp + 40)
                                        if tgt == 0:
                                            m.c(tag + ": turn without target, skipped")
                                            continue
                                        m.c(tag + ": turn candidate")
                                        pi = m.lh(tgt + 2)
                                        pc_ = u(m.lw(obj + 44) + 32 * pi)
                                        v1 = m.lh(pc_ + 20)
                                        v0 = m.lh(tgt + 4)
                                        ldir = m.lh(link + 6)
                                        sb_ = u(m.lw(obj + 48) + 28 * (v1 + v0))
                                        v1 = m.lh(sb_ + 8)
                                        td = m.lh(tgt + 8)
                                        sl = m.lw(obj + 52)
                                        if ldir > 0:
                                            first = td > 0
                                        else:
                                            first = td <= 0
                                        if not first:
                                            v1 = v1 - 1 + m.lh(sb_ + 10)
                                        slc_ = u(sl + SLICE * v1)
                                        td = m.lh(tgt + 8)
                                        if ldir > 0 or ("37fbc.dir_sign" in MUT and not fwd):
                                            m.sw(dirs, td)
                                        else:
                                            m.sw(dirs, u(-td))
                                        out = u(m.lw(sp + 96) + 32 * s8)
                                        cin = m.lw(sp + 92)
                                        m.sw(out + 20, m.lw(cin + 20))
                                        dirs = u(dirs + 4)
                                        v0 = m.lw(cin + 16)
                                        s8 += 1
                                        m.sw(out + 0, obj); m.sw(out + 4, pc_)
                                        m.sw(out + 8, sb_); m.sw(out + 12, slc_)
                                        jnv = m.lw(sp + 36)
                                        s7 = 1
                                        m.sw(out + 28, link); m.sw(out + 24, jnv)
                                        m.sw(out + 16, v0)
                                        piece, subt, slc = pc_, sb_, slc_
                else:
                    m.c(tag + ": junction core (GRPT +2 != 0): leave by the current turn")
                    piece = 0
                    road = None
                    if fwd:
                        if turn != 0:
                            tgt = m.call(0x80039C38, turn)
                            if tgt != 0:
                                ldir = m.lh(turn + 6)
                                if ldir > 0:
                                    td = m.lh(tgt + 8); road = m.lh(turn + 8)
                                    if td > 0:
                                        road = m.lh(turn + 10)
                                else:
                                    td = m.lh(tgt + 8); road = m.lh(turn + 10)
                                    if td > 0:
                                        road = m.lh(turn + 8)
                    else:
                        if turn != 0:
                            tgt = m.call(0x80039C38, turn)
                            ldir = m.lh(turn + 6)
                            if ldir > 0:
                                td = m.lh(tgt + 8); road = m.lh(turn + 10)
                                if td > 0:
                                    road = m.lh(turn + 8)
                            else:
                                td = m.lh(tgt + 8); road = m.lh(turn + 8)
                                if td > 0:
                                    road = m.lh(turn + 10)
                        else:
                            m.c(tag + ": junction core with no turn: NodeArm(node, -1)")
                            road = -1
                    if road is not None:
                        arm = m.call(0x8003A37C, node, road)
                        if arm != 0:
                            piece = m.call(0x80039C90, obj, m.lh(arm + 4))
                        if piece != 0:
                            v1 = m.lh(piece + 20)
                            sl = m.lw(obj + 52)
                            subt = u(m.lw(obj + 48) + 28 * v1)
                            ad = m.lh(arm + 2)
                            first = (ad >= 0) if (fwd or "37fbc.core_first" in MUT) else (ad > 0)
                            slc = _first_or_last(m, subt, first, sl)
                            ad = m.lh(arm + 2)
                            t0p = m.lw(sp + 100)
                            s7 = 2
                            m.sw(t0p, ad)
    # TAIL 0x80037E80 / 0x80038408
    if s7 == 0:
        m.c(tag + ": no candidate: return the input slice")
        cin = m.lw(sp + 92)
        v1 = m.lw(cin + 8)
        out = u(m.lw(sp + 96) + 32 * s8)
        m.sw(out + 8, v1)
        cin = m.lw(sp + 92)
        v0 = m.lw(cin + 12)
        s8 += 1
        m.sw(out + 12, v0)
        return s8
    if s8 != 0:
        return s8
    out = m.lw(sp + 96)
    if piece == 0:
        cin = m.lw(sp + 92)
        v0 = m.lw(cin + 8)
        same = subt == v0 or slc == m.lw(sp + 32)
        if subt == v0:
            same = True
        if same:
            m.c(tag + ": partial candidate (sub-object and slice only)")
            m.sw(out + 8, v0)
            m.sw(out + 12, slc)
            return 1
    if s7 != 2:
        m.c(tag + ": whole candidate, entering the next piece's first sub-object")
        v1 = m.lh(piece + 20)
        sl = m.lw(obj + 52)
        t0p = m.lw(sp + 100)
        subt = u(m.lw(obj + 48) + 28 * v1)
        v1 = m.lh(subt + 8)
        if fwd:
            m.sw(t0p, 1)
            slc = u(sl + SLICE * v1)
        else:
            a0v = m.lh(subt + 10)
            m.sw(t0p, u(-1))
            slc = u(sl + SLICE * (v1 + a0v) - SLICE)
    cin = m.lw(sp + 92)
    m.sw(out + 20, m.lw(cin + 20))
    v0 = m.lw(cin + 16)
    m.sw(out + 0, obj); m.sw(out + 4, piece); m.sw(out + 8, subt); m.sw(out + 12, slc)
    m.sw(out + 24, 0); m.sw(out + 28, 0); m.sw(out + 16, v0)
    return 1


def m_37a30(m):
    return m_neigh(m, True)


def m_37fbc(m):
    return m_neigh(m, False)


def m_39048(m):
    """0x80039048 PickNeighbour(p, cand[], dirs[], n, cursorOut, &dirOut) -> slice."""
    R = m.R
    sp = u(R["sp"] - 56)
    a0, s7, a2, s6 = R["a0"], R["a1"], R["a2"], R["a3"]
    s0 = u(-1)
    s8 = m.lw(sp + 72)
    pool = 6
    s3 = 0
    m.sw(sp + 64, a2)
    if a0 != 0:
        h = m.lhu(a0 + 0)
        pool = h >> 5
        if pool == 0:
            s3 = u(m.lw(POOL0) + 1096 * h)

    def forget():
        m.sb(s3 + 947, 0xFF); m.sw(s3 + 948, 0); m.sw(s3 + 952, 0)

    def take_first(write_dir_always: bool):
        v1 = m.lw(s8 + 8); v0 = m.lw(s7 + 8)
        if v1 == v0:
            m.sw(s8 + 12, m.lw(s7 + 12))
            return False
        m.call(0x8001E0B4, s8, s7, 32)
        return True

    if m.lw(s7 + 28) == 0:                                          # 0x800390CC
        m.c("pick: candidate 0 has no turn record")
        if s3 != 0 and m.lw(s3 + 356) != 0:
            m.c("pick: entity was mid-turn -> forget the junction choice")
            forget()
        take_first(False)
        d = m.lw(m.lw(sp + 64))
        if d == 1 or d == 0xFFFFFFFF:
            m.sw(m.lw(sp + 76), d)
        return m.lw(s8 + 12)
    if s(s6) < 2:
        m.c("pick: single candidate")
        copied = take_first(True)
        if copied or "39048.y_writes_dir" in MUT:
            m.sw(m.lw(sp + 76), m.lw(m.lw(sp + 64)))
        return m.lw(s8 + 12)
    if pool == 0:
        m.c("pick: pool 0 (bike)")
        if s3 != 0:
            gs = m.lw(GAME_STATE)
            human = m.lhu(s3 + 172) < m.lw(gs + 48)
            use_mem = True
            if not human:
                if (m.lbu(m.lw(s3 + 1084) + 1) & 0xF) == 2:
                    use_mem = False
            if use_mem and "39048.no_memory" not in MUT:
                v1 = m.lw(s3 + 948)
                if v1 != 0:
                    if m.lh(v1 + 2) == m.lh(m.lw(s7 + 24) + 2):
                        m.c("pick: same junction as remembered -> reuse +0x3B3")
                        s0 = u(m.lb(s3 + 947))
                    else:
                        m.c("pick: different junction -> forget")
                        forget()
            if s0 == 0xFFFFFFFF:
                m.c("pick: ask AiJunctionChoice 0x8003CCA0")
                s0 = m.call(0x8003CCA0, s3, s7, s6)
    elif pool == 3:
        m.c("pick: pool 3 (traffic)")
        s4 = u(-1); s5 = u(-1)
        h = m.lhu(a0 + 0)
        key = m.lw(m.lw(s7 + 4) + 12)
        veh = u(POOL3 + ((h & 0x1F) << 9))
        if m.call(0x8003F3B4, key) != 0:
            gs = m.lw(GAME_STATE)
            if (m.lbu(gs + 4) & 1) and m.lw(veh + 180) == 0:
                v1 = m.lbu(veh + 511)
                if s(v1) < s(m.lw(BIKE_COUNT)):
                    b = u(m.lw(POOL0) + 1096 * v1)
                    if b != 0:
                        r = m.lw(b + 360)
                        if (r >> 16) == 0:
                            s5 = r
            s2 = s7
            for i in range(max(0, s(s6))):
                gs = m.lw(GAME_STATE)
                rt = m.lh(m.lw(s2 + 28) + 10)
                fl = m.lbu(gs + 4)
                if fl & 0x10:
                    if (m.call(0x8003F5D0, rt) != 0 and not (u(rt - 11) < 2)
                            and rt != 20 and rt != 26):
                        s4 = i
                        break
                else:
                    if (s5 != 0xFFFFFFFF and u(rt) == s5) or m.call(0x8003F5D0, rt) != 0:
                        s4 = i
                        break
                s2 = u(s2 + 32)
        if s4 == 0xFFFFFFFF or s4 == -1:
            gs = m.lw(GAME_STATE)
            if m.lbu(gs + 4) & 0x10:
                s4 = u(-1)
                for i in range(max(0, s(s6))):
                    rt = m.lhu(m.lw(u(s7 + 32 * i) + 28) + 10)
                    if u(rt - 11) < 2 or s16(rt) == 20 or s16(rt) == 26:
                        continue
                    s4 = i
                    break
                if s4 == 0xFFFFFFFF:
                    m.c("pick: pool 3, no admissible road -> index -1")
            else:
                s4 = m.call(0x8001FC58) % s6
        s0 = u(s4)
    else:
        m.c("pick: other pool -> Rand() % n")
        s0 = m.call(0x8001FC58) % s6
    # 0x800393A0
    idx = s(s0)
    m.call(0x8001E0B4, s8, u(s7 + 32 * idx), 32)
    d = m.lw(u(m.lw(sp + 64) + 4 * idx))
    m.sw(m.lw(sp + 76), d)
    if s3 != 0 and (m.lw(s3 + 948) == 0 or "39048.remember_always" in MUT):
        m.c("pick: remember this junction choice")
        m.sb(s3 + 947, s0)
        m.sw(s3 + 948, m.lw(s8 + 24))
        m.sw(s3 + 952, m.lw(s8 + 28))
    return m.lw(s8 + 12)


def m_386dc(m):
    """0x800386DC RoadLookAhead(e, ahead, along, dir, cursor, outPoint, outCursor)."""
    R = m.R
    sp = u(R["sp"] - 312)
    e, s5, along, a3 = R["a0"], R["a1"], R["a2"], R["a3"]
    cursor = m.lw(sp + 328)
    s4 = cursor
    s3 = m.lw(sp + 336)
    m.sw(sp + 252, e)
    t0 = m.lw(s4 + 12)
    m.sw(sp + 256, t0)
    if s(a3) > 0:
        m.sw(sp + 248, 1)
        s6 = u(m.lw(t0 + 32) - along)
    else:
        m.sw(sp + 248, u(-1))
        s6 = along
    outp = lambda: m.lw(sp + 332)  # noqa: E731

    # --- phase 1: walk `ahead` units from the current slice (0x80038748)
    if s(s6) < s(s5):
        e2 = u(m.lw(sp + 252) + 172)                                # 0x80038754, once
    while s(s6) < s(s5):
        m.c("walk: one slice")
        a0 = m.lw(s4 + 12)
        v1 = m.lh(a0 + 0)
        inside = False
        if v1 != 0:
            if v1 != m.lh(m.lw(s4 + 8) + 10) - 1:
                inside = True
        if inside:
            d = m.lw(sp + 248)
            m.sw(s4 + 12, u(a0 + SLICE * s(d)))
        else:
            m.c("walk: sub-object end")
            m.sw(sp + 232, m.lw(sp + 248))
            if s4 == m.lw(sp + 328):
                m.c("walk: first boundary -> private copy of the cursor at sp+72")
                s4 = u(sp + 72)
                m.call(0x8001E0B4, s4, m.lw(sp + 328), 32)
            if m.call(0x800394F0, s4, m.lw(sp + 248)) != 0:
                m.c("walk: next object missing -> extrapolate along the tangent, NO restore")
                sl = m.lw(s4 + 12)
                d = m.lw(sp + 248)
                a2 = u(s5 - s6) if s(d) > 0 else u(s6 - s5)
                m.call(0x8002EAD8, u(sl + 20), u(sl + 14), a2, m.lw(sp + 332))
                if s3 != 0:
                    m.call(0x8001E0B4, s3, s4, 32)
                if "386dc.dead_end_restore" in MUT:
                    m.sw(u(m.lw(sp + 328) + 12), m.lw(sp + 256))
                return None
            v0 = m.lw(sp + 248)
            fn = 0x80037A30 if s(v0) > 0 else 0x80037FBC
            m.sw(sp + 16, 3)
            n = m.call(fn, e2, s4, u(sp + 136), u(sp + 232))
            m.sw(sp + 16, s4)
            m.sw(sp + 20, u(sp + 248))
            m.call(0x80039048, e2, u(sp + 136), u(sp + 232), n)
        s6 = u(s6 + m.lw(m.lw(s4 + 12) + 32))
    # --- phase 2 (0x800388C4): the slice A the target lies in, and its successor B
    m.sw(sp + 260, s4)
    if s3 != 0:
        m.call(0x8001E0B4, s3, s4, 32)
    t1 = m.lw(sp + 248)
    s8 = m.lw(s4 + 12)                                              # A
    m.sw(sp + 264, t1)                                              # dir0
    s3 = m.lw(s8 + 32)                                              # A.chord
    rl_fwd, rl_bwd = (39, 38) if "386dc.racing_line" in MUT else (38, 39)
    if s(t1) > 0:
        s6 = u(s3 - u(s6 - s5))
        s7 = u(m.lb(s8 + rl_fwd) << 13)
    else:
        s6 = u(s6 - s5)
        s7 = u(m.lb(s8 + rl_bwd) << 13)
    a0 = m.lw(s4 + 12)
    v1 = m.lh(a0 + 0)
    m.sw(sp + 248, 1)
    inside = False
    if v1 != 0:
        if v1 != m.lh(m.lw(s4 + 8) + 10) - 1:
            inside = True
    if inside:
        m.c("B: next slice inside the sub-object")
        m.sw(s4 + 12, u(a0 + SLICE))
    else:
        m.c("B: sub-object end -> neighbours (forward)")
        s4 = u(sp + 104)
        m.sw(sp + 232, m.lw(sp + 248))
        m.call(0x8001E0B4, s4, m.lw(sp + 260), 32)
        v0 = m.lw(sp + 248)
        fn = 0x80037A30 if s(v0) > 0 else 0x80037FBC
        m.sw(sp + 16, 3)
        e2 = u(m.lw(sp + 252) + 172)
        n = m.call(fn, e2, s4, u(sp + 136), u(sp + 232))
        m.sw(sp + 16, s4)
        m.sw(sp + 20, u(sp + 248))
        m.call(0x80039048, e2, u(sp + 136), u(sp + 232), n)
    v0 = m.lw(sp + 248)
    s0 = m.lw(s4 + 12)                                              # B
    if s(v0) > 0:
        t1 = m.lw(sp + 264)
        s5 = u(m.lb(s0 + (rl_fwd if s(t1) > 0 else rl_bwd)) << 13)
    else:
        m.c("B: reached against its own direction (flip): distance A->B += B.chord")
        t0 = m.lw(sp + 264)
        s5 = u(m.lb(s0 + (rl_bwd if s(t0) > 0 else rl_fwd)) << 13)
        s3 = u(s3 + m.lw(s0 + 32))
    first_half = (s(s6) <= (s(s3) >> 1)) if "386dc.half" in MUT else (s(s6) < (s(s3) >> 1))
    quad = False
    if first_half:
        m.c("interp: first half of A->B")
        m.call(0x8002EAD8, u(s0 + 20), u(s0 + 2), s5, u(sp + 56))    # P_B
        m.call(0x8002EAD8, u(s8 + 20), u(s8 + 2), s7, u(sp + 40))    # P_A
        if s(s6) >= 0:
            m.c("interp: linear P_A -> P_B")
            s6 = sdiv(m, s6, s3)
            a0, a1 = u(sp + 40), u(sp + 56)
        else:
            m.c("interp: target before A (quadratic arm, previous slice C)")
            quad = True
            s6 = quad_t(m, s6, s3, u(sp + 40), u(sp + 56), first=True)
            s4 = m.lw(sp + 260)
            m.sw(s4 + 12, s8)
            a1 = s8
            v1 = m.lh(a1 + 0)
            m.sw(sp + 248, u(-1))
            inside = False
            if v1 != 0:
                if v1 != m.lh(m.lw(s4 + 8) + 10) - 1:
                    inside = True
            if inside:
                m.sw(s4 + 12, u(a1 - SLICE))
            else:
                s4 = u(sp + 104)
                m.sw(sp + 232, m.lw(sp + 248))
                m.call(0x8001E0B4, s4, m.lw(sp + 260), 32)
                v0 = m.lw(sp + 248)
                fn = 0x80037A30 if s(v0) > 0 else 0x80037FBC
                m.sw(sp + 16, 3)
                e2 = u(m.lw(sp + 252) + 172)
                n = m.call(fn, e2, s4, u(sp + 136), u(sp + 232))
                m.sw(sp + 16, s4)
                m.sw(sp + 20, u(sp + 248))
                m.call(0x80039048, e2, u(sp + 136), u(sp + 232), n)
            v1 = m.lw(sp + 248)
            if s(v1) < 0:
                t0 = m.lw(sp + 264)
                s0 = m.lw(s4 + 12)
                s2 = u(m.lb(s0 + (rl_fwd if s(t0) > 0 else rl_bwd)) << 13)
            else:
                m.c("interp: C reached against its direction")
                if m.lh(m.lw(s4 + 8) + 10) >= 2:
                    m.sw(s4 + 12, u(m.lw(s4 + 12) + SLICE))
                else:
                    m.c("interp: C in a one-slice sub-object: a DEAD neighbour call")
                    fn = 0x80037A30 if s(v1) > 0 else 0x80037FBC
                    m.sw(sp + 16, 3)
                    m.call(fn, u(m.lw(sp + 252) + 172), s4, u(sp + 136), u(sp + 232))
                v0 = m.lw(sp + 248)
                t0 = m.lw(sp + 264)
                s0 = m.lw(s4 + 12)
                x = u(v0 ^ t0)
                v1b = m.lbu(s0 + 38)
                if s(x) < 0:
                    s2 = u(s(u(v1b << 24)) >> 11)
                else:
                    s2 = u(m.lb(s0 + 39) << 13)
            m.call(0x8002EAD8, u(s0 + 20), u(s0 + 2), s2, u(sp + 24))  # P_C
    else:
        m.c("interp: second half of A->B")
        m.call(0x8002EAD8, u(s8 + 20), u(s8 + 2), s7, u(sp + 24))    # P_A
        m.call(0x8002EAD8, u(s0 + 20), u(s0 + 2), s5, u(sp + 40))    # P_B
        s6 = u(s3 - s6)
        if s(s6) >= 0:
            m.c("interp: linear P_B -> P_A")
            s6 = sdiv(m, s6, s3)
            a0, a1 = u(sp + 40), u(sp + 24)
        else:
            m.c("interp: target beyond B (quadratic arm, next slice D)")
            quad = True
            s6 = quad_t(m, s6, s3, u(sp + 40), u(sp + 24), first=False)
            a0 = m.lw(s4 + 12)
            m.sw(sp + 260, s4)
            v1 = m.lh(a0 + 0)
            s3 = m.lw(sp + 248)
            inside = False
            if v1 != 0:
                if v1 != m.lh(m.lw(s4 + 8) + 10) - 1:
                    inside = True
            if inside:
                m.sw(s4 + 12, u(a0 + SLICE * s(s3)))
            else:
                s4 = u(sp + 104)
                m.sw(sp + 232, m.lw(sp + 248))
                m.call(0x8001E0B4, s4, m.lw(sp + 260), 32)
                v0 = m.lw(sp + 248)
                fn = 0x80037A30 if s(v0) > 0 else 0x80037FBC
                m.sw(sp + 16, 3)
                e2 = u(m.lw(sp + 252) + 172)
                n = m.call(fn, e2, s4, u(sp + 136), u(sp + 232))
                m.sw(sp + 16, s4)
                m.sw(sp + 20, u(sp + 248))
                m.call(0x80039048, e2, u(sp + 136), u(sp + 232), n)
            v1 = 0
            if s(s3) < 0:
                v1 = 1 if 0 < s(m.lw(sp + 248)) else 0
            s0 = u(m.lw(s4 + 12) + SLICE * v1)
            a2 = u(m.lb(s0 + 38) << 13)
            m.call(0x8002EAD8, u(s0 + 20), u(s0 + 2), a2, u(sp + 56))  # P_D
    if not quad:
        m.sw(sp + 16, s6)
        m.call(0x8002E6F8, a0, a1, outp(), u(0x10000 - s6))
    else:
        # 0x80038F28: uniform quadratic B-spline over sp+24, sp+40, sp+56
        s7 = m.call(0x8001FC90, s6, s6)
        s5b = u(s(s7) >> 1)
        s2 = u(s5b + 0x8000 - s6)
        s7 = u(s6 - u(s7 - 0x8000))
        for k in range(3):
            a = m.call(0x8001FC90, s2, m.lw(sp + 24 + 4 * k))
            b = m.call(0x8001FC90, s7, m.lw(sp + 40 + 4 * k))
            c = m.call(0x8001FC90, s5b, m.lw(sp + 56 + 4 * k))
            m.sw(u(m.lw(sp + 332) + 4 * k), u(a + b + c))
    if "386dc.no_restore" not in MUT:
        m.sw(u(m.lw(sp + 328) + 12), m.lw(sp + 256))                # 0x80039014
    return None


def sdiv(m, n, d):
    """0x80038CA0 / 0x80038EC0: signed n/d through the unsigned FixDiv."""
    if s(n) > 0:
        if s(d) > 0:
            return m.call(0x80010028, n, d)
        return u(-m.call(0x80010028, n, u(-d)))
    if s(d) > 0:
        return u(-m.call(0x80010028, u(-n), d))
    return m.call(0x80010028, u(-n), u(-d))


def quad_t(m, s6, s3, pa, pb, first):
    """0x80038A84 / 0x80038D30: the out-of-range arm.  s3 is zeroed by FixDiv(0, s3) before it
    is used as a divisor, so t = 0x8000 +- FixDiv(-s6, 0) whenever the chord was positive."""
    if s(s3) > 0:
        s3n = u(-m.call(0x80010028, 0, s3))
        m.sw(u(m.R["sp"] - 312 + 16), s3n)
        m.call(0x8002E6F8, pa, pb, pb, u(0x10000 - s3n))
        s3 = 0
    v = m.call(0x80010028, u(-s6), u(-s3))
    return u(v + 0x8000) if first else u(0x8000 - v)


# ---------------------------------------------------------------------------
# A minimal R3000 executor for the ORIGINAL bytes (integer subset only; any COP2, syscall,
# overflow trap or access outside main RAM stops it).  Used for two things only:
#   1. `verify` re-executes every natural top-level invocation with it and requires its store
#      stream to equal the oracle's, so it is itself checked against src\interp on this code;
#   2. `verify --synthetic N` drives the original on synthetic inputs built from the `quick`
#      snapshot, to reach branches no capture reached, and checks the MODELS against it.
# ---------------------------------------------------------------------------

class CpuStop(Exception):
    pass


class PyCpu:
    STOP = 0x80000180   # a return address that is never executed

    def __init__(self, ram: bytearray, regs: dict, probe_pcs: set, bodies):
        self.m = ram
        self.r = [0] * 32
        for i, n in enumerate(REG):
            self.r[i] = regs.get(n, 0) & 0xFFFFFFFF
        self.r[0] = 0
        self.hi = regs.get("hi", 0)
        self.lo = regs.get("lo", 0)
        self.probe_pcs = probe_pcs
        self.bodies = bodies
        self.seq = 0
        self.writes = []        # (seq, pc, addr, size, value)
        self.reads = []         # body reads only
        self.calls = []         # (seq, from, target, a0, a1, a2, a3, sp)
        self.probes = defaultdict(list)

    def _phys(self, a, n):
        a &= 0xFFFFFFFF
        if a % n:
            raise CpuStop(f"misaligned {n}-byte access 0x{a:08X}")
        seg = a & 0x1FFFFFFF
        if seg >= 0x800000:
            raise CpuStop(f"access outside RAM 0x{a:08X}")
        return seg & 0x1FFFFF

    def load(self, pc, a, n):
        o = self._phys(a, n)
        v = int.from_bytes(self.m[o:o + n], "little")
        for b0, b1 in self.bodies:
            if b0 <= pc < b1:
                self.reads.append((self.seq, pc, o, n, v))
                break
        return v

    def store(self, pc, a, n, v):
        o = self._phys(a, n)
        v &= (1 << (8 * n)) - 1
        self.m[o:o + n] = v.to_bytes(n, "little")
        self.writes.append((self.seq, pc, o, n, v))

    def run(self, pc: int, max_steps: int = 3_000_000):
        r = self.r
        npc = u(pc + 4)
        pending = None
        for _ in range(max_steps):
            if pc == self.STOP:
                return
            if pc in self.probe_pcs:
                self.probes[pc].append((self.seq, {REG[i]: r[i] for i in range(32)}))
            o = self._phys(pc, 4)
            w = int.from_bytes(self.m[o:o + 4], "little")
            op = w >> 26
            rs = (w >> 21) & 31
            rt = (w >> 16) & 31
            rd = (w >> 11) & 31
            sa = (w >> 6) & 31
            fn = w & 63
            imm = w & 0xFFFF
            simm = imm - 0x10000 if imm & 0x8000 else imm
            written = None
            val = 0
            newpend = None
            nnpc = u(npc + 4)
            A = r[rs]
            B = r[rt]
            if op == 0:
                if fn == 0x00: val = u(B << sa); written = rd
                elif fn == 0x02: val = B >> sa; written = rd
                elif fn == 0x03: val = u(s(B) >> sa); written = rd
                elif fn == 0x04: val = u(B << (A & 31)); written = rd
                elif fn == 0x06: val = B >> (A & 31); written = rd
                elif fn == 0x07: val = u(s(B) >> (A & 31)); written = rd
                elif fn == 0x08: nnpc = A
                elif fn == 0x09:
                    self.calls.append((self.seq, pc, A, r[4], r[5], r[6], r[7], r[29]))
                    val = u(pc + 8); written = rd; nnpc = A
                elif fn == 0x10: val = self.hi; written = rd
                elif fn == 0x11: self.hi = A
                elif fn == 0x12: val = self.lo; written = rd
                elif fn == 0x13: self.lo = A
                elif fn == 0x18:
                    p_ = s(A) * s(B)
                    self.lo = p_ & 0xFFFFFFFF; self.hi = (p_ >> 32) & 0xFFFFFFFF
                elif fn == 0x19:
                    p_ = A * B
                    self.lo = p_ & 0xFFFFFFFF; self.hi = (p_ >> 32) & 0xFFFFFFFF
                elif fn == 0x1A:
                    n_, d_ = s(A), s(B)
                    if d_ == 0:
                        self.lo = 1 if n_ < 0 else 0xFFFFFFFF; self.hi = A
                    elif n_ == -0x80000000 and d_ == -1:
                        self.lo = 0x80000000; self.hi = 0
                    else:
                        q_ = abs(n_) // abs(d_)
                        if (n_ < 0) != (d_ < 0):
                            q_ = -q_
                        self.lo = u(q_); self.hi = u(n_ - q_ * d_)
                elif fn == 0x1B:
                    if B == 0:
                        self.lo = 0xFFFFFFFF; self.hi = A
                    else:
                        self.lo = A // B; self.hi = A % B
                elif fn in (0x20, 0x22):
                    x = s(A) + s(B) if fn == 0x20 else s(A) - s(B)
                    if not -0x80000000 <= x <= 0x7FFFFFFF:
                        raise CpuStop(f"overflow trap at 0x{pc:08X}")
                    val = u(x); written = rd
                elif fn == 0x21: val = u(A + B); written = rd
                elif fn == 0x23: val = u(A - B); written = rd
                elif fn == 0x24: val = A & B; written = rd
                elif fn == 0x25: val = A | B; written = rd
                elif fn == 0x26: val = A ^ B; written = rd
                elif fn == 0x27: val = u(~(A | B)); written = rd
                elif fn == 0x2A: val = 1 if s(A) < s(B) else 0; written = rd
                elif fn == 0x2B: val = 1 if A < B else 0; written = rd
                else:
                    raise CpuStop(f"SPECIAL funct 0x{fn:02X} at 0x{pc:08X}")
            elif op == 1:
                if rt not in (0x00, 0x01, 0x10, 0x11):
                    raise CpuStop(f"REGIMM {rt} at 0x{pc:08X}")
                cond = (s(A) < 0) if rt in (0x00, 0x10) else (s(A) >= 0)
                if rt in (0x10, 0x11):
                    val = u(pc + 8); written = 31
                if cond:
                    nnpc = u(npc + (simm << 2))
            elif op in (2, 3):
                tgt = (npc & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                if op == 3:
                    self.calls.append((self.seq, pc, tgt, r[4], r[5], r[6], r[7], r[29]))
                    val = u(pc + 8); written = 31
                nnpc = tgt
            elif op == 4:
                if A == B: nnpc = u(npc + (simm << 2))
            elif op == 5:
                if A != B: nnpc = u(npc + (simm << 2))
            elif op == 6:
                if s(A) <= 0: nnpc = u(npc + (simm << 2))
            elif op == 7:
                if s(A) > 0: nnpc = u(npc + (simm << 2))
            elif op == 8:
                x = s(A) + simm
                if not -0x80000000 <= x <= 0x7FFFFFFF:
                    raise CpuStop(f"overflow trap at 0x{pc:08X}")
                val = u(x); written = rt
            elif op == 9: val = u(A + simm); written = rt
            elif op == 10: val = 1 if s(A) < simm else 0; written = rt
            elif op == 11: val = 1 if A < u(simm) else 0; written = rt
            elif op == 12: val = A & imm; written = rt
            elif op == 13: val = A | imm; written = rt
            elif op == 14: val = A ^ imm; written = rt
            elif op == 15: val = u(imm << 16); written = rt
            elif op in (0x20, 0x21, 0x23, 0x24, 0x25):
                n = {0x20: 1, 0x24: 1, 0x21: 2, 0x25: 2, 0x23: 4}[op]
                v = self.load(pc, u(A + simm), n)
                if op == 0x20 and v & 0x80:
                    v = u(v - 0x100)
                if op == 0x21 and v & 0x8000:
                    v = u(v - 0x10000)
                newpend = (rt, v)
            elif op in (0x28, 0x29, 0x2B):
                n = {0x28: 1, 0x29: 2, 0x2B: 4}[op]
                self.store(pc, u(A + simm), n, B)
            else:
                raise CpuStop(f"opcode 0x{op:02X} at 0x{pc:08X}")
            if written is not None and written != 0:
                r[written] = val
            if pending is not None and pending[0] != written and pending[0] != 0:
                r[pending[0]] = pending[1]
            pending = newpend
            self.seq += 1
            pc, npc = npc, nnpc
        raise CpuStop("step budget exhausted")


class SynthCapture(Capture):
    """The same interface as a trace capture, filled by PyCpu."""

    def __init__(self, name, ram0: bytes, cpu: PyCpu):  # noqa: super-init not wanted
        self.name = name
        self.ram0 = ram0
        self.wseq = [w[0] for w in cpu.writes]
        self.wpc = [w[1] for w in cpu.writes]
        self.waddr = [w[2] for w in cpu.writes]
        self.wsize = [w[3] for w in cpu.writes]
        self.wval = [w[4] for w in cpu.writes]
        self.body_reads = cpu.reads
        self.rseq = [x[0] for x in cpu.reads]
        self.calls = cpu.calls
        self.cseq = [c[0] for c in cpu.calls]
        self.probes = cpu.probes


def probe_set(img: E.Image) -> set:
    pl = set(CALLEES)
    for key, (entry, end, _, _) in FUNCS.items():
        pl.add(entry)
        for a in range(entry, end, 4):
            w = img.word(a)
            if E.OP(w) == 0x03 or (E.OP(w) == 0 and E.FUNCT(w) == 0x09):
                pl.add(a + 8)
            if E.OP(w) == 0 and E.FUNCT(w) == 0x08 and E.RS(w) == 31:
                pl.add(a)
    return pl


def replay_natural(cap: Capture) -> tuple[int, int, list]:
    """Re-execute every natural top-level invocation with PyCpu from the oracle's entry state and
    compare ALL its stores (callees included) with the oracle's, in order."""
    bodies = [(v[0], v[1]) for v in FUNCS.values()]
    tops = {"36b14": 0x80037014, "386dc": 0x80039040}
    n = bad = 0
    msgs = []
    for key, ex_pc in tops.items():
        entry = FUNCS[key][0]
        exs = cap.probes.get(ex_pc, [])
        exseq = [t[0] for t in exs]
        img = bytearray(cap.ram0)
        wi = 0
        for (eseq, regs) in cap.probes.get(entry, []):
            i = bisect.bisect_right(exseq, eseq)
            if i >= len(exs):
                continue
            xseq = exs[i][0]
            while wi < len(cap.wseq) and cap.wseq[wi] < eseq:
                k = cap.wsize[wi]
                o = cap.waddr[wi]
                img[o:o + k] = (cap.wval[wi] & ((1 << (8 * k)) - 1)).to_bytes(k, "little")
                wi += 1
            cpu = PyCpu(bytearray(img), dict(regs), set(), bodies)
            cpu.STOP = regs["ra"]
            n += 1
            try:
                cpu.run(entry)
            except CpuStop as ex:
                bad += 1
                msgs.append(f"{cap.name} {key} @{eseq}: PyCpu stopped: {ex}")
                continue
            j0 = bisect.bisect_left(cap.wseq, eseq)
            j1 = bisect.bisect_right(cap.wseq, xseq + 1)
            # An interrupt taken inside the invocation is not the function's and PyCpu takes no
            # interrupts: drop the kernel's stores (pc below 0x80010000) and every store between
            # the first and the last of them (the game's own vblank callback runs in between).
            ks = [cap.wseq[j] for j in range(j0, j1) if (cap.wpc[j] & 0x1FFFFFFF) < 0x10000]
            win = (ks[0], ks[-1]) if ks else (1, 0)
            want = [(cap.waddr[j], cap.wsize[j], cap.wval[j] & ((1 << (8 * cap.wsize[j])) - 1))
                    for j in range(j0, j1) if not win[0] <= cap.wseq[j] <= win[1]]
            got = [(w[2], w[3], w[4]) for w in cpu.writes]
            if got != want:
                bad += 1
                if len(msgs) < 6:
                    k = next((q for q in range(min(len(got), len(want))) if got[q] != want[q]),
                             None)
                    msgs.append(f"{cap.name} {key} @{eseq}: PyCpu {len(got)} stores, oracle "
                                f"{len(want)}, first difference at #{k}")
    return n, bad, msgs


MODELS = {"36b14": m_36b14, "3697c": m_3697c, "37a30": m_37a30, "37fbc": m_37fbc,
          "39048": m_39048, "386dc": m_386dc}


# ---------------------------------------------------------------------------
# The harness
# ---------------------------------------------------------------------------

def save_pcs(img: E.Image, entry: int, end: int) -> set[int]:
    """pcs of the prologue's callee-saved spills and of the matching epilogue reloads."""
    saved = set()
    for a in range(entry, min(end, entry + 4 * 24), 4):
        w = img.word(a)
        if E.OP(w) == 0x2B and E.RS(w) == 29 and (16 <= E.RT(w) <= 23 or E.RT(w) in (30, 31)):
            saved.add((E.RT(w), E.SIMM(w)))
    pcs = set()
    for a in range(entry, end, 4):
        w = img.word(a)
        if E.OP(w) in (0x2B, 0x23) and E.RS(w) == 29 and (E.RT(w), E.SIMM(w)) in saved:
            pcs.add(a)
    return pcs


def run_capture(cap: Capture, which, excl: dict, verbose: bool, cov: dict, stats: dict):
    fails = []
    for key in which:
        entry, end, _, retv = FUNCS[key]
        body = (entry, end)
        ex_pc = {"36b14": 0x80037014, "3697c": 0x80036B0C, "37a30": 0x80037FB4,
                 "37fbc": 0x80038548, "39048": 0x800394E8, "386dc": 0x80039040}[key]
        ents = cap.probes.get(entry, [])
        exs = cap.probes.get(ex_pc, [])
        exseq = [t[0] for t in exs]
        img = bytearray(cap.ram0)
        wi = 0
        for (eseq, regs) in ents:
            i = bisect.bisect_right(exseq, eseq)
            if i >= len(exs):
                continue
            xseq, xregs = exs[i]
            while wi < len(cap.wseq) and cap.wseq[wi] < eseq:
                n = cap.wsize[wi]
                o = cap.waddr[wi]
                img[o:o + n] = (cap.wval[wi] & ((1 << (8 * n)) - 1)).to_bytes(n, "little")
                wi += 1
            mc = Machine(cap, bytearray(img), body, eseq, xseq, regs, cov[key])
            stats[key] += 1
            try:
                v0 = MODELS[key](mc)
                if mc.ci != len(mc.calls):
                    raise Mismatch(f"model made {mc.ci} calls, original {len(mc.calls)}")
                # stores
                tw = []
                j = bisect.bisect_left(cap.wseq, eseq)
                while j < len(cap.wseq) and cap.wseq[j] <= xseq:
                    if entry <= cap.wpc[j] < end and cap.wpc[j] not in excl[key]:
                        n = cap.wsize[j]
                        tw.append((cap.waddr[j], n, cap.wval[j] & ((1 << (8 * n)) - 1)))
                    j += 1
                if tw != mc.writes:
                    k = next((q for q in range(min(len(tw), len(mc.writes)))
                              if tw[q] != mc.writes[q]), min(len(tw), len(mc.writes)))
                    g = mc.writes[k] if k < len(mc.writes) else None
                    w = tw[k] if k < len(tw) else None
                    raise Mismatch(f"store #{k}: model {fmt(g)} original {fmt(w)} "
                                   f"({len(mc.writes)} vs {len(tw)} stores)")
                # loads
                lo = bisect.bisect_left(cap.rseq, eseq)
                hi = bisect.bisect_right(cap.rseq, xseq)
                tr = {(r[2], r[3], r[4] & ((1 << (8 * r[3])) - 1)) for r in cap.body_reads[lo:hi]
                      if entry <= r[1] < end and r[1] not in excl[key]}
                if tr != mc.reads:
                    extra = sorted(mc.reads - tr)[:3]
                    miss = sorted(tr - mc.reads)[:3]
                    raise Mismatch(f"loads differ: model-only {[fmt(x) for x in extra]} "
                                   f"original-only {[fmt(x) for x in miss]}")
                stats[key + ".stores"] += len(tw)
                stats[key + ".loads"] += len(tr)
                stats[key + ".calls"] += mc.ci
                if retv and u(v0) != xregs["v0"]:
                    raise Mismatch(f"v0 model 0x{u(v0):08X} original 0x{xregs['v0']:08X}")
            except Mismatch as ex:
                fails.append((key, eseq, str(ex), "synthetic" if cap.name == "synth" else "natural"))
                if verbose and len([f for f in fails if f[0] == key]) <= 4:
                    print(f"    FAIL {cap.name} {key} @seq {eseq}: {ex}")
    return fails


def fmt(t):
    if t is None:
        return "-"
    return f"[0x{0x80000000 | t[0]:08X}]{t[1]}=0x{t[2]:X}"


def load_captures(verbose=False):
    bodies = {k: (v[0], v[1]) for k, v in FUNCS.items()}
    caps = []
    for name in CAPTURES:
        d = os.path.join(ROOT, CAPTURES[name][1])
        if not os.path.exists(os.path.join(d, "watch.csv")):
            print(f"  capture '{name}' missing ({d}) - run `roadq.py capture` first")
            continue
        caps.append(Capture(name, bodies, verbose))
    return caps


def static_checks(img: E.Image) -> list[str]:
    """A few load-bearing constants, read from the player's own EXE."""
    bad = []
    def want(addr, word, what):
        if img.word(addr) != word:
            bad.append(f"0x{addr:08X} {what}: 0x{img.word(addr):08X} != 0x{word:08X}")
    want(0x80036F18, 0x28420083, "slti v0,v0,131 - the settle threshold")
    want(0x80036CC0, 0x24437A30, "the forward neighbour function pointer")
    want(0x80036CC8, 0x24437FBC, "the backward neighbour function pointer")
    want(0x80036B80, 0x00031900, "sll v1,v1,4 - the 4096 -> 65536 promotion")
    want(0x80038900, 0x0003BB40, "sll s7,v1,13 - the racing-line byte to 16.16")
    want(0x800388F4, 0x83C30026, "lb v1,38(s8) - SLCT +0x26 on a forward walk")
    want(0x80038904, 0x83C20027, "lb v0,39(s8) - SLCT +0x27 on a backward walk")
    want(0x80039014, 0xAD28000C, "sw t0,12(t1) - the cursor restore")
    want(0x8003A38C, 0x28C20005, "slti v0,a2,5 - at most four arms per junction record")
    return bad


# ---------------------------------------------------------------------------
# Synthetic inputs: the original, executed by PyCpu, on states derived from the `quick` snapshot
# ---------------------------------------------------------------------------

SYNTH_SNAPSHOT = "work\\oracle\\state\\quick"
GRAPH = 0x8005B240              # -> the resident ROAD<n>.MAP image (MAP_ header + 8)


def synth_cases(n: int, seed: int, img: E.Image):
    """Yield (label, ram0, regs, entry) for n synthetic cases.  Every pointer is a live one out
    of the snapshot; only scalars and the per-bike junction memory are randomised."""
    import json
    import random
    rnd = random.Random(seed)
    base_ram = bytearray(open(os.path.join(ROOT, SYNTH_SNAPSHOT, "ram.bin"), "rb").read())
    cpu = json.load(open(os.path.join(ROOT, SYNTH_SNAPSHOT, "cpu.json")))
    gp = cpu["gpr"]["gp"]

    def rw(b, a):
        return int.from_bytes(b[a & 0x1FFFFF:(a & 0x1FFFFF) + 4], "little")

    def rh(b, a):
        return s16(int.from_bytes(b[a & 0x1FFFFF:(a & 0x1FFFFF) + 2], "little"))

    def ww(b, a, v):
        b[a & 0x1FFFFF:(a & 0x1FFFFF) + 4] = u(v).to_bytes(4, "little")

    pool0 = rw(base_ram, POOL0)
    nb = rw(base_ram, BIKE_COUNT)
    g = rw(base_ram, GRAPH)
    ipt, nipt = rw(base_ram, g + 0x30), rh(base_ram, g + 0x3C)
    pdt = rw(base_ram, g + 0x34)
    btt, nbtt = rw(base_ram, g + 0x1C), rh(base_ram, g + 0x28)
    resident = [rw(base_ram, btt + 32 * i + 12) for i in range(nbtt)
                if rw(base_ram, btt + 32 * i + 12)]
    bikes = [pool0 + 1096 * i for i in range(nb)
             if rw(base_ram, pool0 + 1096 * i + 0x148) and rw(base_ram, pool0 + 1096 * i + 0x154)]
    sp = 0x801FFC00
    for k in range(n):
        ram = bytearray(base_ram)
        e = rnd.choice(bikes)
        r = rnd.random()
        if r < 0.33:
            ww(ram, e + 948, 0)
        elif r < 0.66 and nipt > 0:
            ww(ram, e + 948, ipt + 12 * rnd.randrange(nipt))
            # a remembered index is always < n, and n < 3 (TurnsFrom's callers accept 0 < n < 3)
            ram[(e + 947) & 0x1FFFFF] = rnd.randrange(2)
        if rnd.random() < 0.5:
            ww(ram, e + 0x184, rw(ram, e + 0x184) ^ 0x80)
        if rnd.random() < 0.6 and resident:
            # re-seat the bike's cursor on a random resident object, piece, sub-object and slice
            obj = rnd.choice(resident)
            npc_ = rh(ram, obj + 0x12)
            pi = rnd.randrange(max(1, npc_))
            piece = rw(ram, obj + 0x2C) + 32 * pi
            subt = rw(ram, obj + 0x30) + 28 * rh(ram, piece + 20)
            cnt = rh(ram, subt + 10)
            slc = rw(ram, obj + 0x34) + 52 * (rh(ram, subt + 8) + rnd.randrange(max(1, cnt)))
            turn = node = 0
            if rh(ram, obj + 16) != 0 and s(rw(ram, obj + 12)) != 1:
                for q in range(nipt):
                    if rh(ram, ipt + 12 * q) == s(rw(ram, obj)):
                        node = ipt + 12 * q
                        turn = pdt + 12 * (rh(ram, node + 6) + rnd.randrange(max(1, rh(ram, node + 8))))
            for c_, v_ in enumerate((obj, piece, subt, slc)):
                ww(ram, e + 0x148 + 4 * c_, v_)
            ww(ram, e + 0x160, node if rnd.random() < 0.7 else 0)
            ww(ram, e + 0x164, turn if rnd.random() < 0.7 else 0)
        sl = rw(ram, e + 0x154)
        chord = s(rw(ram, sl + 32))
        regs = {"gp": gp, "sp": sp, "ra": PyCpu.STOP}
        if k % 10 == 9:
            # BOUNDARY families: inputs that sit exactly on a comparison the random ones never hit
            if k % 20 == 9:
                # 0x80036F18 `slti v0,v0,131`: a point whose along-distance from the slice two
                # ahead is exactly 131, so the settle test sees |131 - 0| and must NOT stop
                idx = rh(ram, sl)
                cnt = rh(ram, rw(ram, e + 0x150) + 10)
                if idx == 0 or idx + 3 > cnt - 1:
                    continue
                s2 = sl + 2 * SLICE
                pos = [s(rw(ram, s2 + 20 + 4 * c)) for c in range(3)]
                t2 = [rh(ram, s2 + 14 + 2 * c) for c in range(3)]
                found = None
                for o in range(64, 4096):
                    pt = [pos[c] + ((t2[c] * o) >> 12) for c in range(3)]
                    d_ = 0
                    for c in range(3):
                        d_ = u(d_ + mid(u(t2[c] << 4), u(pt[c] - pos[c])))
                    if d_ == 131:
                        found = pt
                        break
                if found is None:
                    continue
                for c in range(3):
                    ww(ram, e + 0xF4 + 4 * c, found[c])
                regs.update(a0=u(e + 0xAC), a1=u(e + 0x148), a2=u(e + 0xF4), a3=0)
                yield ("36b14", bytes(ram), regs, 0x80036B14)
            else:
                # 0x80038A44 `slt v0,s6,v0` with v0 = chord >> 1: a target exactly half-way
                ww(ram, sp + 16, e + 0x148)
                ww(ram, sp + 20, e + 0x370)
                ww(ram, sp + 24, u(sp + 0x100))
                regs.update(a0=u(e), a1=0, a2=u(chord >> 1), a3=rnd.choice([1, u(-1)]))
                yield ("386dc", bytes(ram), regs, 0x800386DC)
            continue
        if k % 2 == 0:
            # 0x80036B14(e + 0xAC, e + 0x148, e + 0xF4) with a point d units along the slice
            span = rnd.choice([40, 400, 2500])
            d = 0 if rnd.random() < 0.05 else rnd.randint(-span * 65536, span * 65536)
            lat = rnd.randint(-12 * 65536, 12 * 65536)
            pt = []
            for c in range(3):
                pos = s(rw(ram, sl + 20 + 4 * c))
                t2 = rh(ram, sl + 14 + 2 * c)
                t0 = rh(ram, sl + 2 + 2 * c)
                pt.append(pos + ((t2 * d) >> 12) + ((t0 * lat) >> 12))
            for c in range(3):
                ww(ram, e + 0xF4 + 4 * c, pt[c])
            regs.update(a0=u(e + 0xAC), a1=u(e + 0x148), a2=u(e + 0xF4), a3=0)
            yield ("36b14", bytes(ram), regs, 0x80036B14)
        else:
            # 0x800386DC(e, ahead, along, dir, &e[+0x148], &e[+0x370], out) as AiDrive calls it
            ahead = rnd.randint(0, rnd.choice([80, 600, 4000]) * 65536)
            along = rnd.randint(-chord // 3, chord + chord // 3) if chord > 0 else 0
            dr = rnd.choice([1, 1, -1, -1, 0])
            out = 0 if rnd.random() < 0.1 else u(sp + 0x100)
            ww(ram, sp + 16, e + 0x148)
            ww(ram, sp + 20, e + 0x370)
            ww(ram, sp + 24, out)
            regs.update(a0=u(e), a1=u(ahead), a2=u(along), a3=u(dr))
            yield ("386dc", bytes(ram), regs, 0x800386DC)


def run_synthetic(n, seed, img, excl, which, cov, stats, verbose):
    pset = probe_set(img)
    bodies = [(v[0], v[1]) for v in FUNCS.values()]
    fails = []
    rejected = Counter()
    for label, ram0, regs, entry in synth_cases(n, seed, img):
        cpu = PyCpu(bytearray(ram0), regs, pset, bodies)
        try:
            cpu.run(entry)
        except CpuStop as ex:
            rejected[str(ex).split(" 0x")[0]] += 1
            continue
        cap = SynthCapture("synth", ram0, cpu)
        fails += run_capture(cap, which, excl, verbose, cov, stats)
    return fails, rejected



def evaluate(t_caps, img, excl, which, args, verbose):
    """One full pass: natural captures and synthetic cases.  Returns (fails, stats, cov, per,
    rejected)."""
    cov = {"natural": defaultdict(Counter), "synthetic": defaultdict(Counter)}
    stats = Counter()
    fails = []
    per = defaultdict(Counter)
    for cap in t_caps:
        before = Counter(stats)
        fails += run_capture(cap, which, excl, verbose, cov["natural"], stats)
        for k in which:
            per[k][cap.name] = stats[k] - before[k]
    rejected = Counter()
    if args.synthetic:
        before = Counter(stats)
        f, rejected = run_synthetic(args.synthetic, args.seed, img, excl, which,
                                    cov["synthetic"], stats, verbose)
        fails += f
        for k in which:
            per[k]["synthetic"] = stats[k] - before[k]
    return fails, stats, cov, per, rejected


def data_checks() -> list[str]:
    """Facts about ROAD<n>.MAP that the transcription leans on, read from the player's disc."""
    import math
    bad = []
    for n in (1, 2):
        d = open(os.path.join(ROOT, "work", "disc_us", "DATA", f"ROAD{n}.MAP"), "rb").read()
        off, blocks = 0x5C, {}
        while off < len(d):
            size = struct.unpack_from("<I", d, off + 4)[0]
            blocks[d[off:off + 4]] = (off + 8, size - 8)
            off += size
        o, sz = blocks[b"BIT_"]
        if sz != 50 * 104:
            bad.append(f"ROAD{n}.MAP BIT_ is {sz} bytes, not 50 x 104")
        dirs, lens = Counter(), Counter()
        for i in range(sz // 104):
            rid, na = struct.unpack_from("<2h", d, o + 104 * i)
            if rid != i or not 0 < na < 5:
                bad.append(f"ROAD{n}.MAP BIT_ record {i}: id {rid}, arms {na}")
            for k in range(na):
                _, dr, _, vx, vy, vz = struct.unpack_from("<6h", d, o + 104 * i + 8 + 24 * k)
                dirs[dr] += 1
                ln = round(math.sqrt(vx * vx + vy * vy + vz * vz))
                lens["zero" if ln == 0 else ("unit" if abs(ln - 4096) <= 1 else "other")] += 1
        if set(dirs) != {1, -1}:
            bad.append(f"ROAD{n}.MAP arm +2 values {dict(dirs)} (expected only +1/-1)")
        if lens["other"]:
            bad.append(f"ROAD{n}.MAP arm vectors {dict(lens)}")
        o, sz = blocks[b"GPDT"]
        if {struct.unpack_from("<h", d, o + 12 * i + 8)[0] for i in range(sz // 12)} != {1}:
            bad.append(f"ROAD{n}.MAP GPDT +8 is not always 1")
        o, sz = blocks[b"PDT_"]
        if {struct.unpack_from("<h", d, o + 12 * i + 6)[0] for i in range(sz // 12)} != {1, -1}:
            bad.append(f"ROAD{n}.MAP PDT_ +6 is not always +1/-1")
        print(f"  ROAD{n}.MAP: BIT_ = {sz and 50} x 104 B, arm +2 {dict(dirs)}, arm vectors "
              f"{dict(lens)}, GPDT +8 always 1, PDT_ +6 always +-1")
    import chunk as C
    for n in (1, 2):
        objs = C.load_type3(os.path.join(ROOT, "work", "disc_us", "DATA", f"STREAM{n}.STR"))
        ok = nsub = 0
        kinds = Counter()
        rl = {0x26: [], 0x27: []}
        tagged = 0
        for oid, o in objs.items():
            kinds[(oid < 50, struct.unpack_from("<h", o.raw, 0x30)[0])] += 1
            # object +0x12 nGRPT, +0x14 {nSUBT, nSLCT}; +0x2C..+0x48 and +0x5C..+0x64 are zero
            # on disc (the loader turns them into block pointers)
            ng, nsu, nsl = struct.unpack_from("<3h", o.raw, 0x32)
            if (ng, nsu, nsl) != (len(C.recs(o.raw, o.bl, "GRPT")), len(o.subt), len(o.slices)):
                bad.append(f"STREAM{n} object {oid}: header counts {ng},{nsu},{nsl}")
            if any(struct.unpack_from("<I", o.raw, 0x20 + q)[0]
                   for q in list(range(0x2C, 0x4C, 4)) + [0x5C, 0x60, 0x64]):
                bad.append(f"STREAM{n} object {oid}: a pointer word is not zero on disc")
            pos = 0
            for sb in o.subt:
                first, cnt = sb[2] & 0xFFFF, sb[2] >> 16
                nsub += 1
                if first != pos:
                    bad.append(f"STREAM{n} object {oid}: SUBT does not tile SLCT")
                pos = first + cnt
                for q in range(cnt):
                    if o.slices[first + q].idx != q:
                        bad.append(f"STREAM{n} object {oid}: slice {first + q} index != {q}")
                    else:
                        ok += 1
            if pos != len(o.slices):
                bad.append(f"STREAM{n} object {oid}: SUBTs cover {pos} of {len(o.slices)} slices")
            so = o.bl["SLCT"][0] + 8
            for i in range(len(o.slices)):
                for f_ in rl:
                    rl[f_].append(struct.unpack_from("<b", o.raw, so + 52 * i + f_)[0])
                tagged += struct.unpack_from("<h", o.raw, so + 52 * i + 0x32)[0] != 0
        if set(kinds) != {(True, 1), (False, 0)}:
            bad.append(f"STREAM{n}: header +0x10 is not (id < 50 ? 1 : 0): {dict(kinds)}")
        m26 = sum(rl[0x26]) / len(rl[0x26])
        m27 = sum(rl[0x27]) / len(rl[0x27])
        print(f"  STREAM{n}.STR: {len(objs)} road objects, {nsub} SUBT tile SLCT exactly, slice "
              f"index == position in its SUBT for {ok} slices; header +0x10 == (id < 50), "
              f"+0x12/+0x14/+0x16 == GRPT/SUBT/SLCT counts, pointer words zero; "
              f"SLCT +0x26 in [{min(rl[0x26])},{max(rl[0x26])}] mean {m26 / 8:+.2f} units, "
              f"+0x27 in [{min(rl[0x27])},{max(rl[0x27])}] mean {m27 / 8:+.2f} units; "
              f"+0x32 != 0 on {tagged} slices")
    return bad


def cmd_verify(args) -> int:
    img = E.load_exe()
    if img.sha1 != SLUS_SHA1:
        print(f"FAIL: SLUS_010.53 sha1 {img.sha1} != {SLUS_SHA1}")
        return 1
    bad = static_checks(img)
    if not args.mutate:
        bad += data_checks()
    for b in bad:
        print("  static FAIL", b)
    # the code the snapshots execute is the player's EXE, byte for byte, over all six bodies
    lo, hi = min(v[0] for v in FUNCS.values()), max(v[1] for v in FUNCS.values())
    for name, (snap, _, _) in CAPTURES.items():
        ram = open(os.path.join(ROOT, snap, "ram.bin"), "rb").read()
        if ram[lo & 0x1FFFFF:hi & 0x1FFFFF] != img.data[img.off(lo):img.off(hi)]:
            bad.append(f"{name}: resident code 0x{lo:08X}..0x{hi:08X} differs from SLUS_010.53")
            print("  static FAIL", bad[-1])
    which = [args.only] if args.only else list(FUNCS)
    excl = {k: save_pcs(img, FUNCS[k][0], FUNCS[k][1]) for k in FUNCS}
    t_caps = load_captures()
    if not t_caps:
        return 1
    if args.mutate:
        caught = total = 0
        for name, why in MUTANTS.items():
            key = name.split(".")[0]
            if args.only and key != args.only:
                continue
            MUT.clear()
            MUT.add(name)
            fails, stats, _, per, _ = evaluate(t_caps, img, excl, [key], args, False)
            nat = sum(1 for f in fails if f[3] == "natural")
            syn = len(fails) - nat
            ok = len(fails) > 0
            caught += ok
            total += 1
            print(f"  mutant {name:28s} {'CAUGHT' if ok else 'MISSED'}  natural {nat:4d}/"
                  f"{stats[key] - per[key]['synthetic']:4d}  synthetic {syn:4d}/"
                  f"{per[key]['synthetic']:4d}   ({why})")
        MUT.clear()
        print(f"roadq --mutate: {caught} of {total} mutants caught"
              f"{'' if caught == total else '  <-- FAIL'}")
        return 0 if caught == total and not bad else 1
    fails, stats, cov, per, rejected = evaluate(t_caps, img, excl, which, args, args.verbose)
    print()
    rn = rb = 0
    for cap in t_caps:
        n_, b_, msgs = replay_natural(cap)
        rn += n_
        rb += b_
        for x in msgs:
            print("   ", x)
    print(f"  PyCpu vs oracle: {rn} natural top-level invocations re-executed, {rb} with a "
          f"different store stream")
    if args.synthetic:
        print(f"  synthetic: {args.synthetic} cases, seed {args.seed}, rejected by PyCpu: "
              f"{sum(rejected.values())} {dict(rejected) if rejected else ''}")
    for k in which:
        nf = sum(1 for f in fails if f[0] == k)
        src = ", ".join(f"{c} {per[k][c]}" for c in per[k])
        print(f"  {k}  0x{FUNCS[k][0]:08X}  invocations {stats[k]:5d} ({src})  "
              f"mismatches {nf}  {'PASS' if nf == 0 and stats[k] else 'FAIL'}")
        print(f"         compared: {stats[k + '.stores']} stores, {stats[k + '.loads']} distinct "
              f"loads, {stats[k + '.calls']} calls")
        if args.coverage or args.verbose:
            labs = sorted(set(cov["natural"][k]) | set(cov["synthetic"][k]))
            print(f"         {'natural':>8s} {'synth':>7s}  branch")
            for lab in labs:
                print(f"         {cov['natural'][k][lab]:8d} {cov['synthetic'][k][lab]:7d}  {lab}")
    total = sum(stats[k] for k in which)
    print(f"roadq: {len(which)} functions, {total} invocations, {len(fails)} mismatches, "
          f"{rb} PyCpu/oracle differences, {len(bad)} static failures")
    return 0 if not fails and not bad and not rb and all(stats[k] for k in which) else 1


def cmd_capture(_args) -> int:
    print("The probe list is: the entry and the `jr ra` of each of the six functions, the")
    print("return address of every jal/jalr inside them, and the entry of every callee.")
    print("Run from the project root; each run writes watch.csv, calls.bin and probes.csv.")
    print()
    img = E.load_exe()
    parg = " ".join(f"--probe 0x{a:08X}" for a in sorted(probe_set(img)))
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
    v.add_argument("--only", choices=list(FUNCS))
    v.add_argument("--verbose", action="store_true")
    v.add_argument("--coverage", action="store_true")
    v.add_argument("--synthetic", type=int, default=1000, help="synthetic cases (0 = none)")
    v.add_argument("--seed", type=int, default=0x5EED)
    sub.add_parser("capture")
    a = ap.parse_args()
    return {"verify": cmd_verify, "capture": cmd_capture}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
