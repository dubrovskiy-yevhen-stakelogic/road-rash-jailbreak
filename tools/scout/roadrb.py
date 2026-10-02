"""Scout probe for THE FUNCTIONS UNDER THE ROAD RE-BIND SLUS 0x800374D4 (Road Rash: Jailbreak,
USA, SLUS_01053).

Sibling probes: roadrt.py (the 24 functions of the runtime road layer) and roadq.py (the road
query), whose machinery this probe IMPORTS and does not copy:
PyCpu, the replay machine, the capture reader and the model harness.

What it proves.  Fourteen resident functions are transcribed below as executable Python models, one
statement per original load/store/call, in the original's order:

    0x8003BFE8 RoadShortcut        0x8003E754 RoadClassCore       0x8003EB58 NodeWedge
    0x8003ED14 CoreCrossSection    0x8003E45C CoreEdgeClass       0x8003E61C Lerp16
    0x8003BD2C TurnByRoads         0x8003F204 RoadsideRunNode     0x8003B024 RouteBindFirst
    0x8003B1C4 RouteBindStep       0x8003A5F4 RoadEndNode         0x8003F4D8 RouteRecordOfRoad
    0x8003F580 RouteHasExit        0x8003F3B4 RouteRecordByKey

Each model is run against EVERY invocation of the original inside captured traces of the oracle
interpreter (`rrverify trace`, whole-RAM watch, call log, pc probes on the resident closure of the
re-bind), exactly as roadrt.py does it: callees are supplied by the trace, and the body's ordered
stores (minus the prologue's callee-saved spills), its set of loads, its calls with their argument
registers, and v0 must all be identical.

The natural captures (quick, rr-race) never put an entity on a junction core, so this probe adds
TELEPORT captures: copies of the `quick` snapshot in which ONLY DATA was changed - one bike's position
and road cursor moved onto junction 7's core or onto one of its stubs - after which the unmodified
game runs its own frames in the oracle interpreter.  Invocations there are the original's, executed
by the reference interpreter, on a state the game did not reach by itself.  PyCpu (roadrt.py's R3000
executor) additionally runs the original bytes on synthetic inputs.

Reads ONLY work\\disc_us, the snapshots under work\\oracle\\state and its own output under
work\\roadrb (all gitignored).  `capture` writes work\\roadrb\\state\\* (patched snapshot copies) and
work\\roadrb\\cap_* (traces).  No game bytes are copied anywhere else.

Usage (from the project root):

    python tools\\scout\\roadrb.py capture            # build the teleport states, run all traces
    python tools\\scout\\roadrb.py capture --print    # only print the rrverify commands
    python tools\\scout\\roadrb.py verify             # the check (last line: roadrb: ...)
    python tools\\scout\\roadrb.py verify --coverage
    python tools\\scout\\roadrb.py verify --synthetic 3000 --seed 7
    python tools\\scout\\roadrb.py verify --mutate    # the negative control: every mutant must FAIL
    python tools\\scout\\roadrb.py consumers          # who reads the wrong-way bit, +0x44 bit 0, +0x24
"""

from __future__ import annotations

import argparse
import bisect
import json
import os
import random
import shutil
import struct
import subprocess
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import exe as E  # noqa: E402  (read-only use)
import roadq as Q  # noqa: E402  (PyCpu arithmetic helpers)
import roadrt as RT  # noqa: E402  (the capture reader, the replay machine, PyCpu with scratchpad)

u, s, s16, mid, lohi = Q.u, Q.s, Q.s16, Q.mid, Q.lohi
absv, slt, sltu, sra = RT.absv, RT.slt, RT.sltu, RT.sra
Mismatch, CpuStop = Q.Mismatch, Q.CpuStop

ROOT = E.ROOT
SLUS_SHA1 = RT.SLUS_SHA1
ROUTE = RT.ROUTE            # 0x800D6170: +0x12 s16 record count (-1 none), +0x18 finish, +0x24 records
GAME_STATE = RT.GAME_STATE
GRAPH = RT.GRAPH            # 0x8005B240 -> the resident ROAD<n>.MAP
BIKE_COUNT = 0x8005B1F8
POOL1 = 0x8005B3A4
WORK = os.path.join("work", "roadrb")

# key: (entry, frame size, returns a value)
FUNCS = {
    "bfe8": (0x8003BFE8, 144, False), "e754": (0x8003E754, 176, True),
    "eb58": (0x8003EB58, 80, True), "ed14": (0x8003ED14, 88, True),
    "e45c": (0x8003E45C, 48, True), "e61c": (0x8003E61C, 8, True),
    "bd2c": (0x8003BD2C, 0, True), "f204": (0x8003F204, 56, False),
    "b024": (0x8003B024, 32, False), "b1c4": (0x8003B1C4, 48, False),
    "a5f4": (0x8003A5F4, 48, True), "f4d8": (0x8003F4D8, 48, True),
    "f580": (0x8003F580, 0, True), "f3b4": (0x8003F3B4, 0, True),
}
NAMES = {
    "bfe8": "RoadShortcut", "e754": "RoadClassCore", "eb58": "NodeWedge", "ed14": "CoreCrossSection",
    "e45c": "CoreEdgeClass", "e61c": "Lerp16", "bd2c": "TurnByRoads", "f204": "RoadsideRunNode",
    "b024": "RouteBindFirst", "b1c4": "RouteBindStep", "a5f4": "RoadEndNode",
    "f4d8": "RouteRecordOfRoad", "f580": "RouteHasExit", "f3b4": "RouteRecordByKey",
}

# Callees these models call that roadrt's table does not list (name, argument registers read).
RT.CALLEES.update({
    0x8003A5F4: ("RoadEndNode", 3), 0x800245DC: ("GraphRoad", 1), 0x800245F4: ("GraphNode", 1),
    0x8003F3B4: ("RouteRecordByKey", 1), 0x8003F4D8: ("RouteRecordOfRoad", 1),
    0x8003F580: ("RouteHasExit", 2), 0x8001FCB0: ("ApproxLen3", 3), 0x80010028: ("FixDiv", 2),
    0x8003BD2C: ("TurnByRoads", 4), 0x8003ED14: ("CoreCrossSection", 4),
    0x8003E45C: ("CoreEdgeClass", 4), 0x8003E61C: ("Lerp16", 3),
})

MUTANTS = {
    "a5f4.near_far": "RoadEndNode mode 0 takes the FAR end of the road",
    "a5f4.degree": "RoadEndNode accepts a graph node of degree 2 as a junction",
    "a5f4.len_shift": "the graph road length is (len >> 5) << 16, not (len >> 6) << 16",
    "b024.start_rec": "RouteBindFirst binds players to record count-1, not to the start record at count",
    "b1c4.no_clear": "RouteBindStep does not clear the slot bit when the node is not a successor",
    "b1c4.ai_follow": "AI bikes off their record take the players' 50-unit successor path",
    "f3b4.le": "RouteRecordByKey scans count+1 records",
    "f4d8.from1": "RouteRecordOfRoad starts at record 1",
    "f580.le": "RouteHasExit scans count+1 exits",
    "bd2c.negate": "TurnByRoads negates the GPDT direction for a PDT_ dir <= 0",
    "e61c.swap": "Lerp16 weights a by t and b by 1-t",
    "e45c.no_clamp": "CoreEdgeClass keeps a lateral whose sign disagrees with the turn direction",
    "e45c.le": "CoreEdgeClass tests |lat| <= edge instead of <",
    "ed14.side_b": "CoreCrossSection picks B's half with A's convention",
    "eb58.no_wrap": "NodeWedge's second arm of the last wedge is arm n, not arm 0",
    "eb58.turn": "NodeWedge turns the arm vector the other way, (v2, v1, -v0)",
    "e754.no_clamp": "RoadClassCore does not clamp the along distance to [0, len]",
    "e754.rev_dirs": "RoadClassCore keeps (A.dir, B.dir) when the record's roads are in reverse order",
    "f204.cap4": "RoadsideRunNode caps the record count at 4, not 3",
    "f204.byte": "RoadsideRunNode copies record byte +3 into entry byte +2, not byte +0",
    "bfe8.last_slice": "RoadShortcut enters an arm with dir > 0 at its LAST slice",
    "bfe8.no_own": "RoadShortcut does not skip the arm of the entity's own road",
}
MUT: set[str] = set()


# ---------------------------------------------------------------------------
# The models.  Each takes the machine `m` (roadrt.Machine) and returns v0 (or None).
# `fr` is the function's own frame (entry sp - frame size); stack arguments are at entry sp + 16.
# ---------------------------------------------------------------------------

def m_a5f4(m):
    """0x8003A5F4 RoadEndNode(pos = e+0x168, &node, mode) -> distance to the chosen end (16.16)."""
    R = m.R
    s2, s5, s4 = R["a0"], R["a1"], R["a2"]
    s1 = 0
    a0 = m.lw(s2)
    s0 = u(-1)
    if (a0 >> 16) == 1:
        m.c("on a junction core: the node itself, distance 0")
        s0 = a0 & 0xFFFF
    else:
        v1 = m.call(0x800245DC, a0 & 0xFFFF)
        if v1 == 0:
            m.c("no graph road")
            m.sw(s5, s0)
            return s1
        v0 = m.lw(v1 + 4)
        s0 = m.lw(v1 + 12)                       # the road's end node
        a0 = m.lw(s2 + 8)                        # the along distance
        sh = 5 if "a5f4.len_shift" in MUT else 6
        v0 = u((v0 >> sh) << 16)                 # srl 6, sll 16: the graph length in 16.16
        s1 = u(v0 - a0)
        if s4 == 0:
            near_start = slt(a0, s1) == 1
            if "a5f4.near_far" in MUT:
                near_start = not near_start
            start = near_start
            if not start:
                d = m.lw(s2 + 4)
                start = s(d) < 0 and s4 == 1     # never true for mode 0
        else:
            d = m.lw(s2 + 4)
            start = s(d) < 0 and s4 == 1
        if start:
            m.c("the start node")
            s1 = m.lw(s2 + 8)
            s0 = m.lw(v1 + 8)
        else:
            m.c("the end node")
    if s0 == u(-1):
        m.sw(s5, s0)
        return s1
    v1 = m.call(0x800245F4, s0)
    if v1 == 0:
        m.sw(s5, s0)
        return s1
    v0 = m.lw(v1 + 4)
    lim = 2 if "a5f4.degree" in MUT else 3
    if v0 < lim:
        m.c("degree < 3: no junction")
        s0 = u(-1)
    else:
        m.c("a junction")
        s0 = m.lw(v1)
    m.sw(s5, s0)
    return s1


