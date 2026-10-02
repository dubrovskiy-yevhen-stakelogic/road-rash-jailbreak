"""Scout probe for THE PER-BIKE STEP, RASHCDG 0x80075EE0 (Road Rash: Jailbreak, USA, SLUS_01053).

The function is 11988 bytes, 2997 instructions, one `jr ra`. This probe maps it, re-derived from
the instruction words of the player's own RASHCDG.BIN / SLUS_010.53:

  blocks    every basic block (leaders: the entry, every branch/jump target inside the function,
            the instruction after every branch's delay slot), its phase and its successors.
  calls     every `jal` / `jalr` site, its phase, and the argument registers as the dataflow below
            resolves them (pointer provenance only - numbers are not evaluated).
  fields    every load/store whose base register is resolved to a known object (the stepped bike,
            another bike, its stat block, its rider, the game state...), merged per offset with
            width, signedness, R/W and phases.
  globals   every absolute address and every table the function reads or writes.
  callee    for every callee: extent, instruction count, `jal` targets, `jalr` / jump tables, and
            which of a0..a3 it reads before writing (i.e. how many arguments it really takes).
  probes    the `--probe` arguments for `rrverify trace` (one per block leader), to a file.
  coverage  reads the probes.csv of one or more `rrverify trace` runs and reports, per block and
            per phase, whether it ran and for which pool-0 slots (the slot is recovered from the
            register the dataflow proves holds the bike at that block's entry).
  cuts      the proposed porting regions: live-in registers / stack words at each entry (a
            backward liveness pass over the whole CFG), and whether each region is single-entry
            single-exit with no pending load delay at its entry.
  remaining the step's remaining callees: callee sets with ported status, arity,
            every call site in RASHCDG / SLUS, the census of the five bike lists against
            0x80071BCC's rule over every RAM image, and the probe hits of the part-1 trace runs
            under work\\bike_step\\p1.
  stats     where entity+0x22C points and what fills it: the RASHCDI loader
            facts, the census of every bike's stat pointer and every block's bytes against the
            .PH files over every RAM image, every store through a +0x22C pointer, the live watch.
  verify    the assertions a gate can match (phase tiling, block/call counts, no jalr, the
            regions' entry/exit and live-in sets, and the remaining-callee and stat-block claims).

Also reads RASHCDI.BIN / RASHCDF.BIN and DATA\\*.PH / GLOBALS.BI under work\\disc_us, and the RAM
images under work\\oracle (never copied anywhere).

The dataflow is a forward pointer-provenance analysis over the CFG: a register is either a known
constant, a pointer (object kind + byte offset, or "indexed"), or unknown; joins keep only what all
predecessors agree on. It never evaluates arithmetic on numbers.

Reads ONLY from work\\disc_us and from probes.csv / ram.bin under work\\. Writes ONLY to stdout
(and to the file `probes` is told to write). Never copies game bytes into the repo.

Usage (from the project root):

    python tools\\scout\\bike_step.py blocks
    python tools\\scout\\bike_step.py calls
    python tools\\scout\\bike_step.py fields
    python tools\\scout\\bike_step.py globals
    python tools\\scout\\bike_step.py callee
    python tools\\scout\\bike_step.py probes --out work\\bike_step\\probes.args
    python tools\\scout\\bike_step.py coverage --csv work\\bike_step\\cov_rr-race_0xFFFF\\probes.csv [...]
    python tools\\scout\\bike_step.py cuts
    python tools\\scout\\bike_step.py remaining
    python tools\\scout\\bike_step.py stats
    python tools\\scout\\bike_step.py verify [--mutate]

`verify` ends with a single line a gate can match:

    bike_step: <n> checks, <m> failures

`verify --mutate` perturbs each documented claim in turn and must end with

    bike_step: <n> mutations, <n> detected
"""

from __future__ import annotations

import argparse
import collections
import csv
import glob
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import exe as E  # noqa: E402

OVL_BASE = 0x8005B5E8
FN_START = 0x80075EE0
FN_END = 0x80078DB4          # exclusive: jr ra at 0x80078DAC, delay slot 0x80078DB0
BIKE_STRIDE = 1096

R = E.REGNAMES
_IMAGES = None


def images():
    global _IMAGES
    if _IMAGES is None:
        _IMAGES = (E.load_overlay("RASHCDG.BIN", OVL_BASE), E.load_exe())
    return _IMAGES


def word(a):
    for img in images():
        if img.contains(a) and img.contains(a + 3):
            return img.word(a)
    return None


def load():
    return images()[0]


# --- the phase table (ours; read out of our own disassembly) ------------------------------------
# (start, end_exclusive, id, name)
PHASES = [
    (0x80075EE0, 0x80075F34, "P",  "prologue + pool-0 loop setup"),
    (0x80075F34, 0x80075F78, "A1", "pool-0 pre-pass: AI flag hygiene, commanded-speed reset"),
    (0x80075F78, 0x80076198, "A2", "pool-0 pre-pass: the aim-point blend (+0x370/+0x37C/+0x38C/+0x3B0)"),
    (0x80076198, 0x800761C8, "A3", "pool-0 pre-pass: list migration 0x80071BCC, loop advance"),
    (0x800761C8, 0x80076208, "B",  "steering driver + servo on two lists"),
    (0x80076208, 0x80076804, "C",  "list 0x8005B298 walk 1: control flags, gear/brake gates, grip terms"),
    (0x80076804, 0x80076820, "D",  "list 0x8005B298 walk 2: head"),
    (0x80076820, 0x8007686C, "D0", "walk 2: recovery countdown +0x2D0 (skips the rest)"),
    (0x8007686C, 0x80076AA4, "D1", "walk 2: longitudinal command decode (AI / analogue / none)"),
    (0x80076AA4, 0x80076C34, "D2", "walk 2: command -> throttle/brake + flagsA bits"),
    (0x80076C34, 0x800771C0, "D3", "walk 2: human only - 0x80074D58, crash timer, two-player draft"),
    (0x800771C0, 0x800774B0, "D4", "walk 2: throttle/brake slew into +0x24C/+0x254"),
    (0x800774B0, 0x800774C8, "D5", "walk 2: advance"),
    (0x800774C8, 0x800774E4, "E",  "list 0x8005B298 walk 3: head"),
    (0x800774E4, 0x800776AC, "E0", "walk 3: slope gate, clamps, grip terms +0x2B8/+0x2BC"),
    (0x800776AC, 0x80077BFC, "E1", "walk 3: lateral limit test -> impact latch (flagsB 0x400)"),
    (0x80077BFC, 0x80077E98, "E2", "walk 3: latched reset / brake+throttle states"),
    (0x80077E98, 0x80078174, "E3", "walk 3: wheelie/stoppie, stance trigger 0x800C4550"),
    (0x80078174, 0x80078460, "E4", "walk 3: acceleration +0x1E4 -> speed +0x240, slope clamp, pitch move"),
    (0x80078460, 0x80078478, "E5", "walk 3: advance"),
    (0x80078478, 0x80078AAC, "F",  "list 0x8005B298 walk 4: wheelie angle, lateral gain, steering servo"),
    (0x80078AAC, 0x80078B54, "G",  "pool-0 pass: position copy + 0x8007F0BC"),
    (0x80078B54, 0x80078C10, "H",  "road tracking, active list, ground frame, 0x8007FA4C"),
    (0x80078C10, 0x80078C58, "I",  "pool-0 pass: rider pose 0x800807F0"),
    (0x80078C58, 0x80078D84, "J",  "tail: 0x80093E6C, 0x800950E8, per-player odometer + EndRace"),
    (0x80078D84, 0x80078DB4, "X",  "epilogue"),
]


def phase_of(a):
    for p in PHASES:
        if p[0] <= a < p[1]:
            return p
    return None


def ptag(a):
    p = phase_of(a)
    return p[2] if p else "?"


# --- decoding ------------------------------------------------------------------------------------

def simm(w):
    return (w & 0xFFFF) - 0x10000 if w & 0x8000 else (w & 0xFFFF)


def branch_target(a, w):
    """(kind, target) for a control transfer at `a`, else None. kind in b, bal, j, jal, jr, jalr."""
    op = w >> 26
    if op in (0x04, 0x05, 0x06, 0x07):
        return "b", (a + 4 + (simm(w) << 2)) & 0xFFFFFFFF
    if op == 0x01:
        rt = (w >> 16) & 0x1F
        return ("bal" if rt in (0x10, 0x11) else "b"), (a + 4 + (simm(w) << 2)) & 0xFFFFFFFF
    if op == 0x02:
        return "j", 0x80000000 | ((w & 0x03FFFFFF) << 2)
    if op == 0x03:
        return "jal", 0x80000000 | ((w & 0x03FFFFFF) << 2)
    if op == 0 and (w & 0x3F) == 0x08:
        return "jr", None
    if op == 0 and (w & 0x3F) == 0x09:
        return "jalr", None
    return None


def is_unconditional(w):
    op = w >> 26
    if op == 0x02:
        return True
    if op == 0x04 and ((w >> 21) & 0x1F) == 0 and ((w >> 16) & 0x1F) == 0:
        return True
    return op == 0 and (w & 0x3F) == 0x08


LOADS = {0x20: ("lb", 1, "s"), 0x21: ("lh", 2, "s"), 0x23: ("lw", 4, "s"),
         0x24: ("lbu", 1, "u"), 0x25: ("lhu", 2, "u"), 0x22: ("lwl", 4, "?"),
         0x26: ("lwr", 4, "?"), 0x32: ("lwc2", 4, "?")}
STORES = {0x28: ("sb", 1), 0x29: ("sh", 2), 0x2B: ("sw", 4), 0x2A: ("swl", 4),
          0x2E: ("swr", 4), 0x3A: ("swc2", 4)}


def code_map():
    return {a: word(a) for a in range(FN_START, FN_END, 4)}


def compute_blocks():
    code = code_map()
    leaders = {FN_START}
    for a, w in code.items():
        bt = branch_target(a, w)
        if bt is None or bt[0] in ("jal", "jalr", "bal"):
            continue
        if bt[1] is not None and FN_START <= bt[1] < FN_END:
            leaders.add(bt[1])
        if a + 8 < FN_END:
            leaders.add(a + 8)
    starts = sorted(leaders)
    blocks = [(s, starts[i + 1] if i + 1 < len(starts) else FN_END) for i, s in enumerate(starts)]
    succ = {}
    for s, e in blocks:
        last = None
        for a in range(s, e, 4):
            bt = branch_target(a, code[a])
            if bt and bt[0] not in ("jal", "jalr", "bal"):
                last = (a, bt)
        out = []
        if last is None:
            out.append(e)
        else:
            a, (kind, t) = last
            if t is not None:
                out.append(t)
            if not is_unconditional(code[a]):
                out.append(a + 8)
        succ[s] = [x for x in out if FN_START <= x < FN_END]
    return blocks, succ, code


# --- pointer-provenance dataflow -----------------------------------------------------------------
#
# A value is None (unknown), ("C", n) a constant, ("P", kind, off) a pointer to object `kind` plus
# byte offset `off` (None = indexed), ("T", base) a constant table base plus an unknown index, or
# ("V", name) a named argument value (dt).