def m_f3b4(m):
    """0x8003F3B4 RouteRecordByKey(key): the first of the `count` records whose +0 is key."""
    key = m.R["a0"]
    v1 = m.lh(ROUTE + 18)
    if v1 <= 0:
        m.c("no records")
        return 0
    a3 = v1 + (1 if "f3b4.le" in MUT else 0)
    p = m.lw(ROUTE + 36)
    a1 = 0
    while True:
        v0 = m.lw(p)
        a1 += 1
        if v0 == key:
            m.c("found")
            return p
        if not a1 < a3:
            m.c("not found")
            return 0
        p = u(p + 120)


def m_f4d8(m):
    """0x8003F4D8 RouteRecordOfRoad(road): the first record with a leg on `road` (RouteFindLeg)."""
    s5 = m.R["a0"]
    s2 = 1 if "f4d8.from1" in MUT else 0
    v0 = m.lh(ROUTE + 18)
    if v0 <= 0:
        return 0
    s1 = 120 * s2
    while True:
        s0 = u(m.lw(ROUTE + 36) + s1)
        v0 = m.call(0x8003B4B0, s0, s5)
        s2 += 1
        if v0:
            m.c("found")
            return s0
        if not s2 < m.lh(ROUTE + 18):
            m.c("not found")
            return 0
        s1 += 120


def m_f580(m):
    """0x8003F580 RouteHasExit(rec, road): 1 when road is one of rec's +0x54 exit roads."""
    a0, a1 = m.R["a0"], m.R["a1"]
    if a0 == 0:
        return 0
    if s(a1) < 0:
        m.c("negative road")
        return 0
    a2 = m.lw(a0 + 16)
    if "f580.le" in MUT:
        a2 = u(a2 + 1)
    if not slt(0, a2):
        return 0
    v1 = 0
    while True:
        v0 = m.lw(a0 + 84)
        v1 += 1
        if v0 == a1:
            m.c("an exit")
            return 1
        if not slt(v1, a2):
            m.c("not an exit")
            return 0
        a0 = u(a0 + 4)


def _slot_bit(m, rec, h, setbit):
    v = m.lhu(rec + 118)
    b = 1 << (h & 31)
    m.sh(rec + 118, (v | b) if setbit else (v & u(~b)))


def m_b024(m):
    """0x8003B024 RouteBindFirst(p = e+0xAC, e, pool, x)."""
    R = m.R
    s1, s2, a2, s0 = R["a0"], R["a1"], R["a2"], R["a3"]
    v1 = 0
    if s1 == 0:
        return
    if s2 != 0:
        gs = m.lw(GAME_STATE)
        h = m.lhu(s2 + 172)
        n = m.lw(gs + 48)
        police = False
        if not sltu(h, n):
            police = (m.lbu(m.lw(s2 + 1084) + 1) & 0xF) == 2
        if not police:
            m.c("pool 0 player or not police: the start record")
            cnt = m.lh(ROUTE + 18)
            base = m.lw(ROUTE + 36)
            if "b024.start_rec" in MUT:
                cnt -= 1
            m.sw(s1 + 256, u(base + 120 * cnt))
        elif s0:
            m.c("police: the other entity's record")
            m.sw(s1 + 256, m.lw(s0 + 428))
        else:
            v0 = m.call(0x8003F4D8, m.lhu(s1 + 188))
            if v0:
                m.c("police: the record of its road")
                m.sw(s1 + 256, v0)
            else:
                m.c("police: no record, bit set on the stale one")
        a1 = m.lw(s2 + 428)
        a0 = m.lhu(s2 + 172)
        _slot_bit(m, a1, a0, True)
        return
    if a2 == 1:
        idx = m.lhu(s1) & 0x1F
        r = u(m.lw(POOL1) + 628 * idx)
        if r:
            m.c("pool 1: its bike's record")
            m.sw(s1 + 256, m.lw(m.lw(r + 596) + 428))
        else:
            m.sw(s1 + 256, 0)
        return
    if s0:
        v1 = m.call(0x8003B4B0, m.lw(s0 + 428), m.lhu(s1 + 188))
    if v1 == 0:
        v0 = m.call(0x8003F4D8, m.lhu(s1 + 188))
        if v0:
            m.c("other pool: the record of its road")
            m.sw(s1 + 256, v0)
            return
        if s0 == 0:
            m.c("other pool: nothing")
            return
    m.c("other pool: x's record")
    m.sw(s1 + 256, m.lw(s0 + 428))


def m_b1c4(m):
    """0x8003B1C4 RouteBindStep(p = e+0xAC, pool, e, x) - x (a3) is never read."""
    R = m.R
    s2, s0, s1 = R["a0"], R["a1"], R["a2"]
    fr = u(R["sp"] - 48)
    a1 = m.lw(s2 + 188)
    if (a1 >> 16) == 1:
        if s0 == 1 or (s0 == 0 and s1 != 0):
            S = m.lw(s2 + 256)
            if S == 0:
                return
            if s(m.lw(S + 16)) <= 0:
                m.c("core: record without exits")
                return
            key = a1 & 0xFFFF
            if m.lw(S) == key:
                if s1 == 0:
                    return
                m.c("core: the record's own node, set the bit")
                _slot_bit(m, S, m.lhu(s1 + 172), True)
                return
            v1 = 0
            while True:
                k = m.lw(S + 100 + 4 * v1)
                if k != u(-1) and k == key:
                    N = m.call(0x8003F3B4, k)
                    if N:
                        m.sw(s2 + 256, N)
                    if s1 == 0:
                        return
                    if N:
                        m.c("core: a successor's node, rebind and set the bit")
                        _slot_bit(m, m.lw(s2 + 256), m.lhu(s1 + 172), True)
                    else:
                        h = m.lhu(s1 + 172)
                        _slot_bit(m, m.lw(s2 + 256), h, False)
                    return
                v0 = m.lw(S + 16)
                v1 += 1
                if not slt(v1, v0):
                    break
            if s1 == 0:
                return
            if "b1c4.no_clear" in MUT:
                return
            m.c("core: not a successor, clear the bit")
            h = m.lhu(s1 + 172)
            _slot_bit(m, m.lw(s2 + 256), h, False)
            return
        m.c("core, other pools: the record of the node")
        N = m.call(0x8003F3B4, m.lhu(s2 + 188))
        if N:
            m.sw(s2 + 256, N)
        return
    v0 = m.call(0x8003B4B0, m.lw(s2 + 256), a1 & 0xFFFF)
    if v0:
        m.c("open road: still on a leg")
        return
    follow = False
    if s0 < 2:
        if s1 == 0 or s0 != 0:
            follow = True
        elif (m.lbu(m.lw(s1 + 1084) + 1) & 0xF) == 2:
            follow = True
        else:
            gs = m.lw(GAME_STATE)
            h = m.lhu(s1 + 172)
            follow = sltu(h, m.lw(gs + 48)) == 1
            if "b1c4.ai_follow" in MUT:
                follow = True
    if not follow:
        m.c("open road, AI or other pool: the record of the road")
        N = m.call(0x8003F4D8, m.lhu(s2 + 188))
        if N:
            m.sw(s2 + 256, N)
        return
    S = m.lw(s2 + 256)
    if S == 0 or s1 == 0:
        return
    if s(m.lw(S + 16)) <= 0:
        return
    d = m.call(0x8003A5F4, u(s1 + 360), u(fr + 16), 0)
    if slt(0x320000, d):
        m.c("open road: more than 50 units from the end node")
        return
    if s(m.lw(S + 16)) <= 0:
        return
    v1 = 0
    while True:
        k = m.lw(S + 100 + 4 * v1)
        if k != u(-1) and k == m.lw(fr + 16):
            N = m.call(0x8003F3B4, k)
            if N == 0:
                return
            if m.call(0x8003F580, N, m.lhu(s1 + 360)):
                m.c("open road: advance to the successor")
                m.sw(s2 + 256, N)
            else:
                m.c("open road: road not an exit of the successor")
            return
        v0 = m.lw(S + 16)
        v1 += 1
        if not slt(v1, v0):
            m.c("open road: end node not a successor")
            return


def m_bd2c(m):
    """0x8003BD2C TurnByRoads(cursor, roadFrom, roadTo, &dir, &turn /* sp+16 */) -> sub-object."""
    R = m.R
    t2, a1, a2, a3 = R["a0"], R["a1"], R["a2"], R["a3"]
    t4 = m.lw(u(R["sp"] + 16))
    if t2 == 0:
        return 0
    j = m.lw(t2 + 24)
    if j == 0:
        m.c("no junction in the cursor")
        return 0
    if not slt(0, m.lh(j + 8)):
        return 0
    a0 = 0
    while True:
        v0 = m.lh(j + 6)
        G = m.lw(GRAPH)
        t0 = u(m.lw(G + 52) + 12 * (v0 + a0))
        if u(m.lh(t0 + 8)) == a1 and u(m.lh(t0 + 10)) == a2:
            m.c("turn found")
            v1 = m.lh(t0 + 2)
            g = u(m.lw(G + 56) + 12 * v1)
            pc = m.lw(t2 + 4)
            off = m.lh(g + 4)
            first = m.lh(pc + 20)
            obj = m.lw(t2)
            d = m.lh(g + 8)
            if "bd2c.negate" in MUT and m.lh(t0 + 6) <= 0:
                d = -d
            subt = m.lw(obj + 48)
            m.sw(a3, u(d))
            m.sw(t4, t0)
            return u(subt + 28 * (first + off))
        j = m.lw(t2 + 24)
        v0 = m.lh(j + 8)
        a0 += 1
        if not slt(a0, v0):
            m.c("no turn for the pair")
            return 0


def m_e61c(m):
    """0x8003E61C Lerp16(t, a, b) = mid(0x10000 - t, a) + mid(t, b); spills the first product."""
    R = m.R
    a0, a1, a2 = R["a0"], R["a1"], R["a2"]
    if "e61c.swap" in MUT:
        a1, a2 = a2, a1
    fr = u(R["sp"] - 8)
    v0 = u(0x10000 - a0)
    lo, hi = lohi(v0, a1)
    m.sw(fr, lo)
    m.sw(fr + 4, hi)
    return u(mid(v0, a1) + mid(a0, a2))


def m_e45c(m):
    """0x8003E45C CoreEdgeClass(gA, gB, t, lat, sideA, sideB, dir, out) -> 1 = off the road."""
    R = m.R
    sp = R["sp"]
    a0, a1, s6, s1 = R["a0"], R["a1"], R["a2"], R["a3"]
    v0 = m.lw(sp + 24)
    a2 = m.lw(sp + 16)
    a3 = m.lw(sp + 20)
    s4 = m.lw(sp + 28)
    s5 = 0
    if s(u(s1 ^ v0)) < 0 and "e45c.no_clamp" not in MUT:
        m.c("lateral against the turn direction: 0")
        s1 = 0
    s1 = absv(s1)
    s3 = u(a0 + (136 if a2 == 2 else 8))
    s0 = u(a1 + (136 if a3 == 2 else 8))
    outer = m.call(0x8003E61C, s6, u(absv(m.lw(s3 + 8))), u(absv(m.lw(s0 + 8))))
    s2 = outer
    ia = u(absv(m.lw(s3 + 8)))
    ib = u(absv(m.lw(s0 + 8)))
    ha = m.lh(s3 + 2)
    inner = outer
    lerp = True
    if ha > 0:
        ia = u(absv(m.lw(s3 + 24)))
        hb = m.lh(s0 + 2)
    else:
        hb = m.lh(s0 + 2)
        if hb <= 0:
            lerp = False
        elif ha != 0:
            ia = u(absv(m.lw(s3 + 24)))
            hb = m.lh(s0 + 2)
    if lerp:
        if hb != 0:
            ib = u(absv(m.lw(s0 + 24)))
        inner = m.call(0x8003E61C, s6, ia, ib)
    lt = (lambda x, y: s1 <= absv(y)) if "e45c.le" in MUT else (lambda x, y: s1 < absv(y))
    if lt(s1, inner):
        m.c("class 4: inside the inner edge")
        m.sh(s4 + 2, 1)
        m.sw(s4 + 4, 0)
        m.sh(s4, 4)
    elif lt(s1, s2):
        m.c("class 1: between the edges")
        m.sw(s4 + 4, inner)
        v1 = m.lhu(s3 + 18)
        m.sh(s4, 1)
        m.sh(s4 + 2, v1)
    else:
        m.c("class 0: off the road")
        s5 |= 1
        m.sw(s4 + 4, s2)
        m.sh(s4 + 2, 0)
        m.sh(s4, 0)
    return s5


def m_ed14(m):
    """0x8003ED14 CoreCrossSection(gA, xsih, dirA, dirB, gB, lat, t, out = e+0x174, dir) -> 0."""
    R = m.R
    sp = R["sp"]
    fr = u(sp - 88)
    s4, s7, a2, a3 = R["a0"], R["a1"], R["a2"], R["a3"]
    s5 = m.lw(sp + 16)
    s6 = m.lw(sp + 24)
    s1 = m.lw(sp + 28)
    if s(a2) > 0:
        s0, s3 = u(s4 + 8), 1
    else:
        s0, s3 = u(s4 + 136), 2
    b_pos = s(a3) > 0
    if "ed14.side_b" in MUT:
        b_pos = not b_pos
    if b_pos:
        v1, s2 = u(s5 + 136), 2
    else:
        v1, s2 = u(s5 + 8), 1
    outer = m.call(0x8003E61C, s6, u(absv(m.lw(s0 + 8))), u(absv(m.lw(v1 + 8))))
    m.sw(fr + 32, outer)
    h4 = m.lhu(s0 + 4)
    lat = m.lw(sp + 20)
    m.sh(fr + 40, h4)
    d = m.lw(sp + 32)
    m.sw(fr + 16, s3)
    m.sw(fr + 20, s2)
    m.sw(fr + 24, d)
    m.sw(fr + 28, u(s1 + 20))
    r = m.call(0x8003E45C, s4, s5, s6, lat)
    w = m.call(0x8003E61C, s6, m.lw(s4 + 4), m.lw(s5 + 4))
    m.sw(s1 + 28, 0)
    m.sh(s1 + 36, 0)
    m.sw(s1 + 40, m.lw(fr + 32))
    a1 = m.lhu(fr + 40)
    v1 = m.lw(s1 + 16)
    m.sw(s1 + 52, w)
    m.sw(s1, s4)
    m.sw(s1 + 4, s7)
    m.sw(s1 + 8, s5)
    m.sw(s1 + 12, s7)
    m.sw(s1 + 16, (v1 & u(-16)) | r)
    m.sh(s1 + 48, a1)
    return 0


def m_eb58(m):
    """0x8003EB58 NodeWedge(P, node, &armA, &armB, hint /* sp+16 */) -> wedge index or -1."""
    R = m.R
    sp = R["sp"]
    fr = u(sp - 80)
    s6, s5, s7, s8 = R["a0"], R["a1"], R["a2"], R["a3"]
    hint = s(m.lw(sp + 16))
    s4, s3, s0 = -1, 0, 0
    done = False
    if hint >= 0:
        n = m.lh(s5 + 2)
        if hint < n:
            m.c("the hint")
            s4 = hint
            s0 = s4
            if s4 < n - 1 or "eb58.no_wrap" in MUT:
                s3 = s4 + 1
            done = True
    if not done:
        if m.lh(s5 + 2) <= 0:
            m.c("no arms")
        else:
            i = 0
            while True:
                arm = u(s5 + 8 + 24 * i)
                i += 1
                if "eb58.turn" in MUT:
                    x = m.lhu(arm + 10)
                    m.sh(fr + 32, x)
                    m.sh(fr + 34, m.lhu(arm + 8))
                    m.sh(fr + 36, u(-m.lhu(arm + 6)))
                else:
                    x = m.lhu(arm + 10)
                    m.sh(fr + 32, u(-x))
                    m.sh(fr + 34, m.lhu(arm + 8))
                    m.sh(fr + 36, m.lhu(arm + 6))
                d = m.call(0x800B6AAC, s6, u(fr + 32), u(arm + 12))
                m.sw(fr + 16 + 4 * (i - 1), d)
                if not i < m.lh(s5 + 2):
                    break
            n = m.lh(s5 + 2)
            if n > 0:
                s0 = 0
                while True:
                    s3 = s0 + 1 if (s0 < n - 1 or "eb58.no_wrap" in MUT) else 0
                    d = m.lw(fr + 16 + 4 * s0)
                    s4 += 1
                    if not s(d) > 0 and s(m.lw(fr + 16 + 4 * s3)) > 0:
                        m.c("the wedge between arm i and arm i+1")
                        break
                    s0 += 1
                    if not s0 < n:
                        m.c("no wedge: arm n and arm 0 are returned")
                        break
            else:
                s0 = 0
    if s4 < 0:
        return u(s4)
    if s7:
        m.sw(s7, u(s5 + 8 + 24 * s0))
    if s8:
        m.sw(s8, u(s5 + 8 + 24 * s3))
    return u(s4)


def m_f204(m):
    """0x8003F204 RoadsideRunNode(e, zone): the node's one roadside-zone entry at e+0x1F4."""
    R = m.R
    sp = R["sp"]
    fr = u(sp - 56)
    s0, s4 = R["a0"], R["a1"]
    s2 = u(-1)
    if s0 == 0:
        return
    s1 = u(s0 + 492)
    node = m.call(0x80039AFC, m.lw(m.lw(s0 + 328)))
    if node:
        m.sw(fr + 16, s4)
        s2 = m.call(0x8003EB58, u(s0 + 184), node, u(fr + 24), u(fr + 28))
    if s(s2) < 0:
        m.c("no wedge: nothing written")
        return
    obj = m.lw(s0 + 328)
    v1 = m.lh(obj + 78)
    cap = 4 if "f204.cap4" in MUT else 3
    t2 = v1 if v1 < cap + 1 else cap             # slti v0,v1,4: at most 3 records
    a0 = m.lw(obj + 92)
    found = 0
    if a0 and t2 > 0:
        t3 = m.lw(fr + 24)
        t1 = m.lw(fr + 28)
        for t0 in range(t2):
            r = u(a0 + 20 * t0)
            if m.lbu(r + 2) != 1:
                continue
            a3 = m.lbu(r + 8)
            if a3 == 255:
                if s2 == m.lbu(r + 3):
                    m.c("record for the wedge index")
                    found = r
                    break
                continue
            a2 = m.lh(t3 + 4)
            if a3 == u(a2):
                if m.lbu(r + 9) == u(m.lh(t1 + 4)):
                    m.c("record for (armA, armB)")
                    found = r
                    break
            if m.lbu(r + 9) != u(a2):
                continue
            if a3 != u(m.lh(t1 + 4)):
                continue
            m.c("record for (armB, armA)")
            found = r
            break
    if not found:
        m.c("no record: +0x1F0 = 0")
        m.sw(s1 + 4, 0)
        return
    m.sw(s1 + 4, u(s1 + 8))
    m.sb(s1 + 10, m.lbu(found + (3 if "f204.byte" in MUT else 0)))
    m.sh(s1 + 8, m.lhu(found + 4))
    m.sb(s1 + 11, m.lbu(found + 6))