ENTITY_KINDS = ("E", "EP")          # the stepped bike; "a player's bike" read out of 0x8005B268
PTR_FIELDS = {0x22C: "ST", 0x354: "RID", 0x358: "PAS"}   # entity pointer fields
GLOBAL_PTRS = {
    0x800CE4D0: ("P", "E", 0),            # pool-0 base (population.md 1)
    0x8005B29C: ("P", "E", 0x440),        # list 0x8005B298 -> first node
    0x8005B2DC: ("P", "E", 0x440),
    0x8005B268: ("P", "EP", 0),
    0x8005B26C: ("P", "EP", 0),
    0x8005B21C: ("P", "EP", 0),
    0x8005B2F8: ("P", "GS", 0),           # -> game state (rules.md 1.1)
    0x800CE4DC: ("P", "POOLCNT", 0),
    0x1F800004: ("P", "E", 0),            # first entry of the scratchpad active list
    0x800CE4D4: ("S", "stride"),          # pool-0 stride (1096)
}
TABLE_PTRS = {0x8005B268: ("P", "EP", 0)}
# Seeds applied at block entry where the pointer is produced by a loop-carried induction the
# analysis does not model (scratchpad cursor, view cursor, per-player arrays).
SEEDS = {
    0x80078B90: {4: ("P", "E", 0)},                      # a0 = *cursor (scratchpad active list)
    0x80078BEC: {4: ("P", "E", 0)},
    0x80078C98: {16: ("P", "VIEW", 0), 18: ("T", 0x8005B380), 22: ("T", 0x8005B268)},
}
CALLER_SAVED = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25]


def looks_like_address(n):
    return 0x80000000 <= n < 0x80200000 or 0x1F800000 <= n < 0x1F800400


def vjoin(a, b):
    return a if a == b else None


def sjoin(s1, s2):
    regs = [vjoin(x, y) for x, y in zip(s1[0], s2[0])]
    slots = {k: s1[1][k] for k in s1[1] if k in s2[1] and s1[1][k] == s2[1][k]}
    return (regs, slots)


def deref_result(base, off, width, kind_load):
    """The value a `lw` produces from `base`+off."""
    if width != 4 or kind_load != "lw":
        return None
    if base is None:
        return None
    if base[0] == "C":
        a = (base[1] + off) & 0xFFFFFFFF
        if a in GLOBAL_PTRS:
            return GLOBAL_PTRS[a]
        return ("P", f"*{a:08X}", 0)
    if base[0] == "T":
        if base[1] in TABLE_PTRS and off == 0:
            return TABLE_PTRS[base[1]]
        return None
    if base[0] == "P" and base[2] is not None:
        f = base[2] + off
        if base[1] in ENTITY_KINDS:
            if f == 0x444:
                return ("P", "E", 0x440)
            if f in PTR_FIELDS:
                return ("P", PTR_FIELDS[f] + ("" if base[1] == "E" else "_" + base[1]), 0)
        return ("P", f"{base[1]}.{f:X}", 0)
    return None


class Access:
    __slots__ = ("addr", "rw", "mn", "width", "sign", "base", "off", "phase")

    def __init__(self, addr, rw, mn, width, sign, base, off):
        self.addr, self.rw, self.mn, self.width, self.sign = addr, rw, mn, width, sign
        self.base, self.off, self.phase = base, off, ptag(addr)


def transfer(block, state, code, record=None, calls=None):
    regs, slots = list(state[0]), dict(state[1])
    s, e = block
    pending_call = None
    for a in range(s, e, 4):
        w = code[a]
        op = w >> 26
        rs, rt, rd = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        fn = w & 0x3F
        newval = {}   # reg -> value
        m_load = LOADS.get(op)
        m_store = STORES.get(op)
        if m_load or m_store:
            base = regs[rs]
            off = simm(w)
            if rs == 29:
                if m_store:
                    slots[off] = regs[rt]
                else:
                    newval[rt] = slots.get(off) if m_load[0] == "lw" else None
            else:
                if record is not None:
                    if m_load:
                        record.append(Access(a, "R", m_load[0], m_load[1], m_load[2], base, off))
                    else:
                        record.append(Access(a, "W", m_store[0], m_store[1], "", base, off))
                if m_load:
                    newval[rt] = deref_result(base, off, m_load[1], m_load[0])
        elif op == 0x0F:                                   # lui
            newval[rt] = ("C", (w & 0xFFFF) << 16)
        elif op in (0x09, 0x08):                           # addiu / addi
            v = regs[rs]
            if v is None:
                newval[rt] = None
            elif v[0] == "C":
                newval[rt] = ("C", (v[1] + simm(w)) & 0xFFFFFFFF)
            elif v[0] == "P":
                newval[rt] = ("P", v[1], None if v[2] is None else v[2] + simm(w))
                if v[1] in ENTITY_KINDS and v[2] == 0x440 and simm(w) == -1088:
                    newval[rt] = ("P", v[1], 0)
            else:
                newval[rt] = None
        elif op == 0x0D:                                   # ori
            v = regs[rs]
            newval[rt] = ("C", v[1] | (w & 0xFFFF)) if v is not None and v[0] == "C" else None
        elif op in (0x0A, 0x0B, 0x0C, 0x0E):               # slti sltiu andi xori
            newval[rt] = None
        elif op == 0 and fn in (0x21, 0x20, 0x25):         # addu add or
            x, y = regs[rs], regs[rt]
            if rt == 0:
                newval[rd] = x
            elif rs == 0:
                newval[rd] = y
            elif fn == 0x25:
                newval[rd] = None
            else:
                v = None
                for p, q in ((x, y), (y, x)):
                    if p is not None and p[0] == "P" and q is not None and q[0] == "S":
                        v = p                      # pool cursor += stride keeps the kind
                    elif p is not None and p[0] == "C" and q is not None and q[0] == "C":
                        v = ("C", (p[1] + q[1]) & 0xFFFFFFFF)
                    elif p is not None and p[0] == "C" and q is None and looks_like_address(p[1]):
                        v = ("T", p[1])
                    elif p is not None and p[0] == "P" and q is None:
                        v = ("P", p[1], None)
                    elif p is not None and p[0] == "T" and q is None:
                        v = p
                    if v is not None:
                        break
                newval[rd] = v
        elif op == 0 and fn in (0x10, 0x12):               # mfhi mflo
            newval[rd] = None
        elif op == 0 and fn in (0x08, 0x09, 0x18, 0x19, 0x1A, 0x1B, 0x0C, 0x0D, 0x11, 0x13):
            pass                                           # jr jalr mult div syscall break mthi mtlo
        elif op == 0:
            newval[rd] = None
        elif op in (0x02, 0x04, 0x05, 0x06, 0x07, 0x01):
            pass
        elif op == 0x03:
            pass
        elif op == 0x12:                                   # cop2 moves
            if (w >> 21) & 31 in (0x00, 0x02):             # mfc2 cfc2
                newval[rt] = None
        else:
            newval[rt] = None
        bt = branch_target(a, w)
        for r, v in newval.items():
            if r != 0:
                regs[r] = v
        if pending_call is not None and a == pending_call[0] + 4:
            if calls is not None:
                calls.append((pending_call[0], pending_call[1], list(regs), dict(slots)))
            for r in CALLER_SAVED:
                regs[r] = None
            pending_call = None
        if bt and bt[0] in ("jal", "jalr", "bal"):
            pending_call = (a, bt[1] if bt[0] != "jalr" else ("jalr", R[rs]))
    return (regs, slots)


def dataflow():
    blocks, succ, code = compute_blocks()
    bmap = {s: (s, e) for s, e in blocks}
    init_regs = [None] * 32
    init_regs[0] = ("C", 0)
    init_regs[4] = ("V", "dt")
    init_regs[29] = ("V", "sp")
    entry = {FN_START: (init_regs, {})}
    work = [FN_START]
    while work:
        s = work.pop()
        st = entry[s]
        if s in SEEDS:
            regs = list(st[0])
            for r, v in SEEDS[s].items():
                regs[r] = v
            st = (regs, st[1])
        out = transfer(bmap[s], st, code)
        for t in succ[s]:
            if t not in entry:
                entry[t] = out
                work.append(t)
            else:
                j = sjoin(entry[t], out)
                if j != entry[t]:
                    entry[t] = j
                    work.append(t)
    # final pass: records
    acc, calls = [], []
    for s, e in blocks:
        if s not in entry:
            continue
        st = entry[s]
        if s in SEEDS:
            regs = list(st[0])
            for r, v in SEEDS[s].items():
                regs[r] = v
            st = (regs, st[1])
            entry[s] = st
        transfer((s, e), st, code, acc, calls)
    return blocks, succ, code, entry, acc, calls


def vstr(v):
    if v is None:
        return "?"
    if v[0] == "C":
        return f"0x{v[1]:08X}" if v[1] > 0xFFFF else str(v[1] if v[1] < 0x8000 else v[1] - 0x10000 if v[1] < 0x10000 else v[1])
    if v[0] == "V":
        return v[1]
    if v[0] == "T":
        return f"&0x{v[1]:08X}[i]"
    if v[0] == "S":
        return v[1]
    if v[0] == "P":
        if v[2] is None:
            return f"{v[1]}+[i]"
        return v[1] if v[2] == 0 else f"{v[1]}+0x{v[2]:X}"
    return str(v)


# --- callee analysis ---------------------------------------------------------------------------

def func_extent(entry, limit=20000):
    """[entry, end) by the usual heuristic: stop at a `jr ra` past every forward branch target."""
    far = entry
    a = entry
    jt = False
    while a < entry + 4 * limit:
        w = word(a)
        if w is None:
            return entry, a, jt
        bt = branch_target(a, w)
        if bt and bt[0] in ("b", "j") and bt[1] is not None and bt[1] > far:
            far = bt[1]
        if bt and bt[0] == "jr" and ((w >> 21) & 31) != 31:
            jt = True
        if w == 0x03E00008 and a >= far:
            return entry, a + 8, jt
        a += 4
    return entry, a, jt


def _uses_defs(w):
    op = w >> 26
    rs, rt, rd = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
    srcs, dst = [], None
    if op == 0:
        fn = w & 0x3F
        if fn in (0x00, 0x02, 0x03):             # shifts by sa read rt only
            srcs, dst = [rt], rd
        elif fn in (0x10, 0x12):                 # mfhi mflo
            dst = rd
        elif fn in (0x18, 0x19, 0x1A, 0x1B):     # mult div
            srcs = [rs, rt]
        elif fn in (0x08,):                      # jr
            srcs = [rs]
        elif fn == 0x09:                         # jalr
            srcs, dst = [rs], rd
        elif fn in (0x11, 0x13):                 # mthi mtlo
            srcs = [rs]
        else:
            srcs, dst = [rs, rt], rd
    elif op in LOADS:
        srcs, dst = [rs], rt
    elif op in STORES or op in (0x04, 0x05):
        srcs = [rs, rt]
    elif op in (0x06, 0x07, 0x01):
        srcs = [rs]
    elif op in (0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E):
        srcs, dst = [rs], rt
    elif op == 0x0F:
        dst = rt
    elif op == 0x12:
        sub = (w >> 21) & 31
        if sub in (0x00, 0x02):
            dst = rt
        elif sub in (0x04, 0x06):
            srcs = [rt]
    return {r for r in srcs if 4 <= r <= 7}, ({dst} if dst is not None and 4 <= dst <= 7 else set())


_LIVE = {}