def m_e754(m):
    """0x8003E754 RoadClassCore(e, node, out = e+0x174, cursorOut, zone /* sp+16 */)."""
    R = m.R
    sp = R["sp"]
    fr = u(sp - 176)
    s3, t1, a2, s5 = R["a0"], R["a1"], R["a2"], R["a3"]
    s7 = s6 = s8 = s4 = 0
    m.sw(sp + 4, t1)                              # a1 to its home slot 180(sp)
    m.sw(sp + 8, a2)                              # a2 to 184(sp)
    for o in (120, 124, 128, 132):
        m.sw(fr + o, 0)
    m.sw(fr + 104, 1)
    s2 = m.lw(s3 + 328)
    if t1 != 0 and m.lw(s2 + 64) != 0:
        s1 = u(s3 + 184)
        m.sw(fr + 16, m.lw(sp + 16))
        w = m.call(0x8003EB58, s1, t1, u(fr + 96), u(fr + 100))
        if s(w) >= 0:
            A = m.lw(fr + 96)
            B = m.lw(fr + 100)
            ra, rb = m.lh(A + 4), m.lh(B + 4)
            m.sw(fr + 16, u(fr + 108))
            s7 = m.call(0x8003BD2C, u(s3 + 328), u(ra), u(rb), u(fr + 104))
            if s7 == 0:
                m.c("no turn for the wedge's roads: reads through a null sub-object")
            s0 = m.lw(s7 + 12)                    # the turn's length, 16.16 - no null check
            m.call(0x8001E0B4, u(fr + 40), u(s3 + 328), 32)
            m.sw(fr + 48, s7)
            v1 = m.lh(s7 + 8)
            m.sw(fr + 52, u(m.lw(s2 + 52) + 52 * v1))
            s6 = m.call(0x80036B14, u(s3 + 172), u(fr + 40), s1)
            m.call(0x80036800, s1, s6, u(fr + 112), u(fr + 116))
            t1 = m.lw(sp + 4)
            if m.lh(t1 + 2) == 2:
                m.c("two-arm node: |lateral|")
                m.sw(fr + 112, u(absv(m.lw(fr + 112))))
            m.sw(fr + 56, m.lw(fr + 112))
            m.sw(fr + 60, m.lw(fr + 116))
            m.sh(fr + 72, m.lhu(s6 + 14))
            m.sh(fr + 74, m.lhu(s6 + 16))
            m.sh(fr + 76, m.lhu(s6 + 18))
            m.call(0x8003662C, u(fr + 72), u(fr + 40), u(fr + 80))
            k = m.lh(s7 + 18)
            if k >= 0:
                m.sw(fr + 124, u(m.lw(s2 + 56) + 16 * k))
                L = u(m.lw(s2 + 64) + 20 * k)
                g1 = m.lh(L + 4)
                g2 = m.lh(L + 6) if g1 != -1 else -1
                if g1 != -1 and g2 != -1:
                    A = m.lw(fr + 96)
                    fwd = m.lh(A + 4) == m.lh(L + 10)
                    if fwd:
                        B = m.lw(fr + 100)
                        fwd = m.lh(B + 4) == m.lh(L + 12)
                    xsdh = None
                    if fwd:
                        m.c("arms in the record's order")
                        m.sw(fr + 128, u(m.lh(A + 2)))
                        m.sw(fr + 132, u(m.lh(B + 2)))
                        xsdh = m.lw(s2 + 60)
                        s8 = u(xsdh + 264 * g1)
                        m.sw(fr + 120, u(xsdh + 264 * g2))
                    else:
                        m.c("arms in reverse order")
                        xsdh = m.lw(s2 + 60)
                        s8 = u(xsdh + 264 * m.lh(L + 6))
                        a0 = u(xsdh + 264 * m.lh(L + 4))
                        Bp = m.lw(fr + 100)
                        Ap = m.lw(fr + 96)
                        m.sw(fr + 120, a0)
                        if "e754.rev_dirs" in MUT:
                            Bp, Ap = Ap, Bp
                        m.sw(fr + 128, u(m.lh(Bp + 2)))
                        m.sw(fr + 132, u(m.lh(Ap + 2)))
                    a = m.lw(fr + 88)                         # the along distance on the turn
                    if "e754.no_clamp" in MUT:
                        t = a
                    else:
                        t = u((a & u(~sra(a, 31))) + (u(s0 - a) & sra(u(s0 - a), 31)))
                    if s(t) > 0:
                        if s(s0) > 0:
                            s4 = m.call(0x80010028, t, s0)
                        else:
                            s4 = u(-m.call(0x80010028, t, u(-s0)))
                    else:
                        if s(s0) > 0:
                            s4 = u(-m.call(0x80010028, u(-t), s0))
                        else:
                            s4 = m.call(0x80010028, u(-t), u(-s0))
                else:
                    m.c("XSAI record without two groups")
            else:
                m.c("sub-object without XSIH")
        else:
            m.c("no wedge")
    if s8:
        a1 = m.lw(fr + 124)
        a2_ = m.lw(fr + 128)
        a3 = m.lw(fr + 132)
        m.sw(fr + 16, m.lw(fr + 120))
        v0 = m.lw(fr + 112)
        out = m.lw(sp + 8)
        m.sw(fr + 24, s4)
        m.sw(fr + 28, out)
        m.sw(fr + 20, v0)
        m.sw(fr + 32, m.lw(fr + 104))
        r = m.call(0x8003ED14, s8, a1, a2_, a3)
        if s5:
            pc = m.lw(fr + 44)
            if m.lh(pc + 2) == 1:
                m.c("cursor out: the turn's cursor")
                m.sw(s5, m.lw(fr + 40))
                a0 = m.lw(fr + 44)
                v0 = m.lw(fr + 112)
                v1 = m.lw(fr + 116)
                m.sw(s5 + 8, s7)
                m.sw(s5 + 12, s6)
                m.sw(s5 + 16, v0)
                m.sw(s5 + 20, v1)
                m.sw(s5 + 4, a0)
                v0 = m.lw(fr + 108)
                v1 = m.lw(fr + 64)
                m.sw(s5 + 28, v0)
                m.sw(s5 + 24, v1)
            else:
                m.c("cursor out: search left the core, slice = 0")
                m.sw(s5 + 12, 0)
        return r & 0xFF
    m.c("no cross-section: out cleared, returns 1")
    out = m.lw(sp + 8)
    v0 = m.lw(out + 16)
    m.sw(out, 0)
    m.sw(out + 16, v0 & u(-16))
    if s5:
        m.sw(s5 + 12, 0)
    return 1


def _dist(m, s2, fr):
    """RoadShortcut's distance: ApproxLen3 of (e+0xB8 - P) >> 16, P at fr+64; stores fr+80..88."""
    a0 = u(m.lw(s2 + 184) - m.lw(fr + 64))
    v1 = m.lw(fr + 72)
    m.sw(fr + 80, a0)
    a1 = u(m.lw(s2 + 188) - m.lw(fr + 68))
    m.sw(fr + 84, a1)
    v0 = u(m.lw(s2 + 192) - v1)
    m.sw(fr + 88, v0)
    return m.call(0x8001FCB0, sra(a0, 16), sra(a1, 16), sra(v0, 16))


def m_bfe8(m):
    """0x8003BFE8 RoadShortcut(e): off the road near a junction, re-bind to a nearer arm road."""
    R = m.R
    sp = R["sp"]
    fr = u(sp - 144)
    s2 = R["a0"]
    s5 = 0
    if m.lhu(s2 + 362) != 0:
        m.c("on a core piece")
        return
    if m.lw(s2 + 372) == 0:
        return
    if not (m.lw(s2 + 388) & 1):
        m.c("on the road")
        return
    s4 = m.lw(s2 + 328)
    m.call(0x8001E100, u(fr + 16), 0, 32)
    m.sw(fr + 16, s4)
    d = m.call(0x8003A5F4, u(s2 + 360), u(fr + 96), 0)
    if m.lw(fr + 96) == u(-1):
        m.c("no junction at the near end")
        return
    if slt(0x31FFFF, d):
        m.c("50 or more units from the end node")
        return
    s1 = m.lw(s2 + 340)
    m.call(0x8002EAD8, u(s1 + 20), u(s1 + 14), m.lw(s2 + 348), u(fr + 64))
    s8 = _dist(m, s2, fr)
    if m.lh(s4 + 16) == 1 and m.lw(s4 + 12) == 0:
        s5 = m.call(0x80039AFC, m.lw(s4))
    if s5 == 0:
        m.c("not on a junction core object")
        return
    if m.lh(s5 + 2) <= 0:
        return
    s3 = 0
    while True:
        arm = u(s5 + 8 + 24 * s3)
        key = m.lw(s2 + 360)
        own = (key >> 16) == 0 and (key & 0xFFFF) == u(m.lh(arm + 4))
        if own and "bfe8.no_own" not in MUT:
            m.c("the arm of its own road")
        else:
            t1 = m.call(0x80039C90, s4, u(m.lh(arm + 4)))
            if t1 == 0:
                m.c("arm road without a piece")
                return
            v1 = m.lh(t1 + 20)
            dr = m.lh(arm + 2)
            a3 = u(m.lw(s4 + 48) + 28 * v1)
            first = dr > 0
            if "bfe8.last_slice" in MUT:
                first = not first
            if first:
                v0 = m.lh(a3 + 8)
                t0 = 0
                s1 = u(m.lw(s4 + 52) + 52 * v0)
            else:
                v1 = m.lh(a3 + 8)
                v0 = m.lh(a3 + 10)
                s1 = u(m.lw(s4 + 52) + 52 * (v1 + v0) - 52)
                t0 = m.lw(s1 + 32)
            m.sw(fr + 20, t1)
            m.sw(fr + 24, a3)
            m.sw(fr + 28, s1)
            m.sw(fr + 36, t0)
            s1 = m.call(0x80036B14, u(s2 + 172), u(fr + 16), u(s2 + 184))
            s0 = u(s1 + 20)
            lat = m.call(0x800B6AAC, u(s2 + 184), u(s1 + 2), s0)
            s1 = u(s1 + 14)
            m.sw(fr + 32, lat)
            alo = m.call(0x800B6AAC, u(s2 + 184), s1, s0)
            m.sw(fr + 36, alo)
            m.call(0x8003662C, u(s2 + 450), u(fr + 16), u(fr + 48))
            m.call(0x8002EAD8, s0, s1, m.lw(fr + 36), u(fr + 64))
            d2 = _dist(m, s2, fr)
            if slt(d2, s8):
                key = m.lw(s2 + 360)
                nk = m.lw(fr + 48) if (key >> 16) == 0 else None
                if nk is not None and (nk >> 16) == 0 and key != nk:
                    m.c("re-bind to the nearer arm road")
                    m.call(0x8001E0B4, u(s2 + 328), u(fr + 16), 32)
                    m.call(0x8001E0B4, u(s2 + 360), u(fr + 48), 12)
                    m.call(0x8003DE28, s2, 1, 0, u(-1))
                    m.call(0x8003DF54, s2, 1, u(-1))
                    a1 = m.call(0x8003F3B4, m.lw(fr + 96))
                    if a1:
                        m.sw(s2 + 428, a1)
                    a2 = m.lw(s2 + 428)
                    if a2:
                        h = m.lhu(s2 + 172)
                        if (h >> 5) < 2:
                            a0 = h & 0x1F
                            nb = m.lw(BIKE_COUNT)
                            if not slt(a0, nb):
                                m.c("pool 1: slot less the bike count")
                                a0 = u(a0 - nb)
                            _slot_bit(m, a2, a0, a1 != 0)
                    m.call(0x8003AF9C, u(s2 + 172), 1, 0)
                    v0 = m.call(0x8003B61C, u(s2 + 172))
                    m.sw(s2 + 324, v0)
                    return
                m.c("nearer, but on a core or the same road")
            else:
                m.c("not nearer")
        s3 += 1
        if not s3 < m.lh(s5 + 2):
            m.c("no arm is nearer")
            return


MODELS = {
    "bfe8": m_bfe8, "e754": m_e754, "eb58": m_eb58, "ed14": m_ed14, "e45c": m_e45c,
    "e61c": m_e61c, "bd2c": m_bd2c, "f204": m_f204, "b024": m_b024, "b1c4": m_b1c4,
    "a5f4": m_a5f4, "f4d8": m_f4d8, "f580": m_f580, "f3b4": m_f3b4,
}


def _labels() -> dict:
    import inspect
    import re
    out = {}
    for key, fn in MODELS.items():
        src = inspect.getsource(fn)
        out[key] = list(dict.fromkeys(re.findall(r'm\.c\("([^"]+)"\)', src)))
    return out


LABELS = _labels()

# the harness in roadrt.py looks its models and function table up in its own module
RT.FUNCS.update(FUNCS)
RT.MODELS.update(MODELS)


# ---------------------------------------------------------------------------
# Structure, captures
# ---------------------------------------------------------------------------

def bodies(img: E.Image) -> dict:
    out = {}
    for k, (entry, _, _) in FUNCS.items():
        pcs, _, _, rets = RT.cfg(img, entry)
        out[k] = (entry, pcs[-1] + 4, rets)
    return out


QUICK = os.path.join("work", "oracle", "state", "quick")
NATURAL = {
    "quick": (QUICK, os.path.join(WORK, "cap_quick"), "--frames 30"),
    "race": (os.path.join("work", "oracle", "state", "rr-race"), os.path.join(WORK, "cap_race"), "--frames 36"),
}

# Teleports: (name, bike slot, GRPT index in junction object 14 (0 = the core), sub-object offset
# inside that piece, slice position 0..1 inside the sub-object, lateral offset in world units,
# raise "off the road" (+0x184 bit 0) before the first frame[, GRPT index whose slice gives the
# POSITION while the cursor stays on the first one]).
TELEPORTS = [
    ("tp_c0", 1, 0, 0, 0.50, 0.0, False),
    ("tp_c1", 1, 0, 1, 0.25, 6.0, False),
    ("tp_c2", 0, 0, 0, 0.50, 14.0, False),
    ("tp_c3", 0, 0, 2, 0.50, -14.0, False),
    ("tp_c4", 0, 0, 1, 0.50, 0.0, False),
    ("tp_c5", 0, 0, 1, 0.10, 30.0, False),
    ("tp_c6", 0, 0, 0, 0.90, -30.0, False),
    ("tp_c7", 0, 0, 2, 0.20, 5.0, False),
    ("tp_s0", 0, 1, 0, 0.00, 10.0, True),
    ("tp_s1", 0, 2, 0, 0.00, -10.0, True),
    ("tp_s2", 0, 3, 0, 0.00, 16.0, True),
    ("tp_s3", 0, 1, 0, 0.00, -18.0, True),
    ("tp_s4", 0, 2, 0, 0.00, 20.0, True),
    ("tp_s5", 0, 3, 0, 0.00, -22.0, True),
    # cursor on one stub, position on ANOTHER stub's centre line: RoadShortcut must re-bind
    ("tp_x0", 0, 1, 0, 0.00, 2.0, True, 2),
    ("tp_x1", 0, 2, 0, 0.00, -2.0, True, 3),
    ("tp_x2", 0, 3, 0, 0.00, 1.0, True, 1),
]
CAPTURES = dict(NATURAL)
for _t in TELEPORTS:
    # a stub teleport moves the player off the streamed road: the game then touches the CD-ROM,
    # which the oracle does not model; --explore lets the frame continue (the road objects stay
    # resident, and no function checked here touches the drive)
    CAPTURES[_t[0]] = (os.path.join(WORK, "state", _t[0]), os.path.join(WORK, "cap_" + _t[0]),
                       "--frames 30" + (" --explore" if _t[6] else ""))
RT.CAPTURES.update(CAPTURES)       # roadrt.Capture reads its table by name


class Ram:
    def __init__(self, b: bytearray):
        self.b = b

    def w(self, a):
        return struct.unpack_from("<I", self.b, a & 0x1FFFFF)[0]

    def i(self, a):
        return struct.unpack_from("<i", self.b, a & 0x1FFFFF)[0]

    def h(self, a):
        return struct.unpack_from("<h", self.b, a & 0x1FFFFF)[0]

    def put(self, a, v):
        struct.pack_into("<I", self.b, a & 0x1FFFFF, v & 0xFFFFFFFF)


def junction7(r: Ram):
    """The resident core object (id 14), its IPT_ record, and helpers - from the snapshot itself."""
    g = r.w(GRAPH)
    btt, ipt, pdt, gpdt = r.w(g + 0x1C), r.w(g + 0x30), r.w(g + 0x34), r.w(g + 0x38)
    obj = next(r.w(btt + 32 * i + 12) for i in range(r.h(g + 0x28))
               if r.w(btt + 32 * i + 12) and r.w(r.w(btt + 32 * i + 12)) == 14)
    j = next(ipt + 12 * q for q in range(r.h(g + 0x3C)) if r.h(ipt + 12 * q) == 14)
    return g, obj, j, pdt, gpdt


def _stub_slice(r: Ram, g, obj, piece, first, cnt):
    """The slice of a stub next to the core, one step inside (the re-seat enters at `first` when
    the arm's dir > 0)."""
    road = r.h(piece + 12)
    node = r.w(g + 0x24) + 104 * 14
    arm = next(node + 8 + 24 * a for a in range(r.h(node + 2)) if r.h(node + 8 + 24 * a + 4) == road)
    return first + 1 if r.h(arm + 2) > 0 else first + cnt - 2


def teleport(ram: bytearray, bike, pi, so, frac, lat_units, offroad, pos_pi=None):
    """Move pool-0 bike `bike` onto junction 7: every copy of its position is shifted by the same
    vector, and its road cursor is set to the chosen piece / sub-object / slice.  Data only."""
    r = Ram(ram)
    g, obj, j, pdt, gpdt = junction7(r)
    e = r.w(RT.POOL0) + 1096 * bike

    def pick(pi_):
        piece_ = r.w(obj + 0x2C) + 32 * pi_
        core_ = r.h(piece_ + 2) == 1
        sub_ = r.w(obj + 0x30) + 28 * (r.h(piece_ + 20) + (so if core_ else 0))
        first, cnt = r.h(sub_ + 8), r.h(sub_ + 10)
        k = first + int(frac * (cnt - 1)) if core_ else _stub_slice(r, g, obj, piece_, first, cnt)
        return piece_, core_, sub_, r.w(obj + 0x34) + 52 * k
    piece, core, sub, slc = pick(pi)
    pslc = slc if pos_pi is None else pick(pos_pi)[3]
    along = r.i(slc + 0x20) // 2
    palong = r.i(pslc + 0x20) // 2
    lat = int(lat_units * 65536)
    p = [r.i(pslc + 20 + 4 * c) + ((r.h(pslc + 14 + 2 * c) * palong) >> 12) + ((r.h(pslc + 2 + 2 * c) * lat) >> 12)
         for c in range(3)]
    d = [p[c] - r.i(e + 0xB8 + 4 * c) for c in range(3)]
    for base in [0xB8, 0x1D4, 0x1F8, 0xF4] + [0xC4 + 12 * q for q in range(8)]:
        for c in range(3):
            r.put(e + base + 4 * c, r.i(e + base + 4 * c) + d[c])
    turn = 0
    if core:
        for q in range(r.h(j + 8)):
            t = pdt + 12 * (r.h(j + 6) + q)
            gg = gpdt + 12 * r.h(t + 2)
            if r.h(gg + 2) == pi and r.h(gg + 4) == so:
                turn = t
                break
    for off, v in ((0x148, obj), (0x14C, piece), (0x150, sub), (0x154, slc), (0x158, lat),
                   (0x15C, along), (0x160, j if core else 0), (0x164, turn),
                   (0x168, ((1 if core else 0) << 16) | (r.h(piece + 12) & 0xFFFF)),
                   (0x170, r.i(slc + 0x28) + along)):
        r.put(e + off, v)
    if offroad:
        r.put(e + 0x184, r.w(e + 0x184) | 1)
    return e, p


def cmd_capture(args) -> int:
    img = E.load_exe()
    ps = RT.probe_set(img)
    parg = " ".join(f"--probe 0x{a:08X}" for a in sorted(ps))
    exe = os.path.join(ROOT, "build_m0", "rrverify.exe")
    cmds = []
    for name, (snap, cap, extra) in CAPTURES.items():
        cmds.append((name, f"{exe} trace --state {snap} {extra} --no-cop2 --no-gpu --max-steps 60000000 "
                           f"--calls --watch 0x80000000:0x200000:ram --out {cap} {parg}"))
    if args.print:
        print(f"{len(ps)} probes (roadrt.py's resident closure of the re-bind and pass H).")
        for _, c in cmds:
            print(c)
            print()
        return 0
    src = os.path.join(ROOT, QUICK)
    for name, bike, pi, so, frac, lat, off, *pos in TELEPORTS:
        d = os.path.join(ROOT, WORK, "state", name)
        os.makedirs(d, exist_ok=True)
        for f in ("bios.bin", "cpu.json", "gpu.json", "scratchpad.bin", "spuram.bin", "vram.bin"):
            shutil.copyfile(os.path.join(src, f), os.path.join(d, f))
        ram = bytearray(open(os.path.join(src, "ram.bin"), "rb").read())
        e, p = teleport(ram, bike, pi, so, frac, lat, off, *pos)
        open(os.path.join(d, "ram.bin"), "wb").write(ram)
        print(f"{name}: bike 0x{e:08X} -> ({p[0] / 65536:.2f}, {p[1] / 65536:.2f}, {p[2] / 65536:.2f}), "
              f"piece {pi}, sub +{so}, lateral {lat}")
    for name, c in cmds:
        if args.only and name not in args.only.split(","):
            continue
        out = subprocess.run(c, cwd=ROOT, shell=False, capture_output=True, text=True)
        tail = [ln for ln in out.stdout.splitlines() if ln.startswith(("stopped", "buffer swaps", "wrote"))]
        print(f"== {name}: exit {out.returncode}; " + " | ".join(tail))
    return 0


# ---------------------------------------------------------------------------
# PyCpu against the oracle, on the natural (and teleport) invocations
# ---------------------------------------------------------------------------

def replay_natural(cap, bods, limit=150):
    bl = [(v[0], v[1]) for v in bods.values()]
    n = bad = 0
    msgs = []
    for key in FUNCS:
        cache = [bytearray(cap.ram0), 0]
        done = 0
        for eseq, regs, xseq, _ in RT.invocations(cap, key, bods):
            if done >= limit:
                break
            done += 1
            img = RT.ram_at(cap, eseq, cache)
            cpu = RT.Cpu(img, cap.scratch_at(eseq), dict(regs), set(), bl)
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
            win = (ks[0], ks[-1]) if ks else (1, 0)          # an interrupt inside: drop the kernel stores
            want = [(cap.waddr[j], cap.wsize[j], cap.wval[j] & ((1 << (8 * cap.wsize[j])) - 1))
                    for j in range(j0, j1) if not win[0] <= cap.wseq[j] <= win[1]]
            got = [(w[2], w[3], w[4]) for w in cpu.writes if w[2] < 0x200000]
            if got != want:
                bad += 1
                if len(msgs) < 8:
                    msgs.append(f"{cap.name} {key} @{eseq}: PyCpu {len(got)} stores, oracle {len(want)}")
    return n, bad, msgs