def live_args(entry, depth=0):
    """a0..a3 live at the entry of the function at `entry`: a backward liveness pass over its own
    CFG. A `jal` counts as reading whatever its callee's entry has live (recursively, depth 6) and
    as clobbering a0..a3; a `jalr` as reading nothing and clobbering a0..a3."""
    if entry in _LIVE:
        return _LIVE[entry]
    _LIVE[entry] = set()                        # recursion guard
    s, e, _ = func_extent(entry)
    leaders = {s}
    for a in range(s, e, 4):
        bt = branch_target(a, word(a))
        if bt and bt[0] in ("b", "j"):
            if bt[1] is not None and s <= bt[1] < e:
                leaders.add(bt[1])
            leaders.add(a + 8)
        elif bt and bt[0] == "jr":
            leaders.add(a + 8)
    starts = sorted(x for x in leaders if s <= x < e)
    blocks = [(b, starts[i + 1] if i + 1 < len(starts) else e) for i, b in enumerate(starts)]
    succ = {}
    for b, be in blocks:
        last = None
        for a in range(b, be, 4):
            bt = branch_target(a, word(a))
            if bt and bt[0] in ("b", "j", "jr"):
                last = (a, bt)
        out = []
        if last is None:
            out.append(be)
        else:
            a, (k, t) = last
            if t is not None:
                out.append(t)
            if not is_unconditional(word(a)):
                out.append(a + 8)
        succ[b] = [x for x in out if s <= x < e]
    live_in = {b: set() for b, _ in blocks}
    changed = True
    while changed:
        changed = False
        for b, be in reversed(blocks):
            live = set()
            for t in succ[b]:
                live |= live_in[t]
            a = be - 4
            while a >= b:
                w = word(a)
                prev = word(a - 4) if a - 4 >= b else None
                pbt = branch_target(a - 4, prev) if prev is not None else None
                if pbt and pbt[0] in ("jal", "jalr"):
                    live -= {4, 5, 6, 7}
                    if pbt[0] == "jal" and depth < 6:
                        live |= live_args(pbt[1], depth + 1)
                    u, d = _uses_defs(w)            # the delay slot runs before the call
                    live = (live - d) | u
                    u, d = _uses_defs(prev)
                    live = (live - d) | u
                    a -= 8
                    continue
                u, d = _uses_defs(w)
                live = (live - d) | u
                a -= 4
            if live != live_in[b]:
                live_in[b] = live
                changed = True
    _LIVE[entry] = live_in[s]
    return live_in[s]


def callee_info(entry):
    s, e, jt = func_extent(entry)
    jals, jalr = [], 0
    for a in range(s, e, 4):
        bt = branch_target(a, word(a))
        if bt and bt[0] == "jal":
            jals.append(bt[1])
        if bt and bt[0] == "jalr":
            jalr += 1
    read = live_args(entry)
    return {"start": s, "end": e, "n": (e - s) // 4, "jals": jals, "jalr": jalr, "jt": jt,
            "args": sorted(R[r] for r in read)}


_ARITY = {}


def arity(t):
    if t not in _ARITY:
        info = callee_info(t)
        _ARITY[t] = max([int(x[1]) + 1 for x in info["args"]] or [0])
    return _ARITY[t]


def subtree(entry, seen=None, depth=0, maxdepth=8):
    if seen is None:
        seen = {}
    if entry in seen or depth > maxdepth:
        return seen
    info = callee_info(entry)
    seen[entry] = info
    for t in info["jals"]:
        subtree(t, seen, depth + 1, maxdepth)
    return seen


# The ported set is READ out of the bench's own row table (tools\\rrverify\\verify_physics.cpp:
# every `Row{"name", "signature", kConstant, ...}` with `kConstant` resolved through its
# `constexpr uint32_t` definition), so a row added or removed there changes this probe's answer.
BENCH_SRC = os.path.join(E.ROOT, "tools", "rrverify", "verify_physics.cpp")
# ... and the bike-step pass's own row file, which verify_physics.cpp includes. Its
# SPLICE rows name the whole step 0x80075EE0 as their entry but port one REGION of it, so they are
# not read as "0x80075EE0 is ported" (their signature says "spliced").
BENCH_INC = os.path.join(E.ROOT, "tools", "rrverify", "rows_bike_step.inc")
DIAGNOSTIC_ROWS = {"sound_emitter_is_invisible"}


def load_ported():
    src = ""
    for path in (BENCH_SRC, BENCH_INC):
        try:
            src += open(path, encoding="utf-8", errors="replace").read() + "\n"
        except OSError:
            if path == BENCH_SRC:
                return {}
    consts = {m.group(1): int(m.group(2), 16)
              for m in re.finditer(r"constexpr\s+uint32_t\s+(k\w+)\s*=\s*(0x[0-9A-Fa-f]+)", src)}
    out = {}
    pat = r'Row\s*\w*\s*\{\s*"(\w+)",\s*"((?:[^"\\]|\\.)*)",\s*(?:\w+::)*(k\w+|0x[0-9A-Fa-f]+)'
    for m in re.finditer(pat, src):
        name, sig, ref = m.group(1), m.group(2), m.group(3)
        if name in DIAGNOSTIC_ROWS or "spliced" in sig:
            continue
        addr = int(ref, 16) if ref.startswith("0x") else consts.get(ref)
        if addr is not None:
            out.setdefault(addr, name)
    return out


PORTED = load_ported()


# --- commands ----------------------------------------------------------------------------------

def header():
    g, s = images()
    print(f"RASHCDG.BIN sha1 {g.sha1}   SLUS_010.53 sha1 {s.sha1}")


def cmd_blocks(_a):
    header()
    blocks, succ, _ = compute_blocks()
    print(f"function 0x{FN_START:08X}..0x{FN_END:08X}  {(FN_END - FN_START) // 4} instructions"
          f"  {len(blocks)} blocks")
    per = collections.Counter()
    for s, e in blocks:
        per[ptag(s)] += 1
        print(f"{ptag(s):3s} {s:08X}..{e - 4:08X} {(e - s) // 4:4d}  -> "
              + " ".join(f"{x:08X}" for x in succ[s]))
    print("blocks per phase: " + " ".join(f"{k}={per[k]}" for k in [p[2] for p in PHASES]))


def cmd_calls(_a):
    header()
    _, _, _, _, _, calls = dataflow()
    for a, tgt, regs, slots in sorted(calls):
        if isinstance(tgt, tuple):
            print(f"{ptag(a):3s} {a:08X} JALR {tgt[1]}")
            continue
        st = PORTED.get(tgt, "")
        ar = arity(tgt)
        args = " ".join(f"a{i}={vstr(regs[4 + i])}" for i in range(ar)) or "(no register arguments)"
        if 16 in slots:
            args += f" [sp+16]={vstr(slots[16])}"
        print(f"{ptag(a):3s} {a:08X} jal 0x{tgt:08X} {st:18s} {args}")
    tally = collections.Counter(t for _, t, _, _ in calls)
    print(f"{len(calls)} call sites, {len(tally)} distinct callees, "
          f"{sum(1 for _, t, _, _ in calls if isinstance(t, tuple))} jalr")
    for t, n in sorted(tally.items()):
        print(f"   0x{t:08X} x{n:2d}  {PORTED.get(t, 'NOT PORTED')}")


def field_rows(acc):
    rows = collections.defaultdict(lambda: {"R": set(), "W": set(), "mn": set(), "ph": set(),
                                             "pc": []})
    glob = collections.defaultdict(lambda: {"R": 0, "W": 0, "mn": set(), "ph": set(), "pc": []})
    unres = []
    for x in acc:
        b = x.base
        if b is None:
            unres.append(x)
            continue
        if b[0] == "P":
            kind = b[1]
            if b[2] is None:
                key = (kind, f"[i]+0x{x.off & 0xFFFF:X}" if x.off >= 0 else f"[i]{x.off}")
            else:
                key = (kind, b[2] + x.off)
            r = rows[key]
        elif b[0] == "C":
            a = (b[1] + x.off) & 0xFFFFFFFF
            r = glob[("abs", a)]
            r[x.rw] += 1
            r["mn"].add(x.mn)
            r["ph"].add(x.phase)
            r["pc"].append(x.addr)
            continue
        elif b[0] == "T":
            r = glob[("tab", b[1], x.off)]
            r[x.rw] += 1
            r["mn"].add(x.mn)
            r["ph"].add(x.phase)
            r["pc"].append(x.addr)
            continue
        else:
            unres.append(x)
            continue
        r[x.rw].add(x.phase)
        r["mn"].add(x.mn)
        r["ph"].add(x.phase)
        r["pc"].append((x.addr, x.rw))
    return rows, glob, unres


def cmd_fields(args):
    header()
    _, _, _, _, acc, _ = dataflow()
    rows, _, unres = field_rows(acc)

    def korder(k):
        order = {"E": 0, "EP": 1, "ST": 2, "ST_EP": 3, "RID": 4, "RID_EP": 5, "PAS": 6}
        return (order.get(k[0], 9), k[0], k[1] if isinstance(k[1], int) else 1 << 20)

    if args.md:
        for k in sorted(rows, key=korder):
            r = rows[k]
            off = f"+0x{k[1]:03X}" if isinstance(k[1], int) else k[1]
            ws = [a for a, rw in r["pc"] if rw == "W"]
            rs = [a for a, rw in r["pc"] if rw == "R"]
            wtxt = " ".join(f"`{a:08X}`" for a in ws[:3]) + (f" +{len(ws) - 3}" if len(ws) > 3 else "")
            print(f"| {k[0]} | `{off}` | {'/'.join(sorted(r['mn']))} | {','.join(sorted(r['R'])) or '-'} "
                  f"| {','.join(sorted(r['W'])) or '-'} | {len(rs)}R {len(ws)}W | {wtxt or '-'} |")
        return
    print("kind  offset  mnem        R-phases          W-phases          sites")
    for k in sorted(rows, key=korder):
        r = rows[k]
        off = f"+0x{k[1]:03X}" if isinstance(k[1], int) else k[1]
        pcs = " ".join(f"{a:08X}{'w' if rw == 'W' else ''}" for a, rw in r["pc"][: (99 if args.all else 6)])
        more = "" if args.all or len(r["pc"]) <= 6 else f" (+{len(r['pc']) - 6})"
        print(f"{k[0]:6s}{off:9s} {'/'.join(sorted(r['mn'])):10s} "
              f"{','.join(sorted(r['R'])):17s} {','.join(sorted(r['W'])):17s} {pcs}{more}")
    print(f"{len(rows)} (object, offset) pairs;  {len(unres)} accesses through an unresolved base:")
    for x in unres:
        print(f"   {x.addr:08X} {x.phase:3s} {x.mn} {x.off}({R[(word(x.addr) >> 21) & 31]})")


def cmd_globals(_a):
    header()
    _, _, _, _, acc, _ = dataflow()
    _, glob, _ = field_rows(acc)
    for k in sorted(glob, key=lambda k: (k[0], k[1], k[2] if len(k) > 2 else 0)):
        r = glob[k]
        if k[0] == "abs":
            name = f"0x{k[1]:08X}"
        else:
            name = f"0x{k[1]:08X}[i]{'+' if k[2] >= 0 else ''}{k[2]}"
        print(f"{name:26s} R{r['R']:<3d} W{r['W']:<3d} {'/'.join(sorted(r['mn'])):9s} "
              f"{','.join(sorted(r['ph'])):12s} " + " ".join(f"{a:08X}" for a in r["pc"][:8]))


def cmd_callee(_a):
    header()
    _, _, _, _, _, calls = dataflow()
    targets = sorted({t for _, t, _, _ in calls if not isinstance(t, tuple)})
    for t in targets:
        info = callee_info(t)
        sub = subtree(t)
        un = [k for k in sub if k not in PORTED and k != t]
        un_n = sum(sub[k]["n"] for k in un)
        print(f"0x{t:08X} {PORTED.get(t, 'NOT PORTED'):18s} {info['n']:5d} insns  args {','.join(info['args']) or '-':12s}"
              f" jalr {info['jalr']} jumptable {'yes' if info['jt'] else 'no'}")
        print(f"      callees: " + " ".join(f"{c:08X}{'' if c not in PORTED else '*'}" for c in sorted(set(info['jals']))))
        if t not in PORTED:
            print(f"      unported subtree below it: {len(un)} functions, {un_n} insns"
                  f"{' (depth-limited)' if len(sub) > 60 else ''}")


def cmd_probes(args):
    blocks, _, _ = compute_blocks()
    text = " ".join(f"--probe 0x{s:08X}:b{s:08X}" for s, _ in blocks)
    if args.out:
        with open(args.out, "w") as f:
            f.write(text + "\n")
        print(f"wrote {len(blocks)} probe arguments to {args.out}")
    else:
        print(text)