# ---------------------------------------------------------------------------
# Synthetic inputs: the ORIGINAL bytes executed by PyCpu on states derived from `quick`
# ---------------------------------------------------------------------------

def synth_cases(n: int, seed: int):
    rnd = random.Random(seed)
    base = bytearray(open(os.path.join(ROOT, QUICK, "ram.bin"), "rb").read())
    scr0 = bytes(open(os.path.join(ROOT, QUICK, "scratchpad.bin"), "rb").read())
    gp = json.load(open(os.path.join(ROOT, QUICK, "cpu.json")))["gpr"]["gp"]
    r0 = Ram(base)
    g, obj, j, pdt, gpdt = junction7(r0)
    pool0 = r0.w(RT.POOL0)
    nb = r0.w(BIKE_COUNT)
    bikes = [pool0 + 1096 * i for i in range(nb)]
    nrec = r0.h(ROUTE + 18)
    recs = [r0.w(ROUTE + 36) + 120 * i for i in range(nrec + 1)]    # + the start record at [count]
    node14 = r0.w(g + 0x24) + 104 * 14
    bit_all = [r0.w(g + 0x24) + 104 * i for i in range(r0.h(g + 0x2C))]
    arms = [node14 + 8 + 24 * a for a in range(r0.h(node14 + 2))]
    cx = sum(r0.i(a + 12) for a in arms) // len(arms)
    cz = sum(r0.i(a + 20) for a in arms) // len(arms)
    cy = r0.i(arms[0] + 16)
    riders = [r0.w(b + 0x354) for b in bikes if r0.w(b + 0x354)]
    import chunk as C
    groups = []
    for sn in (1, 2):
        for o in C.load_type3(os.path.join(ROOT, "work", "disc_us", "DATA", f"STREAM{sn}.STR")).values():
            if "XSDH" in o.bl:
                off, sz = o.bl["XSDH"]
                groups += [o.raw[off + 8 + i:off + 8 + i + 264] for i in range(0, sz - 8, 264)]
    sp = 0x801FFC00

    def place(ram, e, pi, so, frac, lat, core_turn=True):
        r = Ram(ram)
        piece = r.w(obj + 0x2C) + 32 * pi
        core = r.h(piece + 2) == 1
        sub = r.w(obj + 0x30) + 28 * (r.h(piece + 20) + so)
        first, cnt = r.h(sub + 8), r.h(sub + 10)
        k = first + min(cnt - 1, int(frac * cnt))
        slc = r.w(obj + 0x34) + 52 * k
        along = rnd.randint(-2 * 65536, r.i(slc + 0x20) + 2 * 65536)
        p = [r.i(slc + 20 + 4 * c) + ((r.h(slc + 14 + 2 * c) * along) >> 12) + ((r.h(slc + 2 + 2 * c) * lat) >> 12)
             for c in range(3)]
        for base_ in (0xB8, 0x1F8):
            for c in range(3):
                r.put(e + base_ + 4 * c, p[c] + (rnd.randint(-65536, 65536) if base_ == 0x1F8 else 0))
        turn = 0
        if core:
            cands = [pdt + 12 * (r.h(j + 6) + q) for q in range(r.h(j + 8))]
            match = [t for t in cands if r.h(gpdt + 12 * r.h(t + 2) + 4) == so]
            turn = rnd.choice(match or cands) if core_turn else rnd.choice(cands)
        jj = j if (core and rnd.random() < 0.9) else 0
        for off, v in ((0x148, obj), (0x14C, piece), (0x150, sub), (0x154, slc), (0x158, lat),
                       (0x15C, along), (0x160, jj), (0x164, turn),
                       (0x168, ((1 if core else 0) << 16) | (r.h(piece + 12) & 0xFFFF)),
                       (0x170, r.i(slc + 0x28) + along)):
            r.put(e + off, v)
        f184 = r.w(e + 0x184) & ~0x61
        f184 |= rnd.choice([0, 1, 1]) | (0x20 if rnd.random() < 0.3 else 0) | (0x40 if rnd.random() < 0.2 else 0)
        r.put(e + 0x184, f184)
        if rnd.random() < 0.2:
            r.put(e + 0x174, 0)
        if rnd.random() < 0.3:
            r.put(e + 0x178, 0)
        r.put(e + 0x1F0, rnd.choice([0, 0, u(-1)]))
        r.put(e + 0x1EC, rnd.choice([0, u(-1), slc + 44]))
        return p

    for k in range(n):
        ram = bytearray(base)
        r = Ram(ram)
        regs = {"gp": gp, "sp": sp, "ra": Q.PyCpu.STOP}
        fam = k % 8
        e = rnd.choice(bikes)
        if fam in (0, 1, 2):
            # an entity on the core, classified: NodeZone, RoadClass -> RoadClassCore, RoadsideRun
            so = rnd.randrange(3)
            place(ram, e, 0, so, rnd.random(), rnd.randint(-45, 45) * 65536 + rnd.randrange(65536),
                  core_turn=rnd.random() < 0.85)
            if fam == 0:
                regs.update(a0=u(e))
                yield ("core-classify", 0x8003DFF4, bytes(ram), scr0, regs)
            elif fam == 1:
                regs.update(a0=u(e), a1=rnd.choice([0, 1]), a2=rnd.choice([0, u(sp + 0x100)]),
                            a3=u(rnd.choice([-1, 0, 1, 2, 3, 7])))
                yield ("core-class", 0x8003DE28, bytes(ram), scr0, regs)
            else:
                if rnd.random() < 0.5:
                    # redraw the node zone records (BGDT 0..3 of object 14): the road-pair arms, a
                    # record past the cap of 3, and no record at all - data edits only
                    bg = r.w(obj + 0x5C)
                    roads = [r.h(a + 4) for a in arms] + [rnd.randrange(40)]
                    for q in range(4):
                        rec = (bg + 20 * q) & 0x1FFFFF
                        ram[rec + 2] = rnd.choice([0, 1, 1])
                        ram[rec + 3] = rnd.randrange(4)
                        ram[rec + 8] = rnd.choice([255, rnd.choice(roads)])
                        ram[rec + 9] = rnd.choice(roads)
                regs.update(a0=u(e), a1=rnd.choice([0, 1]), a2=u(rnd.choice([-1, 0, 1, 2, 3])))
                yield ("core-run", 0x8003DF54, bytes(ram), scr0, regs)
        elif fam == 3:
            # off the road on a stub: the re-bind with the re-seat -> RoadShortcut
            pi = rnd.choice([1, 2, 3])
            place(ram, e, pi, 0, rnd.choice([0.0, 0.1, 0.5, 0.9, 1.0]),
                  rnd.choice([-1, 1]) * rnd.randint(4, 40) * 65536)
            r.put(e + 0x184, r.w(e + 0x184) | 1)
            if r.w(e + 0x174) == 0:
                r.put(e + 0x174, r.w(obj + 0x3C))
            regs.update(a0=u(e), a1=1)
            yield ("shortcut", 0x800374D4, bytes(ram), scr0, regs)
        elif fam == 4:
            # the route binding, any pool, any record
            which = rnd.random()
            if which < 0.6:
                p = e + 0xAC
            elif which < 0.8 and riders:
                p = rnd.choice(riders) + 0xAC
            else:
                p = rnd.choice([0x800CF660 + 512 * rnd.randrange(4), 0x800CD898]) + 0xAC
            key = rnd.choice([(1 << 16) | 7, (1 << 16) | rnd.randrange(12), 9, 10, 8, 4, 0,
                              rnd.randrange(40)])
            r.put(p + 0xBC, key)
            r.put(p + 0xC4, rnd.choice([0, rnd.randint(-80, 80) * 65536, rnd.randint(0, 3000) * 65536]))
            r.put(p + 0xC0, rnd.choice([1, u(-1)]))
            r.put(p + 0x100, rnd.choice(recs + [0]))
            if rnd.random() < 0.1:
                struct.pack_into("<h", ram, (ROUTE + 18) & 0x1FFFFF, rnd.choice([0, 1]))
            x = rnd.choice([0, 0, rnd.choice(bikes)])
            regs.update(a0=u(p), a1=rnd.choice([0, 1, 1]), a2=u(x))
            yield ("route", 0x8003AF9C, bytes(ram), scr0, regs)
        elif fam == 5:
            # the wedge test, on junction 7's core record or any record of the table
            nd = node14 if rnd.random() < 0.7 else rnd.choice(bit_all)
            P = sp + 0x180
            for c, v in enumerate((cx, cy, cz)):
                r.put(P + 4 * c, v + rnd.randint(-60, 60) * 65536 + rnd.randrange(65536))
            r.put(sp + 16, u(rnd.choice([-1, -1, 0, 1, 2, 3, 4])))
            regs.update(a0=P, a1=nd, a2=rnd.choice([0, sp + 0x1A0]), a3=rnd.choice([0, sp + 0x1A4]))
            yield ("wedge", 0x8003EB58, bytes(ram), scr0, regs)
        elif fam == 6:
            # the core cross-section on two groups copied from the player's disc
            ga, gb = sp + 0x200, sp + 0x310
            ram[ga & 0x1FFFFF:(ga & 0x1FFFFF) + 264] = rnd.choice(groups)
            ram[gb & 0x1FFFFF:(gb & 0x1FFFFF) + 264] = rnd.choice(groups)
            out = sp + 0x440
            edges = [abs(struct.unpack_from("<i", ram, (x + o) & 0x1FFFFF)[0])
                     for x in (ga, gb) for o in (16, 32, 144, 160)]
            lat = rnd.choice([rnd.randint(-40 * 65536, 40 * 65536), rnd.choice(edges) + rnd.randint(-2, 2),
                              -rnd.choice(edges) + rnd.randint(-2, 2)])
            for i, v in enumerate((gb, u(lat), rnd.choice([0, 0x10000, rnd.randrange(0x10000)]), out,
                                   u(rnd.choice([1, -1])))):
                r.put(sp + 16 + 4 * i, v)
            regs.update(a0=ga, a1=r.w(obj + 0x38) + 16 * rnd.randrange(3),
                        a2=u(rnd.choice([1, -1])), a3=u(rnd.choice([1, -1])))
            yield ("edge", 0x8003ED14, bytes(ram), scr0, regs)
        else:
            # the road-end query and the three record lookups, directly
            P = sp + 0x180
            # roads 11 and 12 end at node 8, the graph's one node of degree 2
            r.put(P, rnd.choice([(1 << 16) | 7, (1 << 16) | 8, 9, 10, 8, 11, 12, rnd.randrange(37)]))
            r.put(P + 4, rnd.choice([1, u(-1)]))
            r.put(P + 8, rnd.choice([rnd.randint(-100, 3000) * 65536, rnd.randint(0, 60) * 65536]))
            sub = rnd.random()
            if sub < 0.5:
                regs.update(a0=P, a1=sp + 0x1A0, a2=rnd.choice([0, 1, 2]))
                yield ("endnode", 0x8003A5F4, bytes(ram), scr0, regs)
            elif sub < 0.7:
                regs.update(a0=rnd.choice([7, 4, 1, 0, u(-1), 3]))
                yield ("bykey", 0x8003F3B4, bytes(ram), scr0, regs)
            elif sub < 0.85:
                regs.update(a0=rnd.choice([9, 10, 8, 0, 4, 2, 26, rnd.randrange(40)]))
                yield ("byroad", 0x8003F4D8, bytes(ram), scr0, regs)
            else:
                regs.update(a0=rnd.choice(recs + [0]), a1=u(rnd.choice([10, 0, 4, 9, -1, 26])))
                yield ("exit", 0x8003F580, bytes(ram), scr0, regs)