def entity_reg_at(entry_state):
    """The register (index, delta) whose value is the stepped bike at a block entry."""
    regs = entry_state[0]
    for r in (17, 16, 7, 4, 5, 18, 19, 20, 21, 22, 23, 30, 10):
        v = regs[r]
        if v is not None and v[0] == "P" and v[1] == "E" and v[2] is not None:
            return r, v[2]
    return None


def expand_csv(specs):
    """Each spec may be a probes.csv, a directory (searched recursively for probes.csv) or a glob
    of either; anything else a glob matches (logs, text reports) is ignored. Sorted, de-duplicated."""
    out = []
    for spec in specs:
        cands = glob.glob(spec) if any(ch in spec for ch in "*?[") else [spec]
        for c in cands:
            if os.path.isdir(c):
                out += glob.glob(os.path.join(c, "**", "probes.csv"), recursive=True)
            elif os.path.isfile(c) and c.lower().endswith(".csv"):
                out.append(c)
    return sorted(set(os.path.normpath(x) for x in out))


_COV = {}


def coverage_data(paths):
    key = tuple(paths)
    if key in _COV:
        return _COV[key]
    blocks, _, code, entry, _, _ = dataflow()
    bsize = {s: (e - s) // 4 for s, e in blocks}
    # The bike register is meaningful only inside a per-bike loop body: not in the prologue, the
    # steering calls, the tail or at a walk's exit/advance block (where the cursor is one past).
    no_bike = {"P", "B", "J", "X", "D5", "E5"}
    h_loop = {0x80078B90, 0x80078BA4, 0x80078BA8, 0x80078BEC}
    ereg = {}
    for s, _ in blocks:
        t = ptag(s)
        if s not in entry or t in no_bike or (t == "H" and s not in h_loop):
            ereg[s] = None
        else:
            ereg[s] = entity_reg_at(entry[s])
    base = 0x801B65D4
    hits = collections.Counter()
    slots = collections.defaultdict(set)
    for path in paths:
        with open(path, newline="") as f:
            for row in csv.DictReader(f):
                pc = int(row["pc"], 16)
                if not (FN_START <= pc < FN_END):
                    continue
                hits[pc] += 1
                er = ereg.get(pc)
                if er is not None:
                    v = int(row[R[er[0]]], 16) - er[1]
                    k, rem = divmod(v - base, BIKE_STRIDE)
                    if rem == 0 and 0 <= k < 18:
                        slots[pc].add(k)
    ran = [s for s, _ in blocks if hits[s]]
    res = (blocks, bsize, hits, slots, len(ran), sum(bsize[s] for s in ran))
    _COV[key] = res
    return res


def cmd_coverage(args):
    header()
    paths = expand_csv(args.csv)
    if not paths:
        print("no probes.csv matched " + " ".join(args.csv))
        return 1
    print(f"{len(paths)} probes.csv: " + " ".join(os.path.basename(os.path.dirname(x)) for x in paths))
    blocks, bsize, hits, slots, _, _ = coverage_data(paths)
    ran = [s for s, _ in blocks if hits[s]]
    ins_ran = sum(bsize[s] for s in ran)
    print(f"blocks executed: {len(ran)} of {len(blocks)}   instructions covered (static): "
          f"{ins_ran} of {(FN_END - FN_START) // 4}")
    print("phase  blocks  ran   insns  ran-insns  slots seen")
    for p in PHASES:
        bs = [s for s, _ in blocks if p[0] <= s < p[1]]
        r = [s for s in bs if hits[s]]
        sl = set()
        for s in r:
            sl |= slots[s]
        sls = ",".join(str(x) for x in sorted(sl, key=lambda x: (isinstance(x, str), x)))
        print(f"{p[2]:5s} {len(bs):6d} {len(r):5d} {sum(bsize[s] for s in bs):7d} "
              f"{sum(bsize[s] for s in r):9d}  {sls}")
    if args.detail:
        for s, e in blocks:
            sl = ",".join(str(x) for x in sorted(slots[s], key=lambda x: (isinstance(x, str), x)))
            print(f"  {ptag(s):3s} {s:08X} {bsize[s]:3d} {'RAN ' if hits[s] else 'never'} {hits[s]:6d}  {sl}")
    else:
        print("never-executed blocks (start:size):")
        line = []
        for s, e in blocks:
            if not hits[s]:
                line.append(f"{s:08X}:{bsize[s]}")
        for i in range(0, len(line), 8):
            print("   " + " ".join(line[i:i + 8]))


# Proposed cut points: each region is entered at `entry` and left at `exit`, both
# on the function's own control flow; the sub-row runs exactly the guest instructions in between.
CUTS = [
    ("A", 0x80075F18, 0x800761C8), ("B", 0x800761C8, 0x80076208), ("C", 0x80076208, 0x80076804),
    ("D", 0x80076804, 0x800774C8), ("E", 0x800774C8, 0x80078478), ("F", 0x80078478, 0x80078AAC),
    ("G", 0x80078AAC, 0x80078B54), ("H", 0x80078B54, 0x80078C10), ("I", 0x80078C10, 0x80078C58),
    ("J", 0x80078C58, 0x80078D84),
]


def full_uses_defs(w):
    """All GPR uses/defs of one instruction, plus the sp-relative slot it loads or stores."""
    op = w >> 26
    rs, rt, rd = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
    uses, defs, suse, sdef = set(), set(), set(), set()
    if op == 0:
        fn = w & 0x3F
        if fn in (0x00, 0x02, 0x03):
            uses, defs = {rt}, {rd}
        elif fn in (0x04, 0x06, 0x07):
            uses, defs = {rs, rt}, {rd}
        elif fn in (0x10, 0x12):
            defs = {rd}
        elif fn in (0x18, 0x19, 0x1A, 0x1B):
            uses = {rs, rt}
        elif fn == 0x08:
            uses = {rs}
        elif fn == 0x09:
            uses, defs = {rs}, {rd}
        elif fn in (0x11, 0x13):
            uses = {rs}
        elif fn in (0x0C, 0x0D):
            pass
        else:
            uses, defs = {rs, rt}, {rd}
    elif op in LOADS:
        uses, defs = {rs}, {rt}
        if rs == 29:
            suse = {simm(w)}
    elif op in STORES:
        uses = {rs, rt}
        if rs == 29:
            sdef = {simm(w)}
    elif op in (0x04, 0x05):
        uses = {rs, rt}
    elif op in (0x06, 0x07, 0x01):
        uses = {rs}
    elif op in (0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E):
        uses, defs = {rs}, {rt}
    elif op == 0x0F:
        defs = {rt}
    elif op == 0x03:
        defs = {31}
    uses.discard(0)
    defs.discard(0)
    return uses, defs, suse, sdef


def function_liveness():
    """Live GPRs and live sp slots at every instruction of the function (backward, whole CFG).
    A `jal` uses its callee's live argument registers, defines every caller-saved register and
    preserves s0..s8/sp/gp (o32)."""
    blocks, succ, code = compute_blocks()
    live_in = {b: (frozenset(), frozenset()) for b, _ in blocks}
    at = {}
    changed = True
    exit_live = frozenset({2, 3, 16, 17, 18, 19, 20, 21, 22, 23, 28, 29, 30, 31})
    while changed:
        changed = False
        for b, be in reversed(blocks):
            regs, slots = set(), set()
            if not succ[b] and be == FN_END:
                regs |= exit_live
            for t in succ[b]:
                regs |= live_in[t][0]
                slots |= live_in[t][1]
            a = be - 4
            while a >= b:
                w = code[a]
                prev = code.get(a - 4) if a - 4 >= b else None
                pbt = branch_target(a - 4, prev) if prev is not None else None
                if pbt and pbt[0] in ("jal", "jalr"):
                    regs -= set(CALLER_SAVED) | {31}
                    if pbt[0] == "jal":
                        regs |= live_args(pbt[1])
                    for x in (a, a - 4):
                        u, d, su, sd = full_uses_defs(code[x])
                        regs = (regs - d) | u
                        slots = (slots - sd) | su
                        at[x] = (frozenset(regs), frozenset(slots))
                    a -= 8
                    continue
                u, d, su, sd = full_uses_defs(w)
                regs = (regs - d) | u
                slots = (slots - sd) | su
                at[a] = (frozenset(regs), frozenset(slots))
                a -= 4
            new = (frozenset(regs), frozenset(slots))
            if new != live_in[b]:
                live_in[b] = new
                changed = True
    return at


def cut_edges_ok(s, e):
    """Every control transfer into [s, e) from outside lands on s, every one out of it lands on e,
    and the instruction executed just before s (on every path) is not a load (no pending load
    delay at the hook). Returns a list of violations."""
    blocks, succ, code = compute_blocks()
    bad = []
    inside = lambda a: s <= a < e
    # instruction-level edges: fall-through inside blocks and block successor edges
    edges = []
    for b, be in blocks:
        for a in range(b, be - 4, 4):
            edges.append((a, a + 4))
        for t in succ[b]:
            edges.append((be - 4, t))
    for src, dst in edges:
        if not inside(src) and inside(dst) and dst != s:
            bad.append(f"enters at {dst:08X} from {src:08X}")
        if inside(src) and not inside(dst) and dst != e:
            bad.append(f"leaves to {dst:08X} from {src:08X}")
        if dst == s:
            w = code[src]
            if (w >> 26) in LOADS:
                bad.append(f"load at {src:08X} right before the entry")
    return bad


def cmd_cuts(_a):
    header()
    at = function_liveness()
    for name, s, e in CUTS:
        bad = cut_edges_ok(s, e)
        print(f"region {name}: {'single entry / single exit, no pending load' if not bad else '; '.join(bad)}")
    print("region  entry     exit      insns  live-in registers            live-in sp slots")
    for name, s, e in CUTS:
        regs, slots = at[s]
        print(f"{name:6s}  {s:08X}  {e:08X}  {(e - s) // 4:5d}  {','.join(R[r] for r in sorted(regs)):28s} "
              f"{','.join(f'sp+{k}' for k in sorted(slots))}")


# --- the step's remaining callees; the stat block entity+0x22C -----------------------------------

_CDI = None
_JALS = None


def rashcdi():
    """The race LOADER overlay. It shares its base with RASHCDG, so its words are read through this
    image only - never through word(), which resolves an address in RASHCDG first."""
    global _CDI
    if _CDI is None:
        _CDI = E.load_overlay("RASHCDI.BIN", OVL_BASE)
    return _CDI


def jal_index():
    """{target: [(tag, site)]} over RASHCDG (G), SLUS (S) and RASHCDI (I). A SLUS call into the
    overlay window lands in whichever overlay is resident; a RASHCDI call lands in RASHCDI."""
    global _JALS
    if _JALS is None:
        _JALS = collections.defaultdict(list)
        for tag, img in (("G", images()[0]), ("S", images()[1]), ("I", rashcdi())):
            for a, w in img.words():
                if (w >> 26) == 0x03:
                    _JALS[0x80000000 | ((w & 0x03FFFFFF) << 2)].append((tag, a))
    return _JALS


def race_callers(target):
    """Call sites that can reach a RASHCDG function during a race: RASHCDG and SLUS."""
    return sorted((t, a) for t, a in jal_index().get(target, []) if t in ("G", "S"))


def lui_ori(img, a_lui, a_ori):
    """The 32-bit constant a `lui rt,hi` at a_lui and an `ori rt,rt,lo` at a_ori form, else None."""
    u, o = img.word(a_lui), img.word(a_ori)
    if (u >> 26) != 0x0F or (o >> 26) != 0x0D:
        return None
    if ((u >> 16) & 31) != ((o >> 21) & 31):
        return None
    return ((u & 0xFFFF) << 16) | (o & 0xFFFF)


def imm_of(img, a, op):
    """The 16-bit immediate of the instruction at `a` if its opcode is `op`, else None."""
    w = img.word(a)
    return (w & 0xFFFF) if (w >> 26) == op else None


def func_of(a, entries):
    """Which of `entries` (function entry points) has `a` inside its extent."""
    for f in entries:
        s, e, _ = func_extent(f)
        if s <= a < e:
            return f
    return None


def reg_readers(fn, reg):
    """Instructions inside `fn` that read GPR `reg`, apart from the prologue save `sw reg,k(sp)`."""
    s, e, _ = func_extent(fn)
    out = []
    for a in range(s, e, 4):
        w = word(a)
        u, _, _, _ = full_uses_defs(w)
        if reg not in u:
            continue
        if (w >> 26) == 0x2B and ((w >> 21) & 31) == 29 and ((w >> 16) & 31) == reg:
            continue
        out.append(a)
    return out


def rw32(r, a):
    return struct.unpack_from("<I", r, a & 0x1FFFFF)[0]


def rs16(r, a):
    return struct.unpack_from("<h", r, a & 0x1FFFFF)[0]


RAM_GLOBS = [os.path.join(E.ROOT, "work", "oracle", "state", "*", "ram.bin"),
             os.path.join(E.ROOT, "work", "oracle", "vr_capture", "ramdumps", "*.bin")]
_RACE_IMAGES = None


def race_images():
    """Every RAM image with a sane pool 0 (the bench's 18: four savestates, 14 VR dumps).
    `retro-shell` (the frontend) has no pool 0 and drops out on the stride test."""
    global _RACE_IMAGES
    if _RACE_IMAGES is None:
        _RACE_IMAGES = []
        for g in RAM_GLOBS:
            for p in sorted(glob.glob(g)):
                with open(p, "rb") as f:
                    r = f.read()
                if len(r) < 0x200000 or rw32(r, 0x800CE4D4) != BIKE_STRIDE:
                    continue
                cp = rw32(r, 0x800CE4DC)
                if not 0x80000000 <= cp < 0x80200000 or not 0 <= rw32(r, cp) < 24:
                    continue
                name = os.path.basename(os.path.dirname(p)) if p.endswith("ram.bin") else \
                    os.path.splitext(os.path.basename(p))[0]
                _RACE_IMAGES.append((name, r))
    return _RACE_IMAGES


def pool0(r):
    base = rw32(r, 0x800CE4D0)
    return [base + i * BIKE_STRIDE for i in range(rw32(r, rw32(r, 0x800CE4DC)) + 1)]


LIST_HEADS = (0x8005B270, 0x8005B298, 0x8005B2D8, 0x8005B350, 0x8005B378)


def list_members(r):
    """{entity: head} by walking the five circular lists through their `next` words (+4)."""
    out = {}
    for h in LIST_HEADS:
        node, k = rw32(r, h + 4), 0
        while node != h and k < 64:
            out[node - 1088] = h
            node, k = rw32(r, node + 4), k + 1
    return out


def migrate_head(e140, flags_c):
    """The head 0x80071BCC links a bike into, from (s16)+0x140 and flagsC."""
    if e140 == 0:
        return 0x8005B270
    if flags_c & 0x600:
        return 0x8005B2D8 if flags_c & 0x400 else 0x8005B378
    if flags_c & 0x1FF:
        return 0x8005B350
    return 0x8005B298


def migrate_census():
    """(images, bike states, states on exactly the list the rule predicts, states with a
    migration pending, i.e. flagsC & 0x08001800)."""
    imgs = race_images()
    n = ok = pend = 0
    bad = []
    for name, r in imgs:
        mem = list_members(r)
        for i, e in enumerate(pool0(r)):
            fc = rw32(r, e + 0x238)
            n += 1
            pend += 1 if fc & 0x08001800 else 0
            if mem.get(e) == migrate_head(rs16(r, e + 0x140), fc):
                ok += 1
            else:
                bad.append((name, i))
    return (len(imgs), n, ok, pend), bad


DATA_DIR = os.path.join(E.DISC, "DATA")


def data_file(name):
    try:
        with open(os.path.join(DATA_DIR, name), "rb") as f:
            return f.read()
    except OSError:
        return None


STAT_BLOCK = 448          # RASHCDI 0x800655A8 `li a2,448`; 0x8006619C..A8 `(s4*8 - s4) << 6`


def stats_census(source):
    """Over every race image: the stat pointer of every live bike against
    *(0x8005B248) + 448 * idx (idx = 3 / 4 for player 1 / 2, else the rider record's class nibble
    riderDef[+1] & 0xF), and the bytes of each block against the file slice `source` names:
    {block: (file, offset)}, with "LEVEL" meaning LEVEL<game_state[+0x3C] + 1>.PH.
    Returns (images, bikes, pointers that match, blocks that match, blocks checked)."""
    imgs = race_images()
    nb = pok = bok = bn = 0
    notes = []
    for name, r in imgs:
        gs = rw32(r, 0x8005B2F8)
        bank = rw32(r, gs + 0x3C)
        base = rw32(r, 0x8005B248)
        p1, p2 = rw32(r, 0x8005B268), rw32(r, 0x8005B26C)
        for e in pool0(r):
            rd = rw32(r, e + 0x43C)
            idx = 3 if e == p1 else 4 if (p2 and e == p2) else (r[(rd + 1) & 0x1FFFFF] & 0xF)
            nb += 1
            if rw32(r, e + 0x22C) == base + STAT_BLOCK * idx:
                pok += 1
            else:
                notes.append(f"{name}: bike {e:08X} +0x22C {rw32(r, e + 0x22C):08X}, idx {idx}")
        for k, (fname, off) in sorted(source.items()):
            if k >= 4 and rw32(r, gs + 0x30) < 2:
                continue
            bn += 1
            data = data_file(f"LEVEL{bank + 1}.PH" if fname == "LEVEL" else fname)
            o = (base + STAT_BLOCK * k) & 0x1FFFFF
            if data is not None and r[o:o + STAT_BLOCK] == data[off:off + STAT_BLOCK]:
                bok += 1
            else:
                notes.append(f"{name}: block {k} != {fname}@0x{off:X}")
    return (len(imgs), nb, pok, bok, bn), notes


def stat_writers():
    """Every store whose base register was loaded from +0x22C (`lw rX,556(rY)`), scanning forward
    from the load until rX is redefined or 40 instructions pass: {(tag, pc): offset}."""
    out = {}
    imgs = (("G", images()[0]), ("S", images()[1]), ("I", rashcdi()),
            ("F", E.load_overlay("RASHCDF.BIN", OVL_BASE)))
    for tag, img in imgs:
        words = list(img.words())
        for i, (a, w) in enumerate(words):
            if (w >> 26) != 0x23 or (w & 0xFFFF) != 0x22C:
                continue
            rx = (w >> 16) & 31
            for a2, w2 in words[i + 1:i + 41]:
                if (w2 >> 26) in STORES and ((w2 >> 21) & 31) == rx:
                    out[(tag, a2)] = simm(w2)
                _, d, _, _ = full_uses_defs(w2)
                if rx in d:
                    break
    return out


P1_RUNS = os.path.join(E.ROOT, "work", "bike_step", "p1")


def p1_probe_hits():
    """[{pc: hits}] per part-1 trace run (the `b_*` runs are the follow-up set and are read
    separately by `remaining`)."""
    paths = sorted(glob.glob(os.path.join(P1_RUNS, "*_0x*", "probes.csv")))
    paths = [p for p in paths if not os.path.basename(os.path.dirname(p)).startswith("b_")]
    runs = []
    for p in paths:
        n = os.path.basename(os.path.dirname(p))
        hits = {}                       # from the run's log: it lists every probe, zero hits too
        with open(os.path.join(P1_RUNS, n + ".log"), errors="replace") as f:
            for line in f:
                m = re.match(r"\s+0x([0-9A-F]{8}) \S+\s+hits (\d+)", line)
                if m:
                    hits[int(m.group(1), 16)] = int(m.group(2))
        runs.append((n, hits))
    return runs


def p1_watch_writes(prefix=""):
    """(runs, reads, writes) of the watches in the part-1 trace runs: prefix "" = the stat-array
    runs, "w_" = the runs watching every bike's +0x22C word."""
    paths = sorted(p for p in glob.glob(os.path.join(P1_RUNS, prefix + "*_0x*", "watch.csv"))
                   if os.path.basename(os.path.dirname(p))[:2] not in ("b_", "w_") or prefix)
    rd = wr = 0
    for p in paths:
        with open(p, newline="") as f:
            for row in csv.DictReader(f):
                if row["kind"].lower().startswith("r"):
                    rd += 1
                else:
                    wr += 1
    return len(paths), rd, wr


P1_FUNCS = [0x80071BCC, 0x8007F08C, 0x80072994, 0x80074D58, 0x800C4550, 0x80093E6C, 0x80093ED4,
            0x80093F94, 0x800950E8, 0x800951B8, 0x8007AC04, 0x8008CF74, 0x80090D84, 0x8002ED94]


def cmd_remaining(_a):
    header()
    print(f"RASHCDI.BIN sha1 {rashcdi().sha1}")
    for f in P1_FUNCS:
        info = callee_info(f)
        st = PORTED.get(f, "NOT PORTED")
        print(f"0x{f:08X} {st:20s} {info['n']:4d} insns  args {','.join(info['args']) or '-':12s}"
              f" jalr {info['jalr']} jumptable {'yes' if info['jt'] else 'no'}")
        for t, n in sorted(collections.Counter(info["jals"]).items()):
            ci = callee_info(t)
            print(f"      -> 0x{t:08X} x{n}  {PORTED.get(t, 'NOT PORTED'):20s} {ci['n']:4d} insns "
                  f"arity {arity(t)}")
        cs = race_callers(f)
        print("      called from: " + (" ".join(f"{'SLUS ' if t == 'S' else ''}{a:08X}" for t, a in cs) or "-"))
    (ni, n, ok, pend), bad = migrate_census()
    print(f"list census: {ni} images, {n} bike states, {ok} on the list 0x80071BCC's rule names, "
          f"{pend} with a migration pending" + (f"; off: {bad[:6]}" if bad else ""))
    for sub in ("", "b_"):
        paths = sorted(p for p in glob.glob(os.path.join(P1_RUNS, sub + "*_0x*", "probes.csv"))
                       if os.path.basename(os.path.dirname(p)).startswith("b_") == (sub == "b_"))
        if not paths:
            continue
        names = [os.path.basename(os.path.dirname(p)) for p in paths]
        table = collections.defaultdict(dict)
        for n, p in zip(names, paths):
            with open(p, newline="") as f:
                for row in csv.DictReader(f):
                    table[int(row["pc"], 16)][n] = table[int(row["pc"], 16)].get(n, 0) + 1
            with open(os.path.join(P1_RUNS, n + ".log"), errors="replace") as f:
                for line in f:           # probes with zero hits are only in the log
                    m = re.match(r"\s+0x([0-9A-F]{8}) \S+\s+hits (\d+)", line)
                    if m:
                        table[int(m.group(1), 16)].setdefault(n, int(m.group(2)))
        print("trace runs: " + "  ".join(names))
        for a in sorted(table):
            print(f"   0x{a:08X} " + " ".join(f"{table[a].get(n, 0):6d}" for n in names))


def cmd_stats(_a):
    header()
    cdi = rashcdi()
    print(f"RASHCDI.BIN sha1 {cdi.sha1}")
    c = claims()["stats_loader"]
    fmt_at = addiu_target_img(cdi, c["level_ph_ref"][0])
    s = cdi.data[cdi.off(fmt_at):cdi.off(fmt_at) + 16].split(b"\0")[0]
    print(f"LEVEL<n>.ph loader 0x800654BC: name {s!r} (0x{fmt_at:08X}), n-1 = game_state[+0x{imm_of(cdi, 0x800654E8, 0x23):X}], "
          f"read {imm_of(cdi, 0x80065530, 0x09)} bytes")
    print(f"bike .PH loader 0x80065558: read {imm_of(cdi, 0x800655A8, 0x09)} bytes")
    print(f"block count: {imm_of(cdi, 0x80067BC0, 0x09)} (two players) / {imm_of(cdi, 0x80067BC4, 0x09)} (one), "
          f"base pointer 0x{lw_target_img(cdi, 0x800661A4):08X}")
    (ni, nb, pok, bok, bn), notes = stats_census(claims()["stats_source"])
    print(f"census: {ni} images, {nb} bikes, {pok} stat pointers = base + 448*idx, "
          f"{bok} of {bn} blocks byte-equal to their file slice")
    for x in notes[:12]:
        print("   " + x)
    for name, r in race_images():
        gs = rw32(r, 0x8005B2F8)
        base = rw32(r, 0x8005B248)
        per = collections.Counter((rw32(r, e + 0x22C) - base) // STAT_BLOCK for e in pool0(r))
        print(f"   {name:16s} bank {rw32(r, gs + 0x3C)} players {rw32(r, gs + 0x30)} base 0x{base:08X} "
              f"loaded-mask 0x{rw32(r, 0x8005AD40):08X} bikes per block {dict(sorted(per.items()))}")
    print("stores through a +0x22C pointer (all four images):")
    for (t, a), off in sorted(stat_writers().items()):
        print(f"   {({'G': 'RASHCDG', 'S': 'SLUS', 'I': 'RASHCDI', 'F': 'RASHCDF'})[t]} 0x{a:08X} -> stats+0x{off:X}")
    runs, rd, wr = p1_watch_writes()
    if runs:
        print(f"live: {runs} trace runs watching the stat array: {rd} reads, {wr} writes")
    runs, rd, wr = p1_watch_writes("w_")
    if runs:
        print(f"live: {runs} trace runs watching every bike's +0x22C word: {rd} reads, {wr} writes")


def addiu_target_img(img, at):
    w = img.word(at)
    rs = (w >> 21) & 31
    for back in range(1, 12):
        u = img.word(at - 4 * back)
        if (u >> 26) == 0x0F and ((u >> 16) & 31) == rs:
            return (((u & 0xFFFF) << 16) + simm(w)) & 0xFFFFFFFF
    return None


lw_target_img = addiu_target_img      # `lui rX,hi` + `lw rY,lo(rX)` forms the address the same way


# The claims this probe checks, in one place, so that --mutate can perturb each one in turn and
# show that the check guarding it fails.
CLEAN_RUNS = os.path.join(E.ROOT, "work", "bike_step", "cov_*")


def claims():
    return {
        "sha1": "cfe43a7786759f2cb9c57751cf99e84d1074782c",
        "insns": 2997,
        "blocks": 570,
        "jr_ra": [0x80078DAC],
        "jalr": 0,
        "calls": 95,
        "fixmul_fixdiv": (38, 31),
        # every non-leaf call site and its callee
        "sites": {
            0x800760FC: 0x8002E548, 0x800761B0: 0x80071BCC, 0x800761D4: 0x80073874,
            0x800761E8: 0x80073874, 0x800761F4: 0x80074C84, 0x80076200: 0x80074C84,
            0x80076B48: 0x8007EF60, 0x80076C58: 0x80074D58, 0x80076C64: 0x80074E6C,
            0x80076E28: 0x8002E698, 0x80077514: 0x8002E698, 0x8007794C: 0x80020018,
            0x80077D1C: 0x8002EAD8, 0x80078058: 0x800C4550, 0x80078330: 0x80074FB4,
            0x80078B3C: 0x8007F0BC, 0x80078B54: 0x80037338, 0x80078B5C: 0x8003E150,
            0x80078B64: 0x8003AE24, 0x80078B6C: 0x8003B520, 0x80078BA8: 0x8007504C,
            0x80078BF0: 0x8007FA4C, 0x80078C40: 0x800807F0, 0x80078C58: 0x80093E6C,
            0x80078C60: 0x800950E8, 0x80078D38: 0x80092C7C,
        },
        # register arguments each callee really takes
        "arity": {0x8007FA4C: 1, 0x80074D58: 1, 0x80071BCC: 2, 0x80074C84: 2, 0x8007EF60: 4,
                  0x800C4550: 3, 0x8007F0BC: 2, 0x80037338: 0, 0x8003E150: 0, 0x8003AE24: 0,
                  0x8003B520: 0, 0x80093E6C: 0, 0x800950E8: 0,
                  # the remaining callees
                  0x80072994: 1, 0x8007F08C: 1, 0x80090D84: 1, 0x80093ED4: 2, 0x80093F94: 2,
                  0x800951B8: 2, 0x8007AC04: 1, 0x8008CF74: 1},
        # status against the bench's row table: the 16 ported callees of the step, and among the
        # remaining callees the ones already benched
        "ported": {0x8001FC90, 0x80010028, 0x80020018, 0x8002E548, 0x8002E698, 0x8002EAD8,
                   0x80074FB4, 0x80073874, 0x80074E6C, 0x8007504C, 0x800807F0, 0x80092C7C,
                   0x80074C84, 0x8007EF60, 0x8007F0BC, 0x8007FA4C,
                   0x80071D24, 0x800723FC, 0x8002EB78, 0x8002E468, 0x8004D2A4, 0x800BCA68,
                   0x800BCD10,
                   # rows_bike_step.inc: the rider pass's heading writer
                   0x8007AC04, 0x8008CF74,
                   # rows_bike_step.inc: the list migration, its leaf, the
                   # wipeout end with its row rotation, and the passenger launch
                   0x80071BCC, 0x8007F08C, 0x80072994, 0x8002ED94, 0x80074D58},
        # the unported callees of the step (as this probe reads the bench: rows_bike_step.inc and
        # verify_physics.cpp only), and the unported functions under the remaining callees
        "unported": {0x800C4550, 0x80037338, 0x8003E150, 0x8003AE24,
                     0x8003B520, 0x80093E6C, 0x800950E8,
                     0x80090D84, 0x80091468, 0x800C3E9C,
                     0x800C4500, 0x800C4454, 0x800C2FF4, 0x80093ED4, 0x80093F94, 0x800951B8,
                     0x80039F68},
        # the jal targets of each function read there (sets, from the words)
        "p1_callees": {
            0x80071BCC: {0x80071D24, 0x800723FC, 0x80072994, 0x8007F08C},
            0x8007F08C: set(),
            0x80072994: {0x8001FC90, 0x8002EAD8, 0x8002ED94, 0x80090D84, 0x80091468},
            0x80074D58: {0x80090D84},
            0x800C4550: {0x800C3E9C, 0x800C4500, 0x800C4454, 0x800C2FF4},
            0x80093E6C: {0x80093ED4},
            0x80093ED4: {0x80039F68, 0x80093F94},
            0x80093F94: {0x80093FE4, 0x8009432C},
            0x800950E8: {0x800951B8},
            0x800951B8: {0x80039F68, 0x800952AC, 0x80099D48},
            0x8007AC04: {0x80010028, 0x8001FC90, 0x8002E468, 0x8002EAD8, 0x8002EB78, 0x8004D2A4,
                         0x8008CF74},
            0x8008CF74: set(),
        },
        # every call site that can run during a race (RASHCDG "G", SLUS "S")
        "callers": {
            0x80071BCC: [("G", 0x800761B0), ("G", 0x8007B8E4), ("G", 0x8007DD28)],
            0x80072994: [("G", 0x80071C3C), ("G", 0x80071C9C), ("G", 0x8007BCD0)],
            0x8007F08C: [("G", 0x80071CB0)],
            0x80074D58: [("G", 0x80076C58), ("G", 0x8007C97C)],
            0x80093E6C: [("G", 0x80078C58), ("S", 0x8003CFA4), ("S", 0x8003DAA0)],
            0x800950E8: [("G", 0x80078C60), ("S", 0x8003CFAC), ("S", 0x8003DAA8)],
            0x8007AC04: [("G", 0x8007DDDC), ("G", 0x8007DDEC)],
        },
        # the dt 0x80071BCC receives (a1 -> s1 at 0x80071BEC) is read only as a1 in
        # the delay slots of its two calls to 0x80072994, which takes one argument
        "dt_forward": [0x80071C40, 0x80071CA0],
        # the masks 0x80071BCC decides on, (andi site -> immediate)
        "migrate_masks": {0x80071C04: 0x600, 0x80071C0C: 0x800, 0x80071C14: 0x1FF,
                          0x80071C30: 0x1000, 0x80071C4C: 0x400, 0x80071C6C: 0x1FF,
                          0x80071C74: 0x800, 0x80071C90: 0x1000},
        # (images, bike states, states on the list the rule names, pending)
        "migrate_census": (18, 324, 324, 0),
        # constants built by lui/ori pairs, (lui, ori) -> value
        "p1_consts": {(0x80071CF0, 0x80071D00): 0xF7FFFFFF, (0x8007F08C, 0x8007F090): 0xFF7FFFFF,
                      (0x800729A0, 0x800729B8): 0xC0018200, (0x80072AE4, 0x80072AEC): 0xFFFC7F1F,
                      (0x80074D98, 0x80074DDC): 0x000D6944, (0x80074E28, 0x80074E30): 0x002DD62D},
        # 0x80074D58's gate is flagsA (+0x230) & 0x400
        "p1_gates": {0x80074D6C: ("lw", 0x230), 0x80074D74: ("andi", 0x400)},
        # the two heading writers, and their function
        "heading_writers": {0x80077C80: FN_START, 0x8007AE38: 0x8007AC04},
        # probe hits over the part-1 trace runs (work\bike_step\p1)
        "p1_live": {"never": [0x80071BCC, 0x8007B8E4, 0x8007DD28, 0x80072994, 0x80074D58,
                              0x80090D84, 0x80077C80, 0x80078058],
                    "always": [0x80093E6C, 0x800950E8, 0x8007AC04, 0x8007AE38, 0x800C4550]},
        # the race loader (RASHCDI) facts
        "stats_loader": {"level_ph_ref": (0x800654D0, 0x8005B8D4), "level_ph_bytes": (0x80065530, 1344),
                         "bike_ph_bytes": (0x800655A8, 448), "bank_field": (0x800654E8, 0x3C),
                         "base_ptr": (0x800661A4, 0x8005B248), "store": (0x800661B0, 0x22C),
                         "blocks": {0x80067BC0: 5, 0x80067BC4: 4}},
        # where each block's 448 bytes come from ("LEVEL" = LEVEL<bank+1>.PH)
        "stats_source": {0: ("LEVEL", 0), 1: ("LEVEL", 448), 2: ("LEVEL", 896), 3: ("CRUISEA1.PH", 0)},
        # (images, bikes, pointers = base + 448*idx, blocks equal, blocks checked)
        "stats_census": (18, 324, 324, 72, 72),
        # every store through a +0x22C pointer in the four code images
        "stats_writers": {("G", 0x80094298): 0xE0, ("G", 0x80095B74): 0x24, ("G", 0x80095E64): 0xE0,
                          ("G", 0x800C964C): 0xE0, ("I", 0x80066280): 0x1BD},
        # the one store of that scan that is NOT into a stat block - 0x80095AEC's
        # object is a RIDER (e[+0x354]), whose +0x22C is another field: `lw a0,0x354(..)` feeds it
        "rider_22c": {0x80095AEC: [0x8009050C, 0x800927B4]},
        # (trace runs, writes into the 1792-byte stat array) - live
        "stats_live": (4, 0),
        # (trace runs, writes into the 18 bikes' +0x22C words) - live
        "ptr_live": (2, 0),
        # RASHCDI 0x800644C8 saves the cop block's +0xE0 into 0x800D86F0+8 (and sets
        # +0x13 = 1); (images where *(0x800D86F8) == block 2's +0xE0 and the byte +0x13 is 1)
        "cop_record": {"load": (0x800645D0, 0xE0), "save": (0x800645D8, 8), "images": 18},
        # (object, offset, R/W, instruction) for the load-bearing new/refined fields
        "fields": [("E", 0x1E4, "W", 0x800781F4), ("E", 0x1E4, "W", 0x800782D0),
                   ("E", 0x240, "W", 0x80078284), ("E", 0x258, "W", 0x800767C0),
                   ("E", 0x2D4, "W", 0x80078A34), ("E", 0x444, "R", 0x800767F4),
                   ("E", 0x1D4, "W", 0x80078AF4), ("PAS", 0x1D4, "W", 0x80078B0C),
                   ("E", 0x39C, "W", 0x80075F74), ("EP", 0x144, "R", 0x80076524)],
        # phase and region boundaries
        "phase_starts": {p[2]: p[0] for p in PHASES},
        "cuts": [(n, a, b) for n, a, b in CUTS],
        # live-in registers allowed at each region entry besides gp/sp
        "livein": {"A": {3, 19}, "C": {17}, "G": {4}, "I": {3}},
        # the list head each arm of 0x80071BCC links into, and B's two list arguments
        "list_heads": {0x80071BF8: 0x8005B270, 0x80071C5C: 0x8005B2D8, 0x80071C68: 0x8005B378,
                       0x80071CAC: 0x8005B350, 0x80071CBC: 0x8005B298},
        "b_lists": [0x8005B298, 0x8005B2D8, 0x8005B298, 0x8005B2D8],
        # the spawner accumulator 0x8008CD88 adds dt into
        "spawn_acc": 0x8005B318,
        # the clean-run union
        "coverage": (256, 1493),
    }


_CACHE = {}


def cached(name, fn):
    if name not in _CACHE:
        _CACHE[name] = fn()
    return _CACHE[name]


def addiu_target(at):
    """The address a `lui`-high + `addiu` pair forms, `addiu` at `at` (the lui is searched back)."""
    w = word(at)
    rs = (w >> 21) & 31
    for back in range(1, 8):
        u = word(at - 4 * back)
        if (u >> 26) == 0x0F and ((u >> 16) & 31) == rs:
            return (((u & 0xFFFF) << 16) + simm(w)) & 0xFFFFFFFF
    return None


def run_checks(c, verbose=True):
    """Returns [(claim, ok, message)]. Every check names the claim it guards."""
    res = []

    def check(claim, cond, msg):
        res.append((claim, bool(cond), msg))
        if verbose and not cond:
            print("  FAIL", msg)

    g = images()[0]
    check("sha1", g.sha1 == c["sha1"], "RASHCDG.BIN sha1")
    blocks, succ, code = cached("blocks", compute_blocks)
    jrs = [a for a, w in code.items() if w == 0x03E00008]
    check("jr_ra", jrs == c["jr_ra"], f"the jr ra sites {[hex(x) for x in jrs]}")
    njalr = sum(1 for a, w in code.items() if branch_target(a, w) and branch_target(a, w)[0] == "jalr")
    check("jalr", njalr == c["jalr"], f"{njalr} jalr")
    check("insns", len(code) == c["insns"], f"{len(code)} instructions")
    check("blocks", len(blocks) == c["blocks"], f"{len(blocks)} basic blocks")
    # phases: tile the function, start on a leader (C starts mid-block by design, 2.2)
    starts = c["phase_starts"]
    order = [p[2] for p in PHASES]
    check("phase_starts", starts[order[0]] == FN_START, "the first phase starts at the entry")
    leaders = {s0 for s0, _ in blocks}
    for k, (a, b) in enumerate(zip(order, order[1:])):
        check("phase_starts", starts[b] > starts[a], f"phase {b} follows {a}")
    for n in order:
        check("phase_starts", starts[n] in leaders or n == "C", f"phase {n} starts on a leader")
    for n, a0, _ in c["cuts"]:
        if n in starts and n != "A":
            check("phase_starts", starts[n] == a0, f"phase {n} starts at region {n}'s entry")
    _, _, _, entry, acc, calls = cached("dataflow", dataflow)
    check("calls", len(calls) == c["calls"], f"{len(calls)} call sites")
    tally = collections.Counter(t for _, t, _, _ in calls)
    check("fixmul_fixdiv", (tally[0x8001FC90], tally[0x80010028]) == c["fixmul_fixdiv"],
          f"FixMul/FixDiv sites {tally[0x8001FC90]}/{tally[0x80010028]}")
    site_tgt = {a: t for a, t, _, _ in calls}
    nonleaf = {a: t for a, t in site_tgt.items() if t not in (0x8001FC90, 0x80010028)}
    check("sites", nonleaf == c["sites"], "the non-leaf call sites and their callees (3.1)")
    for t, n in c["arity"].items():
        got = arity(t)
        check("arity", got == n, f"0x{t:08X} takes {got} register argument(s)")
    for t in c["ported"]:
        check("ported", t in PORTED, f"0x{t:08X} has a bench row")
    for t in c["unported"]:
        check("unported", t not in PORTED, f"0x{t:08X} has no bench row")
    idx = {}
    for x in acc:
        if x.base is not None and x.base[0] == "P" and x.base[2] is not None:
            idx[(x.addr, x.rw)] = (x.base[1], x.base[2] + x.off)
    for kind, off, rw, at in c["fields"]:
        got = idx.get((at, rw))
        check("fields", got == (kind, off), f"{at:08X} {rw} is {kind}+0x{off:X} (dataflow: {got})")
    for s0 in (0x80076228, 0x8007683C, 0x80077538, 0x80078494, 0x80078AD4, 0x80078C2C, 0x80075F34):
        check("dataflow", entity_reg_at(entry[s0]) is not None, f"bike resolved at {s0:08X}")
    at_live = cached("live", function_liveness)
    for name, s0, e0 in c["cuts"]:
        bad = cut_edges_ok(s0, e0)
        check("cuts", not bad, f"region {name} single-entry/single-exit {bad}")
        if s0 not in at_live:
            check("cuts", False, f"region {name} entry {s0:08X} is not an instruction")
            continue
        regs, slots = at_live[s0]
        extra = set(regs) - {28, 29} - c["livein"].get(name, set())
        check("livein", not extra, f"region {name} live-in {sorted(R[x] for x in extra)}")
        check("livein", not {k for k in slots if k < 96}, f"region {name} live-in locals")
    for at, head in c["list_heads"].items():
        got = addiu_target(at)
        check("list_heads", got == head, f"0x80071BCC arm {at:08X} links into {got and hex(got)}")
    b_args = [vstr(regs[4]) for a, t, regs, _ in sorted(calls) if ptag(a) == "B"]
    check("list_heads", b_args == [f"0x{x:08X}" for x in c["b_lists"]], f"B's list arguments {b_args}")
    acc_addr = ((word(0x8008CD90) & 0xFFFF) << 16) + simm(word(0x8008CD94))
    check("spawn_acc", acc_addr == c["spawn_acc"], f"0x8008CD88 accumulates into 0x{acc_addr:08X}")
    paths = expand_csv([CLEAN_RUNS])
    if paths:
        _, _, _, _, nb, ni = cached("cov", lambda: coverage_data(paths))
        check("coverage", (nb, ni) == c["coverage"], f"clean-run union {nb} blocks / {ni} insns "
              f"over {len(paths)} runs")

    # ---- the remaining callees ----
    for f, want in c["p1_callees"].items():
        got = set(callee_info(f)["jals"])
        check("p1_callees", got == want, f"0x{f:08X} calls {sorted(hex(x) for x in got)}")
    for f, want in c["callers"].items():
        got = race_callers(f)
        check("callers", got == sorted(want), f"0x{f:08X} is called from {[(t, hex(a)) for t, a in got]}")
    s1 = reg_readers(0x80071BCC, 17)
    check("dt_forward", s1 == c["dt_forward"], f"0x80071BCC reads s1 (= dt) at {[hex(x) for x in s1]}")
    for a in c["dt_forward"]:
        bt = branch_target(a - 4, word(a - 4))
        w = word(a)                      # `addu a1,s1,zero`
        ok = bt is not None and bt[0] == "jal" and arity(bt[1]) < 2 and (w >> 26) == 0 \
            and (w & 0x3F) == 0x21 and ((w >> 16) & 31) == 0 and ((w >> 11) & 31) == 5 \
            and ((w >> 21) & 31) == 17
        check("dt_forward", ok, f"{a:08X} is `move a1,..` in the delay slot of a call that ignores a1")
    for a, imm in c["migrate_masks"].items():
        got = imm_of(images()[0], a, 0x0C)
        check("migrate_masks", got == imm, f"0x80071BCC andi at {a:08X} is {got and hex(got)}")
    got_mc, _ = cached("migrate_census", migrate_census)
    check("migrate_census", got_mc == c["migrate_census"],
          f"list census (images, states, on the predicted list, pending) = {got_mc}")
    for (lu, lo), v in c["p1_consts"].items():
        got = lui_ori(images()[0], lu, lo)
        check("p1_consts", got == v, f"lui {lu:08X} + ori {lo:08X} = {got and hex(got)}")
    for a, (mn, imm) in c["p1_gates"].items():
        op = {"lw": 0x23, "andi": 0x0C}[mn]
        got = imm_of(images()[0], a, op)
        check("p1_gates", got == imm, f"{mn} at {a:08X} has immediate {got and hex(got)}")
    for a, fn in c["heading_writers"].items():
        w = word(a)
        is_sh = (w >> 26) == 0x29 and (w & 0xFFFF) == 0x1C2
        inside = FN_START <= a < FN_END if fn == FN_START else func_of(a, [fn]) == fn
        check("heading_writers", is_sh and inside and (fn == FN_START) == (FN_START <= a < FN_END),
              f"{a:08X} is `sh ..,0x1C2(..)` inside 0x{fn:08X}")
    runs = cached("p1_hits", p1_probe_hits)
    if runs:
        for a in c["p1_live"]["never"]:
            check("p1_live", all(h.get(a) == 0 for _, h in runs),
                  f"{a:08X} probed and never hit in {len(runs)} runs")
        for a in c["p1_live"]["always"]:
            check("p1_live", all((h.get(a) or 0) > 0 for _, h in runs),
                  f"{a:08X} hit in every one of {len(runs)} runs")

    # ---- the stat block ----
    cdi = rashcdi()
    L = c["stats_loader"]
    at, target = L["level_ph_ref"]
    fmt = addiu_target_img(cdi, at)
    name = cdi.data[cdi.off(fmt):cdi.off(fmt) + 16].split(b"\0")[0] if fmt and cdi.contains(fmt) else b""
    check("stats_loader", fmt == target and name == b"%sLEVEL%ld.ph",
          f"RASHCDI {at:08X} names 0x{fmt or 0:08X} {name!r}")
    for key in ("level_ph_bytes", "bike_ph_bytes"):
        a, n = L[key]
        got = imm_of(cdi, a, 0x09)
        check("stats_loader", got == n, f"RASHCDI {a:08X} reads {got} bytes")
    a, off = L["bank_field"]
    check("stats_loader", imm_of(cdi, a, 0x23) == off, f"RASHCDI {a:08X} takes n-1 from game_state+0x{off:X}")
    a, g = L["base_ptr"]
    got = lw_target_img(cdi, a)
    check("stats_loader", got == g, f"RASHCDI {a:08X} loads the base from 0x{got or 0:08X}")
    a, off = L["store"]
    w = cdi.word(a)
    check("stats_loader", (w >> 26) == 0x2B and (w & 0xFFFF) == off, f"RASHCDI {a:08X} is `sw ..,0x{w & 0xFFFF:X}(..)`")
    # the index multiply feeding that store: sll 3, subu, sll 6 = x 448
    mul = [cdi.word(x) for x in (0x8006619C, 0x800661A0, 0x800661A8)]
    k = (1 << ((mul[0] >> 6) & 31)) - 1 if (mul[0] & 0x3F) == 0 and (mul[1] & 0x3F) == 0x23 else 0
    k <<= ((mul[2] >> 6) & 31) if (mul[2] & 0x3F) == 0 else 0
    check("stats_loader", k == L["bike_ph_bytes"][1] == STAT_BLOCK, f"the block index is scaled by {k}")
    for a, n in L["blocks"].items():
        check("stats_loader", imm_of(cdi, a, 0x09) == n, f"RASHCDI {a:08X} sets the block count {imm_of(cdi, a, 0x09)}")
    got_sc, _ = stats_census(c["stats_source"]) if c["stats_source"] != claims()["stats_source"] \
        else cached("stats_census", lambda: stats_census(c["stats_source"]))
    check("stats_census", got_sc == c["stats_census"],
          f"stat census (images, bikes, pointers ok, blocks equal, blocks checked) = {got_sc}")
    got_w = cached("stat_writers", stat_writers)
    check("stats_writers", got_w == c["stats_writers"],
          f"stores through +0x22C: {sorted((t, hex(a), hex(o)) for (t, a), o in got_w.items())}")
    for fn, sites in c["rider_22c"].items():
        for a in sites:
            w = word(a)
            jal_after = [b for b in range(a + 4, a + 32, 4)
                         if branch_target(b, word(b)) == ("jal", fn)]
            check("rider_22c", (w >> 26) == 0x23 and ((w >> 16) & 31) == 4 and (w & 0xFFFF) == 0x354
                  and bool(jal_after), f"{a:08X} loads a0 from +0x354 before calling 0x{fn:08X}")
    cr = c["cop_record"]
    ok_ld = imm_of(cdi, cr["load"][0], 0x23) == cr["load"][1]
    ok_sv = imm_of(cdi, cr["save"][0], 0x2B) == cr["save"][1]
    check("cop_record", ok_ld and ok_sv, f"RASHCDI {cr['load'][0]:08X} lw +0x{imm_of(cdi, cr['load'][0], 0x23) or 0:X}, "
          f"{cr['save'][0]:08X} sw +0x{imm_of(cdi, cr['save'][0], 0x2B) or 0:X}")
    same = sum(1 for _, r in race_images()
               if rw32(r, 0x800D86F0 + cr["save"][1]) == rw32(r, rw32(r, 0x8005B248) + 2 * STAT_BLOCK + cr["load"][1])
               and r[0x0D86F0 + 0x13] == 1)
    check("cop_record", same == cr["images"], f"{same} images hold the cop block's +0xE0 at 0x800D86F8")
    nr, _, nw = cached("p1_watch", p1_watch_writes)
    if nr:
        check("stats_live", (nr, nw) == c["stats_live"], f"{nr} runs watching the stat array, {nw} writes")
    nr, _, nw = cached("p1_watch_w", lambda: p1_watch_writes("w_"))
    if nr:
        check("stats_live", (nr, nw) == c["ptr_live"], f"{nr} runs watching the 18 +0x22C words, {nw} writes")
    return res


# One mutation per claim: each is a statement that could have been read wrong.
MUTATIONS = [
    ("sha1", "a different RASHCDG image", lambda c: c.update(sha1="0" * 40)),
    ("insns", "2996 instructions", lambda c: c.update(insns=2996)),
    ("blocks", "571 basic blocks", lambda c: c.update(blocks=571)),
    ("jr_ra", "the jr ra one word early", lambda c: c.update(jr_ra=[0x80078DA8])),
    ("jalr", "one jalr", lambda c: c.update(jalr=1)),
    ("calls", "96 call sites", lambda c: c.update(calls=96)),
    ("fixmul_fixdiv", "34 FixDiv sites", lambda c: c.update(fixmul_fixdiv=(38, 34))),
    ("sites", "G calls 0x8007FA4C, not 0x8007F0BC",
     lambda c: c["sites"].update({0x80078B3C: 0x8007FA4C})),
    ("arity", "0x8007FA4C takes dt as a second argument", lambda c: c["arity"].update({0x8007FA4C: 2})),
    ("ported", "0x80093E6C already has a bench row", lambda c: c["ported"].add(0x80093E6C)),
    ("unported", "0x80071BCC is still unported",
     lambda c: c["unported"].add(0x80071BCC)),
    ("unported", "EndRace is not ported", lambda c: c["unported"].add(0x80092C7C)),
    ("unported", "0x80074C84 is still unported",
     lambda c: c["unported"].add(0x80074C84)),
    # the remaining callees
    ("p1_callees", "0x80074D58 calls 0x80091468 as well",
     lambda c: c["p1_callees"][0x80074D58].add(0x80091468)),
    ("p1_callees", "0x8008CF74 is not a leaf", lambda c: c["p1_callees"].update({0x8008CF74: {0x8001FC90}})),
    ("callers", "0x80071BCC is called only by the step",
     lambda c: c["callers"].update({0x80071BCC: [("G", 0x800761B0)]})),
    ("callers", "only the step calls 0x80093E6C", lambda c: c["callers"].update({0x80093E6C: [("G", 0x80078C58)]})),
    ("arity", "0x80072994 takes dt", lambda c: c["arity"].update({0x80072994: 2})),
    ("dt_forward", "0x80071BCC also reads dt when it links", lambda c: c["dt_forward"].append(0x80071CC0)),
    ("migrate_masks", "the thrown-bike list is chosen by 0x200, not 0x400",
     lambda c: c["migrate_masks"].update({0x80071C4C: 0x200})),
    ("migrate_census", "one bike sits on a list the rule does not name",
     lambda c: c.update(migrate_census=(18, 324, 323, 0))),
    ("p1_consts", "0x80074D58 launches the passenger at +0xD6945",
     lambda c: c["p1_consts"].update({(0x80074D98, 0x80074DDC): 0xD6945})),
    ("p1_gates", "0x80074D58 is gated on flagsB (+0x234)", lambda c: c["p1_gates"].update({0x80074D6C: ("lw", 0x234)})),
    ("heading_writers", "0x8007AE38 is inside the step", lambda c: c["heading_writers"].update({0x8007AE38: FN_START})),
    ("p1_live", "0x8007AE38 never runs in a capture", lambda c: c["p1_live"]["never"].append(0x8007AE38)),
    ("p1_live", "the step's own 0x80071BCC call runs", lambda c: c["p1_live"]["always"].append(0x800761B0)),
    # the stat block
    ("stats_loader", "LEVEL<n>.ph is read whole (2400 bytes)",
     lambda c: c["stats_loader"].update(level_ph_bytes=(0x80065530, 2400))),
    ("stats_loader", "a bike .PH is read whole (484 bytes)",
     lambda c: c["stats_loader"].update(bike_ph_bytes=(0x800655A8, 484))),
    ("stats_loader", "the LEVEL index is the difficulty game_state+0x3A",
     lambda c: c["stats_loader"].update(bank_field=(0x800654E8, 0x3A))),
    ("stats_census", "the AI blocks are GLOBALS.BI's tail",
     lambda c: c["stats_source"].update({0: ("GLOBALS.BI", 0x0A5C)})),
    ("stats_census", "block 1 is LEVEL<n>.PH's second 484-byte record",
     lambda c: c["stats_source"].update({1: ("LEVEL", 484)})),
    ("stats_census", "one bike's pointer is off", lambda c: c.update(stats_census=(18, 324, 323, 72, 72))),
    ("stats_writers", "nothing writes stats[+0xE0] at run time",
     lambda c: c.__setitem__("stats_writers", {k: v for k, v in c["stats_writers"].items() if v != 0xE0})),
    ("stats_live", "the race writes the stat array", lambda c: c.update(stats_live=(4, 1))),
    ("stats_live", "the race re-points a bike's +0x22C", lambda c: c.update(ptr_live=(2, 1))),
    ("cop_record", "0x800D86F8 is a ramp scale, not a copy of stats+0xE0",
     lambda c: c["cop_record"].update(load=(0x800645D0, 0xE4))),
    ("rider_22c", "0x80095AEC is handed the bike (its +0x22C is the stat block)",
     lambda c: c["rider_22c"].update({0x80095AEC: [0x8009050C, 0x80090514]})),
    ("cop_record", "one image's cop record disagrees", lambda c: c["cop_record"].update(images=17)),
    ("fields", "the net acceleration is +0x1E8, not +0x1E4",
     lambda c: c.__setitem__("fields", [("E", 0x1E8, "W", 0x800781F4)] + c["fields"][1:])),
    ("phase_starts", "G starts at 0x80078AA8", lambda c: c["phase_starts"].update(G=0x80078AA8)),
    ("cuts", "the F/G cut at 0x80078AA8",
     lambda c: c.__setitem__("cuts", [(n, a if n != "G" else 0x80078AA8, b if n != "F" else 0x80078AA8)
                                      for n, a, b in c["cuts"]])),
    ("livein", "region G needs nothing but dt", lambda c: c["livein"].pop("G")),
    ("list_heads", "the 0x400 arm links into 0x8005B378",
     lambda c: c["list_heads"].update({0x80071C5C: 0x8005B378})),
    ("list_heads", "B steers list 0x8005B270 second", lambda c: c.__setitem__(
        "b_lists", [0x8005B298, 0x8005B270, 0x8005B298, 0x8005B270])),
    ("spawn_acc", "rules.md 9.2's 0x8005B2D8", lambda c: c.update(spawn_acc=0x8005B2D8)),
    ("coverage", "257 blocks in the clean union", lambda c: c.update(coverage=(257, 1493))),
]


def cmd_verify(args):
    header()
    base = run_checks(claims(), verbose=True)
    fails = sum(1 for _, ok, _ in base if not ok)
    if not args.mutate:
        if not any(cl == "coverage" for cl, _, _ in base):
            print("  (coverage not checked: no work\\bike_step\\cov_*\\probes.csv)")
        print(f"bike_step: {len(base)} checks, {fails} failures")
        return 1 if fails else 0
    if fails:
        print(f"bike_step: the unmutated claims already fail ({fails}); --mutate is meaningless")
        return 1
    import copy
    detected = 0
    ran = 0
    for claim, what, fn in MUTATIONS:
        c = copy.deepcopy(claims())
        fn(c)
        res = run_checks(c, verbose=False)
        guarded = [r for r in res if r[0] == claim]
        if not guarded:
            print(f"  n/a       {claim:13s} {what} (no check ran for this claim)")
            continue
        ran += 1
        caught = [m for cl, ok, m in res if not ok and cl == claim]
        if caught:
            detected += 1
            print(f"  DETECTED  {claim:13s} {what}  <- {caught[0]}")
        else:
            print(f"  MISSED    {claim:13s} {what}")
    print(f"bike_step: {ran} mutations, {detected} detected")
    return 0 if detected == ran and ran > 0 else 1


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("blocks")
    sub.add_parser("calls")
    p = sub.add_parser("fields")
    p.add_argument("--all", action="store_true")
    p.add_argument("--md", action="store_true")
    sub.add_parser("globals")
    sub.add_parser("callee")
    p = sub.add_parser("probes")
    p.add_argument("--out")
    p = sub.add_parser("coverage")
    p.add_argument("--csv", nargs="+", required=True,
                   help="probes.csv files, directories holding them, or globs of either")
    p.add_argument("--detail", action="store_true")
    p = sub.add_parser("verify")
    p.add_argument("--mutate", action="store_true",
                   help="perturb each documented claim in turn and show its check failing")
    sub.add_parser("cuts")
    sub.add_parser("remaining", help="the step's remaining callees, callers, list census")
    sub.add_parser("stats", help="where entity+0x22C points and what fills it")
    a = ap.parse_args()
    rc = {"blocks": cmd_blocks, "calls": cmd_calls, "fields": cmd_fields, "globals": cmd_globals,
          "callee": cmd_callee, "probes": cmd_probes, "coverage": cmd_coverage, "cuts": cmd_cuts,
          "verify": cmd_verify, "remaining": cmd_remaining, "stats": cmd_stats}[a.cmd](a)
    sys.exit(rc or 0)


if __name__ == "__main__":
    main()