def run_synthetic(n, seed, bods, pset, excl, which, cov, stats, fails, verbose):
    bl = [(v[0], v[1]) for v in bods.values()]
    rejected, fams = Counter(), Counter()
    for fam, entry, ram0, scr0, regs in synth_cases(n, seed):
        cpu = RT.Cpu(bytearray(ram0), scr0, dict(regs), pset, bl)
        try:
            cpu.go(entry, Q.PyCpu.STOP)
        except CpuStop as ex:
            rejected[str(ex).split(" 0x")[0]] += 1
            continue
        fams[fam] += 1
        cap = RT.SynthCapture("synth", ram0, scr0, cpu)
        for key in which:
            RT.run_model(cap, key, bods, excl, cov, stats, fails, verbose)
    return rejected, fams


# ---------------------------------------------------------------------------
# Data and static checks
# ---------------------------------------------------------------------------

def data_checks() -> list[str]:
    """The data these functions lean on, read from the player's own STREAM<n>.STR and snapshots."""
    import chunk as C
    bad = []
    for sn in (1, 2):
        objs = C.load_type3(os.path.join(ROOT, "work", "disc_us", "DATA", f"STREAM{sn}.STR"))
        xa, h2, bg, nbg, ends = Counter(), Counter(), Counter(), Counter(), Counter()
        for oid, o in objs.items():
            b = o.raw

            def blk(t):
                if t not in o.bl:
                    return b"", 0
                off, sz = o.bl[t]
                return b[off + 8:off + sz], sz - 8
            xs, xsz = blk("XSIH")
            xai, _ = blk("XSAI")
            xd, xdsz = blk("XSDH")
            bgd, bgsz = blk("BGDT")
            ng = xdsz // 264
            kind = struct.unpack_from("<h", b, 0x30)[0]
            half = struct.unpack_from("<i", b, 0x2C)[0]
            for i in range(xsz // 16):
                grp = struct.unpack_from("<h", xs, 16 * i + 4)[0]
                rr = struct.unpack_from("<7h", xai, 20 * i)
                key = ("core" if grp < 0 else "road", rr[1],
                       "g1,g2 in range" if 0 <= rr[2] < ng and 0 <= rr[3] < ng else
                       ("+6 = -1" if rr[3] == -1 else "other"))
                xa[key] += 1
            for i in range(0, xdsz, 264):
                for hh in (8, 136):
                    h2[struct.unpack_from("<h", xd, i + hh + 2)[0]] += 1
            n4e = struct.unpack_from("<h", b, 0x20 + 0x4E)[0]
            nbg[n4e == bgsz // 20] += 1
            for i in range(min(3, bgsz // 20)):
                rec = bgd[20 * i:20 * i + 20]
                if kind == 1 and half == 0 and rec[2] == 1:
                    bg["core zone record, +8 = 255" if rec[8] == 255 else "core zone record, road pair"] += 1
            if kind == 1 and half == 0:
                nz = sum(1 for i in range(min(3, bgsz // 20)) if bgd[20 * i + 2] == 1)
                ends[nz] += 1
        if xa.get(("core", 1, "g1,g2 in range"), 0) != 73 or xa.get(("road", 2, "+6 = -1"), 0) != 310:
            bad.append(f"STREAM{sn}: XSAI census {dict(xa)}")
        if set(h2) != {1}:
            bad.append(f"STREAM{sn}: XSDH half +2 values {dict(h2)}")
        if nbg.get(False):
            bad.append(f"STREAM{sn}: object +0x4E is not the BGDT count on {nbg[False]} objects")
        print(f"  STREAM{sn}.STR: XSAI by (XSIH kind, +2, groups) {dict(xa)}; XSDH half +2 {dict(h2)}; "
              f"object +0x4E == BGDT count on {nbg[True]} of {sum(nbg.values())}; core objects' first 3 "
              f"BGDT records with +2 == 1: {dict(bg)}, per object {dict(sorted(ends.items()))}")
    for name in ("quick", "rr-race", "rr-pack", "rr-grid"):
        p = os.path.join(ROOT, "work", "oracle", "state", name, "ram.bin")
        if not os.path.exists(p):
            continue
        r = Ram(bytearray(open(p, "rb").read()))
        gpv = json.load(open(os.path.join(ROOT, "work", "oracle", "state", name, "cpu.json")))["gpr"]["gp"]
        rg = r.w(gpv + 472)
        nn = r.w(rg + 0x0C)
        deg = Counter(r.w(r.w(rg + 20) + 40 * i + 4) for i in range(nn))
        cnt = r.h(ROUTE + 18)
        st = r.w(ROUTE + 36) + 120 * cnt
        bound = Counter()
        pool0 = r.w(RT.POOL0)
        for i in range(r.w(BIKE_COUNT)):
            e = pool0 + 1096 * i
            rec = r.w(e + 0x1AC)
            bound["start" if rec == st else ("record" if rec else "none")] += 1
        grf = "none"
        for sn in (1, 2):
            fb = open(os.path.join(ROOT, "work", "disc_us", "DATA", f"STREAM{sn}.GRF"), "rb").read()
            live = bytes(r.b[rg & 0x1FFFFF:(rg & 0x1FFFFF) + len(fb)])
            fo = struct.unpack_from("<2I", fb, 0x14)
            if (live[:0x14] == fb[:0x14] and live[0x1C:] == fb[0x1C:]
                    and struct.unpack_from("<2I", live, 0x14) == (rg + fo[0], rg + fo[1])):
                grf = f"STREAM{sn}.GRF verbatim ({len(fb)} B) with +0x14/+0x18 = base + file offset"
        if grf == "none":
            bad.append(f"{name}: the race graph at 0x{rg:08X} is not a STREAM<n>.GRF image")
        print(f"  {name}: *(gp+472) = {grf}")
        spu = r.w(0x8005A41C)
        print(f"  {name}: graph @0x{rg:08X} {nn} nodes, degree {dict(sorted(deg.items()))}; {cnt} route "
              f"records + the start record (key {r.i(st)}, mask 0x{r.h(st + 0x76) & 0xFFFF:04X}); "
              f"bikes bound {dict(bound)}; *(0x8005A41C) = 0x{spu:08X}")
        if spu != 0x1F801C00:
            bad.append(f"{name}: the libspu register base is 0x{spu:08X}")
    return bad


def static_checks(img: E.Image) -> list[str]:
    bad = []

    def want(addr, word, what):
        if img.word(addr) != word:
            bad.append(f"0x{addr:08X} {what}: 0x{img.word(addr):08X} != 0x{word:08X}")
    want(0x8003C080, 0x3C020031, "lui v0,0x31 - RoadShortcut's gate 0x31FFFF")
    want(0x8003C084, 0x3442FFFF, "ori v0,v0,0xffff")
    want(0x8003B404, 0x3C030032, "lui v1,0x32 - RouteBindStep's gate 0x320000")
    want(0x8003A660, 0x00021182, "srl v0,v0,6 - the graph road length")
    want(0x8003A6C4, 0x2C420003, "sltiu v0,v0,3 - a junction has degree >= 3")
    want(0x8003F280, 0x28620004, "slti v0,v1,4 - RoadsideRunNode reads at most 3 records")
    want(0x8003E4A4, 0x02A08821, "move s1,s5 - CoreEdgeClass zeroes a lateral against the turn")
    want(0x80050624, 0xA4430184, "sh v1,388(v0) - SpuSetReverbDepth: 0x1F801D84")
    want(0x80050658, 0xA4430186, "sh v1,390(v0) - 0x1F801D86")
    want(0x80019300, 0x0C007C15, "jal 0x8001F054 - AudioFrame sets the reverb depth")
    # nothing under the re-bind is unread: its resident closure (plus the two neighbour functions
    # RoadSliceSearch reaches through its one jalr pair) against the probes that transcribe it
    here = {FUNCS[k][0] for k in FUNCS} | {0x800245DC, 0x800245F4}
    rt = {v[0] for k, v in RT.FUNCS.items() if k not in FUNCS}
    rq = {0x80036B14, 0x8003697C, 0x80037A30, 0x80037FBC, 0x80039048, 0x8003CCA0, 0x800394F0,
          0x80039A08, 0x80039AA0, 0x80039AFC, 0x80039B60, 0x80039BB0, 0x80039C38, 0x8003A37C,
          0x80039C90, 0x8003C840, 0x8003C948, 0x8003F408, 0x8003F5D0}
    ported = {0x80010028, 0x8001E0B4, 0x8001E100, 0x8001FC58, 0x8001FCB0, 0x8002E698, 0x8002EAD8,
              0x80036800, 0x8003B4B0, 0x800B6AAC}
    cl = RT.closure(img, [0x800374D4, 0x80037A30, 0x80037FBC])
    reach = set(cl) | {t for f in cl.values() for _, t in f[1]}
    jalr = sorted(pc for f, v in cl.items() for pc in v[2])
    unknown = sorted(reach - here - rt - rq - ported)
    print(f"  closure of 0x800374D4: {len(reach)} functions ({len(reach & here)} here, "
          f"{len(reach & rt)} road runtime, {len(reach & rq)} road query, {len(reach & ported)} "
          f"ported rows), jalr at {', '.join(f'0x{x:08X}' for x in jalr)}; unread: "
          f"{', '.join(f'0x{x:08X}' for x in unknown) or 'none'}")
    if unknown or jalr != [0x80036CE8, 0x80036EC0]:
        bad.append("the re-bind's closure holds unread functions or an unexpected jalr")
    return bad


# ---------------------------------------------------------------------------
# verify / mutate
# ---------------------------------------------------------------------------

def load_caps(bods, verbose=False):
    caps = []
    for name in CAPTURES:
        d = os.path.join(ROOT, CAPTURES[name][1])
        if not os.path.exists(os.path.join(d, "watch.csv")):
            print(f"  capture '{name}' missing ({d}) - run `roadrb.py capture` first")
            continue
        caps.append(RT.Capture(name, bods, verbose))
    return caps


def evaluate(caps, bods, pset, excl, which, args, verbose):
    cov = defaultdict(Counter)
    stats = Counter()
    fails = []
    for cap in caps:
        for key in which:
            RT.run_model(cap, key, bods, excl, cov, stats, fails, verbose)
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
    RT.EXE_IMG = img
    bods = bodies(img)
    pset = RT.probe_set(img)
    excl = {k: RT.save_pcs(img, b[0], b[1]) for k, b in bods.items()}
    which = [args.only] if args.only else list(FUNCS)
    sbad = static_checks(img) + data_checks()
    caps = load_caps(bods, args.verbose)
    for c in caps:
        for k, b in bods.items():
            for a in range(b[0], b[1], 4):
                if int.from_bytes(c.ram0[a & 0x1FFFFF:(a & 0x1FFFFF) + 4], "little") != img.word(a):
                    sbad.append(f"{c.name}: resident code of {k} differs from the EXE at 0x{a:08X}")
                    break
    for b in sbad:
        print("  static FAIL", b)
    if args.mutate:
        return cmd_mutate(args, caps, bods, pset, excl)
    nat_n = nat_bad = 0
    for c in caps:
        n, bad, msgs = replay_natural(c, bods)
        nat_n += n
        nat_bad += bad
        for ml in msgs:
            print("    PyCpu:", ml)
    print(f"  PyCpu vs oracle: {nat_n} oracle invocations re-executed, {nat_bad} with a different store stream")
    cov, stats, fails, rej, fams = evaluate(caps, bods, pset, excl, which, args, args.verbose)
    if args.synthetic:
        print(f"  synthetic: {args.synthetic} cases, seed {args.seed}, executed {dict(sorted(fams.items()))}, "
              f"rejected by PyCpu {sum(rej.values())} {dict(rej)}")
    total = tf = 0
    capnames = [c.name for c in caps]
    for key in which:
        n = stats[key]
        nf = len([f for f in fails if f[0] == key])
        total += n
        tf += nf
        nat = sum(stats[key + "." + c] for c in ("quick", "race"))
        tp = sum(stats[key + "." + c] for c in capnames if c.startswith("tp_"))
        print(f"  {key:5s} 0x{FUNCS[key][0]:08X} {NAMES[key]:18s} invocations {n:6d} (natural {nat}, "
              f"teleport {tp}, synth {stats[key + '.synth']})  mismatches {nf}  "
              f"{'PASS' if nf == 0 and n > 0 else ('FAIL' if nf else 'UNRUN')}")
        print(f"        compared: {stats[key + '.stores']} stores, {stats[key + '.loads']} distinct loads, "
              f"{stats[key + '.calls']} calls")
        if args.coverage:
            for lab in LABELS.get(key, []):
                o, p_ = cov[(key, "oracle")][lab], cov[(key, "pycpu")][lab]
                tag = "" if o else ("   <- PyCpu only" if p_ else "   <- NEVER")
                print(f"          oracle {o:6d}  pycpu {p_:6d}  {lab}{tag}")
    for f in fails[:12]:
        print(f"    FAIL {f[1]} {f[0]} @{f[2]}: {f[3]}")
    unrun = [k for k in which if stats[k] == 0]
    ok = tf == 0 and nat_bad == 0 and not sbad and not unrun
    print(f"roadrb: {len(which)} functions, {total} invocations, {tf} mismatches, {nat_bad} PyCpu/oracle "
          f"differences, {len(sbad)} static failures, {len(unrun)} unrun -> {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


def cmd_mutate(args, caps, bods, pset, excl) -> int:
    caught = 0
    names = list(MUTANTS)
    if args.mutants:
        names = [n for n in names if n in args.mutants.split(",")]
    for mname in names:
        MUT.clear()
        MUT.add(mname)
        key = mname.split(".")[0]
        _, stats, fails, _, _ = evaluate(caps, bods, pset, excl, [key], args, False)
        MUT.clear()
        ora = len([f for f in fails if f[1] != "synth"])
        syn = len([f for f in fails if f[1] == "synth"])
        no = stats[key] - stats[key + ".synth"]
        ok = (ora + syn) > 0
        caught += ok
        print(f"  mutant {mname:18s} {'CAUGHT' if ok else 'MISSED'}  oracle {ora:4d}/{no:<5d} "
              f"synthetic {syn:4d}/{stats[key + '.synth']:<5d} ({MUTANTS[mname]})")
    print(f"roadrb --mutate: {caught} of {len(names)} mutants caught")
    return 0 if caught == len(names) else 1


# ---------------------------------------------------------------------------
# consumers: who reads what this layer writes (our own scan of all four images)
# ---------------------------------------------------------------------------

def _images():
    return [("SLUS", E.load_exe())] + [(n[:7], E.load_overlay(n, 0x8005B5E8))
                                        for n in ("RASHCDF.BIN", "RASHCDG.BIN", "RASHCDI.BIN")]


def _fstart(img, a):
    while not (E.OP(img.word(a)) == 9 and E.RS(img.word(a)) == 29 and E.RT(img.word(a)) == 29
               and E.SIMM(img.word(a)) < 0):
        a -= 4
    return a


def _writes(w):
    op = E.OP(w)
    if op == 0:
        return None if E.FUNCT(w) in (8, 0x18, 0x19, 0x1A, 0x1B, 0x11, 0x13) else E.RD(w)
    if op == 3:
        return 31
    if op in (1, 2, 4, 5, 6, 7, 0x28, 0x29, 0x2A, 0x2B, 0x2E, 0x32, 0x3A):
        return None
    if op == 0x12:
        return E.RT(w) if E.RS(w) in (0, 2) else None
    return E.RT(w)


def field_reads(img, base_off, off, window=40):
    """Loads at `off` from a register loaded as `lw r, base_off(x)` in a straight-line window,
    with the first mask the loaded value meets (shifts folded): [(pc of the lw, pc of the load,
    op, effective mask or None)]."""
    out = []
    hi = img.base + len(img.data)
    for a in range(img.base, hi - 4 * window, 4):
        w = img.word(a)
        if not (E.OP(w) == 0x23 and E.IMM(w) == base_off):
            continue
        regs = {E.RT(w)}
        for k in range(1, window):
            b = a + 4 * k
            v = img.word(b)
            op = E.OP(v)
            if op in E.LOAD_OPS and E.RS(v) in regs and E.SIMM(v) == off:
                t, sh, mask = E.RT(v), 0, None
                for jn in range(1, 8):
                    x = img.word(b + 4 * jn)
                    if E.OP(x) == 0 and E.FUNCT(x) in (2, 3) and E.RT(x) == t:
                        sh += (x >> 6) & 31
                        t = E.RD(x)
                    elif E.OP(x) in (0x0C, 0x0D) and E.RS(x) == t:
                        mask = ((E.IMM(x) << sh) & 0xFFFFFFFF, "andi" if E.OP(x) == 0x0C else "ori")
                        break
                if mask is None and sh == 7 and op == 0x24:     # lbu then srl 7: bit 7 as 0/1
                    mask = (0x80, "andi")
                out.append((a, b, E.LOAD_OPS[op], mask))
            if op == 0 and E.FUNCT(v) == 0x21 and E.RS(v) in regs and E.RT(v) == 0:
                regs.add(E.RD(v))
                continue
            wr = _writes(v)
            if wr in regs:
                regs.discard(wr)
            if not regs or (op == 0 and E.FUNCT(v) == 8 and E.RS(v) == 31):
                break
    return out


def any_reads(img, off, bits):
    """Every load at `off` (any base) whose value meets an andi/shift covering `bits`."""
    out = []
    for a in range(img.base, img.base + len(img.data) - 40, 4):
        v = img.word(a)
        if E.OP(v) not in (0x20, 0x21, 0x23, 0x24, 0x25) or E.SIMM(v) != off:
            continue
        t, sh = E.RT(v), 0
        for jn in range(1, 8):
            x = img.word(a + 4 * jn)
            if E.OP(x) == 0 and E.FUNCT(x) in (2, 3) and E.RT(x) == t:
                sh += (x >> 6) & 31
                t = E.RD(x)
            elif E.OP(x) == 0x0C and E.RS(x) == t:
                eff = (E.IMM(x) << sh) & 0xFFFFFFFF
                if eff & bits:
                    out.append((a, a + 4 * jn, eff, E.REGNAMES[E.RS(v)]))
                break
    return out


def cmd_consumers(_args) -> int:
    imgs = _images()
    print("riderDef = entity +0x43C (lw r,1084(x)); byte +0x00 and +0x44 read through it:")
    for name, img in imgs:
        for a, b, op, mask in field_reads(img, 1084, 0):
            if mask and mask[1] == "andi" and mask[0] & 0x80:
                f = _fstart(img, b)
                print(f"  {name} 0x{b:08X} {op} riderDef[0] & 0x{mask[0]:X}   (function 0x{f:08X})")
        for a, b, op, mask in field_reads(img, 1084, 68):
            f = _fstart(img, b)
            m_ = f"{mask[1]} 0x{mask[0]:X}" if mask else "stored back"
            print(f"  {name} 0x{b:08X} {op} riderDef[+0x44], then {m_}   (function 0x{f:08X})")
    print("any load at +0x44 tested with bit 0, any base register:")
    for name, img in imgs:
        for a, b, eff, base in any_reads(img, 68, 1):
            print(f"  {name} 0x{a:08X} (base {base}) & 0x{eff:X} at 0x{b:08X}   (function 0x{_fstart(img, a):08X})")
    print("entity header +0x24 read with a mask meeting bits 5..6 (0x60), 8 (0x100) or 9 (0x200):")
    for name, img in imgs:
        for a, b, eff, base in any_reads(img, 36, 0x360):
            print(f"  {name} 0x{a:08X} (base {base}) field 0x{eff:X} at 0x{b:08X}   (function 0x{_fstart(img, a):08X})")
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
    v.add_argument("--synthetic", type=int, default=1600, help="synthetic cases (0 = none)")
    v.add_argument("--seed", type=int, default=0x5EED)
    c = sub.add_parser("capture")
    c.add_argument("--print", action="store_true")
    c.add_argument("--only", help="comma list of capture names to run")
    sub.add_parser("consumers")
    a = ap.parse_args()
    return {"verify": cmd_verify, "capture": cmd_capture, "consumers": cmd_consumers}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
