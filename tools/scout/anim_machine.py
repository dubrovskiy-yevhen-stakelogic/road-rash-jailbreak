r"""Scout probe for the RIDER ANIMATION MACHINE of Road Rash: Jailbreak (USA, SLUS_01053):
RASHCDG 0x8005BD74..0x8005E848 (cfe43a77...).

Run through tools\scout\anim.py (which keeps its older pose commands):

    python tools\scout\anim.py info             # skeleton + data only (seconds)
    python tools\scout\anim.py verify           # anim: <n> checks, <m> failures      (~10 minutes)
    python tools\scout\anim.py verify --mutate  # every mutated claim fails its own check, nothing else
    python tools\scout\anim.py live --only crash,punch-cop --keep     # runs, traces kept under work\anim\probe

What it re-derives, independently of the C++ tree:

  * SKELETON (the images): the 28 functions of the range and their extents, the closure (no Rand, no GetRCnt, no
    jalr), the sequencer's jump table, the one GTE user and its four commands, the call sites of every entry point,
    the words the arithmetic rests on, the bank loader's file table (RASHCDI) and the presentation pass's dispatch
    onto the three clock writers;
  * DATA (the player's own files against 18 RAM images - 4 savestates and the 14 frame dumps of the VR capture): the six
    resident animation banks ARE DATA\ANIMTBL*.PSX (bank slot by slot), DATA\ANIMNOIZ.DAT is resident and is the
    source of the per-stance event lists, the stance table's bank / clip references resolve, every channel value of
    every sampled clip equals our own DMD3 decoder (anim_dmd3.py), every posed part matrix equals the quaternion
    formula, and FIGHT.BIN's hit frames against the clips they play;
  * LIVE: the transcriptions (anim_models.py) run as models against the unmodified original in our interpreter on
    every call of every run (impact.py's strict harness, plus a comparison of the frame buffers behind pointer
    arguments with the guest's own stack writes);
  * FACTS: the time base (dt = game_state+0x1C), a queued transition refused on a busy channel, the fight's strike
    frame against the clip frame, frozen clips skipped by the pass, no Rand from inside the machine.

Reads ONLY work\disc_us and work\oracle; writes ONLY under work\anim\probe. <build>\rrverify.exe is used read-only,
and never without --out. No game bytes are copied into the repository."""

from __future__ import annotations

import argparse
import bisect
import glob
import os
import shutil
import struct
import sys
from collections import Counter

import warnings
warnings.filterwarnings("ignore", category=SyntaxWarning)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pairs as P            # noqa: E402  the per-call harness
import impact as I           # noqa: E402  the strict harness and the data-edited runs
import exe as E              # noqa: E402
import anim_models as AM     # noqa: E402
import anim_dmd3 as D        # noqa: E402

ROOT = P.ROOT
WORK = os.path.join(ROOT, "work", "anim", "probe")
DATA = os.path.join(ROOT, "work", "disc_us", "DATA")
SHA1 = {"SLUS": "67ed165a2c517d4e6106fb0dfa324d66dd9a76f1", "G": "cfe43a7786759f2cb9c57751cf99e84d1074782c",
        "I": "9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06"}
LO, HI = 0x8005BD74, 0x8005E848
POOL = 0x800CE170                 # {base, programs, ?, count, bank slots, used, capacity, 0, banks u32[16] at +0x20}
BANKS = 0x800CE190
MUTATE = P.MUTATE


def cfg(orig, mutated):
    return mutated if MUTATE["on"] else orig


u32, s32 = P.u32, P.s32


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


class Bench:
    def __init__(self, verbose=True):
        self.n = self.fail = 0
        self.verbose = verbose

    def check(self, ok, what):
        self.n += 1
        if not ok:
            self.fail += 1
        if self.verbose or not ok:
            print(("  ok   " if ok else "  FAIL ") + what)
        return ok


# ---------------------------------------------------------------------------------------------------------- images

_RI = {}


def rashcdi():
    if not _RI:
        _RI["I"] = E.load_overlay("RASHCDI.BIN", 0x8005B5E8)
    return _RI["I"]


def byte_at(a):
    return (P.word_at(a & ~3) >> (8 * (a & 3))) & 0xFF


def slus_string(a):
    out = b""
    while True:
        b = byte_at(a)
        if b == 0:
            return out.decode("latin-1")
        out += bytes([b])
        a += 1


def functions():
    """the functions of [LO, HI): each from its first word to its last `jr ra` (impact.extent), padding skipped"""
    out, a = [], LO
    while a < HI:
        lo, hi = I.extent(a)
        out.append((lo, hi))
        a = hi
        while a < HI and P.word_at(a) == 0:
            a += 4
    return out


def closure(roots):
    seen, st = {}, list(roots)
    while st:
        f = st.pop()
        if f in seen:
            continue
        lo, hi = I.extent(f)
        seen[f] = (hi - lo) // 4
        st += list(I.callees(f))
    return seen


def words(lo, hi):
    return [P.word_at(a) for a in range(lo, hi, 4)]


# ------------------------------------------------------------------------------------------------------- skeleton

# (entry, bytes, call sites)
TABLE = [
    (0x8005BD74, 152, 5), (0x8005BE0C, 20, 1), (0x8005BE20, 36, 1), (0x8005BE44, 20, 1), (0x8005BE58, 156, 18),
    (0x8005BEF4, 120, 1), (0x8005BF6C, 172, 4), (0x8005C018, 152, 5), (0x8005C0B0, 144, 3), (0x8005C140, 504, 1),
    (0x8005C338, 52, 2), (0x8005C36C, 48, 2), (0x8005C39C, 124, 4), (0x8005C418, 212, 2), (0x8005C4EC, 64, 3),
    (0x8005C52C, 96, 1), (0x8005C58C, 872, 1), (0x8005C8F4, 528, 2), (0x8005CB04, 108, 1), (0x8005CB70, 1848, 1),
    (0x8005D2A8, 196, 1), (0x8005D36C, 720, 1), (0x8005D63C, 2972, 1), (0x8005E1D8, 228, 2), (0x8005E2BC, 668, 1),
    (0x8005E558, 76, 3), (0x8005E5A4, 668, 1), (0x8005E840, 8, 1),
]

# words the transcription rests on (pc, word, what)
WORDS = [
    (0x8005BDAC, 0x304200E5, "andi v0,v0,0xe5 - Restart keeps flags bits 0,2,5,6,7 and clears everything else"),
    (0x8005BD8C, 0x2403000A, "li v1,10 - Restart's default rate"),
    (0x8005C450, 0x2C620006, "sltiu v0,v1,6 - the sequencer's six opcodes"),
    (0x8005C4BC, 0x0102102A, "slt v0,t0,v0 - the sequencer's guard: program length < pc + steps"),
    (0x8005C4F8, 0x00A3102A, "slt v0,a1,v1 - Tick: acc < rate (signed)"),
    (0x8005C648, 0x3402FFFF, "li v0,0xffff - Advance compares the SIGN-EXTENDED loop count with 0x0000FFFF"),
    (0x8005C804, 0xA0600003, "sb zero,3(v1) - op3's rewrite of op 0"),
    (0x8005CBF4, 0x240A047C, "li t2,1148 - the mirror offset (TransitionCapture)"),
    (0x8005D718, 0x2417047C, "li s7,1148 - the mirror offset (Pose)"),
    (0x8005D6E0, 0x0043001A, "div zero,v0,v1 - Pose's sub-frame fraction (sub << 16) / rate"),
    (0x8005D8F4, 0x00121380, "sll v0,s2,0xe - Pose's 14-bit lerp fraction"),
    (0x8005D30C, 0x92020024, "lbu v0,36(s0) - ApplyFrame reads the flags BYTE ..."),
    (0x8005D318, 0xAE020024, "sw v0,36(s0) - ... and stores it as a WORD: bits 8..31 of the flags are cleared"),
    (0x8005E26C, 0x30420008, "andi v0,v0,0x8 - AnimationPass: bit 3 freezes the clock"),
    (0x8005E280, 0x8C45001C, "lw a1,28(v0) - AnimationPass: dt = game_state+0x1C"),
    (0x8005E45C, 0x00021202, "srl v0,v0,0x8 - DecodeHeader: the delta width is the high byte of the 4th halfword"),
    (0x8005E708, 0x24420004, "addiu v0,v0,4 - Sample: a run is 4 + the count code long"),
    (0x8005E79C, 0x00071243, "sra v0,a3,0x9 - Sample: key = (acc * scale) >> 9"),
    (0x800C3364, 0x00101023, "negu v0,s0 - FallScrubLean's sign flip ..."),
    (0x800C336C, 0x00822024, "and a0,a0,v0 - ... masked with -(last frame), not with a 0/1 flag"),
]

# the sequencer's jump table 0x8005B5EC: opcode -> target
JT = [0x8005C4B4, 0x8005C4B4, 0x8005C470, 0x8005C4B4, 0x8005C48C, 0x8005C4A0]

# RASHCDI 0x80066414..0x80066594: (addiu a1 site, file literal, store site, store word) -> bank slot
LOADER = [
    (0x80066430, "DATA\\ANIMTBL2.PSX", 0x80066440, 0xAE220004, 1),
    (0x80066464, "DATA\\ANIMTBL3.PSX", 0x80066470, 0xAE220008, 2), (0x8006645C, "DATA\\ANIMTBJ3.PSX", 0x80066470, 0xAE220008, 2),
    (0x80066530, "DATA\\ANIMTBL1.PSX", 0x80066554, 0xAE42E190, 0), (0x800664AC, "DATA\\ANIMTBS1.PSX", 0x800664D0, 0xAE02E190, 0),
    (0x8006654C, "DATA\\ANIMTBLW.PSX", 0x80066558, 0xAE22000C, 3), (0x800664C8, "DATA\\ANIMTBSW.PSX", 0x800664D8, 0xAE42000C, 3),
    (0x80066524, "DATA\\ANIMTBLB.PSX", 0x80066540, 0xAE220010, 4), (0x800664A0, "DATA\\ANIMTBSB.PSX", 0x800664BC, 0xAE420010, 4),
    (0x80066580, "DATA\\ANIMTBLP.PSX", 0x8006658C, 0xAC62E1A4, 5),
    (0x800664FC, "DATA\\ANIMTBLS.PSX", 0x8006650C, 0xAE420018, 6), (0x800664F4, "DATA\\ANIMTBLJ.PSX", 0x8006650C, 0xAE420018, 6),
]

# the presentation pass 0x80090814 on a rider with mount 2: site -> callee
PRESENT = [(0x80090C98, 0x800C31CC), (0x80090CA8, 0x800C341C), (0x80090CB8, 0x800C3630), (0x80090CCC, 0x8005BE58),
           (0x80090CDC, 0x80091468), (0x80090D00, 0x800C5078)]
PRESENT_WORDS = [(0x80090C08, 0x24020058, "li v0,88"), (0x80090C18, 0x24020027, "li v0,39"),
                 (0x80090C28, 0x24020026, "li v0,38"), (0x80090C3C, 0x24020028, "li v0,40"),
                 (0x80090C50, 0x24020059, "li v0,89"), (0x80090C8C, 0x30420200, "andi v0,v0,0x200"),
                 (0x80090C78, 0x30420040, "andi v0,v0,0x40")]

RAND, GETRCNT = 0x8001FC58, 0x80043F00


def check_skeleton(bench):
    im = P.images()
    bench.check(im["SLUS"].sha1 == SHA1["SLUS"] and im["G"].sha1 == SHA1["G"] and rashcdi().sha1 == SHA1["I"],
                "image hashes: SLUS 67ed165a, RASHCDG cfe43a77, RASHCDI 9a8b79d8")
    I.call_sites(0)
    fs = functions()
    got = [(lo, hi - lo, len(I.call_sites(lo))) for lo, hi in fs]
    want = list(TABLE)
    if MUTATE["on"]:
        want[4] = (0x8005BE58, 152, 18)                        # mutated: ClipDone 4 bytes short
    bench.check(got == want, f"the range 0x8005BD74..0x8005E848 is {len(fs)} functions, extents and call-site counts "
                              f"as tabled ({sum(b for _, b, _ in got) // 4} instructions)")
    n_ins = sum(b for _, b, _ in got) // 4
    claim = cfg((28, 2741), (17, 1801))                        # mutated: the gate subtree's count
    bench.check((len(fs), n_ins) == claim, f"the machine is {len(fs)} functions / {n_ins} instructions "
                                           f"(claimed {claim[0]} / {claim[1]})")
    gate = closure([0x8005BEF4, 0x8005BF6C, 0x8005C018, 0x8005C0B0, 0x8005C140, 0x8005BE58, 0x80012858, 0x80068D20])
    lo_g, hi_g = I.extent(0x800C3E9C)
    lo_p, hi_p = I.extent(0x800C37B0)
    tot = sum(gate.values()) + (hi_g - lo_g) // 4 + (hi_p - lo_p) // 4
    bench.check(len(gate) + 2 == 17 and tot == 1801,
                f"the '17 functions, 1801 instructions' count is the GATE's subtree: 0x800C3E9C + StancePath + the "
                f"{len(gate)} functions the entry points reach ({tot} instructions) - not the machine")
    c = closure([lo for lo, _ in fs])
    outside = sorted(f for f in c if not (LO <= f < HI))
    bench.check(len(c) == 35 and sum(c.values()) == 3333 and outside == [0x80010028, 0x8001005C, 0x8001FC90, 0x8001FF3C,
                                                                       0x80066A60, 0x800710C0, 0x800714FC],
                f"closure of the range: {len(c)} functions, {sum(c.values())} instructions; outside the range only "
                f"{[hex(f) for f in outside]}")
    bad = [f for f in c if f in (RAND, GETRCNT)]
    if MUTATE["on"]:
        bad = [RAND]                                            # mutated: 'the machine draws a random number'
    jalr = sum(1 for f in c for w in words(*I.extent(f)) if (w >> 26) == 0 and (w & 0x3F) == 9)
    bench.check(not bad and jalr == 0, f"no Rand 0x8001FC58, no GetRCnt 0x80043F00, no jalr anywhere in the closure "
                                       f"({[hex(f) for f in bad]}, jalr {jalr})")
    jt = [P.word_at(0x8005B5EC + 4 * i) for i in range(6)]
    want_jt = list(JT)
    if MUTATE["on"]:
        want_jt[4], want_jt[5] = want_jt[5], want_jt[4]        # mutated: op 4 stops, op 5 skips
    bench.check(jt == want_jt and P.word_at(0x8005C468) == 0x00400008,
                "the sequencer's jump table 0x8005B5EC: ops 0, 1, 3 play, 2 jumps, 4 skips, 5 stops")
    cop = {}
    for f in c:
        for a in range(*I.extent(f), 4):
            w = P.word_at(a)
            if (w >> 26) in (0x12, 0x32, 0x3A):
                cop.setdefault(f, []).append(w)
    cmds = Counter(w for w in cop.get(0x8005D63C, []) if (w >> 25) == 0x25)
    want_c = Counter({0x4A49E012: 3, 0x4A486012: 1})
    if MUTATE["on"]:
        want_c = Counter({0x4A49E012: 4})                      # mutated: four MVMVA on IR
    bench.check(list(cop) == [0x8005D63C] and len(cop[0x8005D63C]) == 37 and cmds == want_c,
                f"GTE: only 0x8005D63C, 37 COP2 words, commands {dict((hex(k), v) for k, v in cmds.items())} "
                f"(MVMVA sf=1 RT*IR, MVMVA sf=1 RT*V0; lm=0, no translation)")
    for a, w, what in WORDS:
        want_w = cfg(w, w ^ 1) if a == 0x8005E708 else w      # mutated: a run of 5 + the code
        bench.check(P.word_at(a) == want_w, f"word at {a:#x}: {what}")
    ri = rashcdi()
    for site, name, st, sw, slot in LOADER:
        imm = ri.word(site) & 0xFFFF
        nm = slus_string(0x80050000 + imm)
        want_nm = cfg(name, "DATA\\ANIMTBS1.PSX") if site == 0x80066530 else name   # mutated: bank 0 short
        bench.check((ri.word(site) >> 16) == 0x24A5 and nm == want_nm and ri.word(st) == sw,
                    f"RASHCDI loader: {nm} -> bank slot {slot} (addiu at {site:#x}, store at {st:#x})")
    bench.check(ri.word(0x80066484) == 0x30420008 and ri.word(0x80066448) == 0x2402002C
                and ri.word(0x80066564) == 0x8C42B254,
                "RASHCDI loader: short banks when game_state+4 & 8, J3 / J banks when game_state+4 == 44, "
                "ANIMTBLP only when *(0x8005B254)")
    for site, callee in PRESENT:
        want_t = cfg(callee, 0x800C3630) if site == 0x80090CA8 else callee   # mutated: 40/88 scrubbed by pitch
        bench.check(P.jal_target(site) == want_t, f"presentation pass: jal at {site:#x} -> {want_t:#x}")
    for a, w, what in PRESENT_WORDS:
        bench.check(P.word_at(a) == w, f"presentation pass word at {a:#x}: {what}")
    sites = I.call_sites(0x8005BE58)
    fn = sorted({func_of(s) for s in sites})
    bench.check(len(sites) == 18 and len(fn) == 12, f"ClipDone 0x8005BE58: 18 call sites in 12 functions "
                                                    f"{[hex(f) for f in fn]}")
    bench.check(I.call_sites(0x8005BEF4) == [0x800C41F8], "ChannelFree 0x8005BEF4: one call site, the gate (0x800C41F8)")


_ST = []


def func_of(pc):
    if not _ST:
        I.call_sites(0)
        _ST.extend(sorted(I._CS))
    return _ST[bisect.bisect_right(_ST, pc) - 1]


# ----------------------------------------------------------------------------------------------------------- data

def images():
    st = [os.path.join(P.STATE_DIR, s, "ram.bin") for s in ("quick", "rr-race", "rr-pack", "rr-grid")]
    return st + sorted(glob.glob(os.path.join(ROOT, "work", "oracle", "vr_capture", "ramdumps", "ram_*.bin")))


def img_name(p):
    return os.path.basename(p) if "ramdumps" in p else p.split(os.sep)[-2]


class Ram:
    def __init__(self, path):
        self.b = open(path, "rb").read()

    def w(self, a):
        return struct.unpack_from("<I", self.b, a & 0x1FFFFF)[0]

    def h(self, a):
        return struct.unpack_from("<h", self.b, a & 0x1FFFFF)[0]

    def hu(self, a):
        return struct.unpack_from("<H", self.b, a & 0x1FFFFF)[0]

    def u8(self, a):
        return self.b[a & 0x1FFFFF]


_DEC = {}


def decoded():
    if not _DEC:
        wf = D.widths_first(P.word_at, byte_at)
        for n in D.FILES:
            clips = D.decode_file(n, wf)
            _DEC[n] = (D.load(n), clips, [c.track() for c in clips])
    return _DEC


def identify_banks(r):
    """{bank slot: (file, buffer)} by byte equality with the file, the loader's +0x14 relocation undone"""
    out = {}
    dec = decoded()
    for b in range(16):
        bk = r.w(BANKS + 4 * b)
        if not bk:
            continue
        buf, tab, n = r.w(bk + 8), r.w(bk + 4), r.hu(bk + 2)
        for name, (data, clips, _) in dec.items():
            if n != len(clips):
                continue
            blob = bytearray(r.b[buf & 0x1FFFFF:(buf & 0x1FFFFF) + len(data)])
            good = True
            for c in clips:
                if struct.unpack_from("<I", blob, c.off + 20)[0] != u32(buf + c.off + 24) or r.w(tab + 4 * c.index) != u32(buf + c.off):
                    good = False
                    break
                struct.pack_into("<I", blob, c.off + 20, 0x18)
            if good and bytes(blob) == data:
                out[b] = (name, buf, bk)
    return out


BANKFILE = {0: "ANIMTBL1", 1: "ANIMTBL2", 2: "ANIMTBL3", 3: "ANIMTBLW", 4: "ANIMTBLB", 5: "ANIMTBLP"}


def check_data(bench):
    dec = decoded()
    imgs = images()
    # D1: the banks
    per = {}
    for p in imgs:
        r = Ram(p)
        per[img_name(p)] = {b: v[0] for b, v in identify_banks(r).items()}
    want = dict(BANKFILE)
    if MUTATE["on"]:
        want[0] = "ANIMTBS1"                                     # mutated: the short rider bank
    bench.check(len(per) == 18 and all(v == want for v in per.values()),
                f"in all {len(per)} RAM images the six resident banks are byte-for-byte {sorted(set(want.values()))} "
                f"(bank slot order {[want[b] for b in sorted(want)]}; the loader turns +0x14 into a pointer)")
    # D2: ANIMNOIZ.DAT
    nz = open(os.path.join(DATA, "ANIMNOIZ.DAT"), "rb").read()
    res = []
    for p in imgs:
        r = Ram(p)
        tbl = r.w(0x8005B3E4)
        base = u32(tbl - cfg(0x88C, 0x880))                    # mutated: the table 12 bytes earlier
        eq = rel = other = 0
        for off in range(0, len(nz), 4):
            f, v = struct.unpack_from("<I", nz, off)[0], r.w(base + off)
            if f == v:
                eq += 1
            elif v == u32(f + base):
                rel += 1
            else:
                other += 1
        res.append((img_name(p), eq, rel, other))
    bench.check(all(o == 0 and rl == 181 for _, _, rl, o in res) and nz[:4] == b"ARec" and nz[0x884:0x888] == b"APtr",
                f"DATA\\ANIMNOIZ.DAT ({len(nz)} bytes: 'ARec' event records, 'APtr' at 0x884) is resident in all "
                f"{len(res)} images, word-equal but for 181 relocated pointers; *(0x8005B3E4) = its base + 0x88C, the "
                f"per-stance event list every play call passes as `ex`")
    # D0: the payload layout closes on every clip of every file
    nclip, badl, lefts = 0, [], Counter()
    for name, (data, clips, _) in dec.items():
        for c in clips:
            nclip += 1
            lefts[c.left] += 1
            if len(c.chans) != 3 + 4 * c.parts + cfg(0, 1) or not (0 <= c.left < 3) or c.end - c.off + c.left != c.size:
                badl.append((name, c.index))
    bench.check(nclip == 512 and not badl,
                f"DMD3 payload: all {nclip} clips of the 12 files decode to exactly 3 + 4 x parts channels and the "
                f"channel records end within 2 bytes of the block's end (bytes left {dict(lefts)}); bad {badl[:3]}")
    # D3: the stance table against the three bank configurations the loader can build
    SETS = {"long": {0: "ANIMTBL1", 1: "ANIMTBL2", 2: "ANIMTBL3", 3: "ANIMTBLW", 4: "ANIMTBLB", 5: "ANIMTBLP"},
            "short": {0: "ANIMTBS1", 1: "ANIMTBL2", 2: "ANIMTBL3", 3: "ANIMTBSW", 4: "ANIMTBSB", 5: "ANIMTBLP",
                      6: "ANIMTBLS"},
            "short-44": {0: "ANIMTBS1", 1: "ANIMTBL2", 2: "ANIMTBJ3", 3: "ANIMTBSW", 4: "ANIMTBSB", 5: "ANIMTBLP",
                         6: "ANIMTBLJ"}}
    keys = {n: [c.keys for c in dec[n][1]] for n in dec}
    bad, used, absent = [], Counter(), Counter()
    for st in range(223):
        w0 = P.word_at(0x800541D4 + 8 * st)
        bank, clip = w0 & 0xF, (w0 >> 4) & 0xFFF
        used[bank] += 1
        for nm, fam in SETS.items():
            if bank not in fam:
                absent[nm] += 1
            elif clip >= len(keys[fam[bank]]):
                bad.append((st, nm))
    if MUTATE["on"]:
        bad = [] if bad else [(0, "mutated")]
    bench.check(not bad and dict(used) == {0: 123, 1: 17, 2: 14, 6: 69} and dict(absent) == {"long": 69},
                f"stance table SLUS 0x800541D4, records 0..222: banks {dict(sorted(used.items()))}; every clip index "
                f"lies inside its bank in the long, short and short-44 sets, except that bank 6 (69 stances) is absent "
                f"from the long set; bad {bad[:3]}")
    # D4: the DMD3 decode against every sampled channel of every image
    ok = badc = mid = 0
    for p in imgs:
        r = Ram(p)
        bf = identify_banks(r)
        byptr = {v[2]: v for v in bf.values()}
        base, cnt = r.w(POOL), r.w(POOL + 12)
        for i in range(cnt):
            a = base + 0x83C * i
            if not r.w(a):
                continue
            h0 = r.h(a + 48)
            if h0 < 0 or r.w(a + 40) not in byptr:
                continue
            name, buf, _ = byptr[r.w(a + 40)]
            _, clips, tracks = dec[name]
            k = [j for j, c in enumerate(clips) if u32(buf + c.off) == r.w(a + 44)]
            if not k:
                continue
            tk, A = tracks[k[0]], clips[k[0]].keys
            nch = r.h(a + 50)

            def pair(ch, f):
                return (tk[ch][f], tk[ch][min(f + 1, A - 1)])
            got = [(r.h(a + 56 + 24 * ch), r.h(a + 58 + 24 * ch)) for ch in range(nch)]
            at_h0 = [got[ch] == pair(ch, h0) for ch in range(nch)]
            if all(at_h0):
                ok += nch
                continue
            f1 = s32(r.w(a + 16))                                # a capture taken inside Sample: a prefix is at `frame`
            at_f1 = [got[ch] == pair(ch, f1) for ch in range(nch)]
            k = next((ch for ch in range(nch) if not at_f1[ch]), nch)
            if 0 < k < nch and all(at_h0[k:]):
                mid += 1
                ok += nch
            else:
                badc += sum(1 for x in at_h0 if not x)
    bench.check(badc == 0 and ok > 20000 and mid == 1,
                f"DMD3 decode (anim_dmd3.py) == every sampled channel (key and next key) of every playing clip in 18 "
                f"images: {ok} channels, {badc} wrong; {mid} object caught inside 0x8005E5A4 (a prefix of its channels "
                f"already at `frame`, the rest at the sampled frame - ram_000300)")
    # D5: pose = QuatToMatrix of the channels
    okm = badm = 0
    for p in imgs:
        r = Ram(p)
        base, cnt = r.w(POOL), r.w(POOL + 12)
        for i in range(cnt):
            a = base + 0x83C * i
            R = r.w(a)
            if not R:
                continue
            fl, op = r.w(a + 36), r.w(a + 4) + 12 * r.w(a + 12)
            b1, b2 = r.u8(op + 1), r.u8(op + 2)
            if not (fl & 2) or b1 == 3 or (fl & 4) or r.h(a + 48) < 0:
                continue
            mask = 0xFFFFFFFF if b2 & 0x40 else r.w(a + 1760)
            mirror = b2 & 1
            typ = (r.hu(r.w(R) + 14) & 0x78) >> 3
            cls = 0
            if ((r.w(R + 36) >> 18) & 1) == 1 and r.w(R + 52):
                pass
            elif typ == 1 and r.w(R + 52):
                cls = (r.hu(r.w(r.w(R + 52)) + 14) & 0xF80) >> 7
            for k in range(r.u8(r.w(a + 44) + 15)):
                dst = r.u8(0x800CC1B0 + k) if mirror else k
                if not (mask >> dst) & 1 or dst == 0:
                    continue
                q = [r.h(a + 128 + 24 * (4 * k + j)) for j in range(4)]
                if typ in (1, 4) and cls == 1 and (r.w(0x800CC1C4) >> k) & 1:
                    q = quat_mul([r.h(0x800CC1C4 + 10 + 8 * k + 2 * j) for j in range(4)], q)
                q = AM._mirror_q(q, typ if typ in (1, 4, 5) else 0, k, mirror)
                q = [s16(v) << 2 for v in q]
                if MUTATE["on"]:
                    q = [q[1], q[0], q[2], q[3]]                  # mutated: x and y swapped
                got = [r.hu(r.w(R + 4) + 24 * dst + 4 + 2 * j) for j in range(9)]
                if quat_to_matrix(q) == got:
                    okm += 1
                else:
                    badm += 1
    bench.check(okm >= 300 and badm == 0,
                f"every posed part matrix (slots 1..16 of every unblended, uninterpolated rider in 18 images) == "
                f"QuatToMatrix(the part's four channels): {okm} equal, {badm} not - the pose is 1.14 QUATERNIONS, "
                f"not Euler angles")
    # D6: FIGHT.BIN hit frames against the clips. FightStep tests `frame >= hitFrame` only while ClipDone is false,
    # i.e. while frame < keys - 1 (a hard start plays keys 0..keys-1): hitFrame >= keys - 1 can never strike.
    fb = open(os.path.join(DATA, "FIGHT.BIN"), "rb").read()
    L = {0: "ANIMTBL1", 1: "ANIMTBL2", 2: "ANIMTBL3", 3: "ANIMTBLW", 4: "ANIMTBLB", 5: "ANIMTBLP"}
    S = {0: "ANIMTBS1", 1: "ANIMTBL2", 2: "ANIMTBL3", 3: "ANIMTBSW", 4: "ANIMTBSB", 5: "ANIMTBLP", 6: "ANIMTBLS"}
    c, never_recs = Counter(), set()
    for rec in range(40):
        _, _, nb, _, ob = struct.unpack_from("<HBBII", fb, 12 * rec)
        for k in range(nb):
            st, _, _, _, dmg, _, hf = struct.unpack_from("<HHHHHBB", fb, ob + 12 * k)
            if dmg == 0:
                continue
            c["nodes"] += 1
            w0 = P.word_at(0x800541D4 + 8 * st)
            bank, clip = w0 & 0xF, (w0 >> 4) & 0xFFF
            for var, M in (("L", L), ("S", S), ("J", SETS["short-44"])):
                if bank not in M:
                    c[(var, "no bank")] += 1
                    continue
                A = keys[M[bank]][clip]
                ok_ = hf <= A - 2
                c[(var, ok_)] += 1
                if var == "S" and not ok_:
                    never_recs.add(rec)
    want = cfg((57, 27, 60, 24, 58, 26), (84, 0, 84, 0, 84, 0))  # mutated: 'every strike can land in every set'
    got = (c[("L", True)], c[("L", "no bank")], c[("S", True)], c[("S", False)], c[("J", True)], c[("J", False)])
    bench.check(c["nodes"] == 84 and c[("L", False)] == 0 and got == want,
                f"FIGHT.BIN: of {c['nodes']} striking nodes, {got[0]} can land in the long bank set (hitFrame <= keys - 2 "
                f"of the clip their animStart stance plays) and {got[1]} play bank-6 (passenger) stances, absent there; "
                f"in the short set {got[2]} can and {got[3]} never can - hitFrame >= keys - 1, where ClipDone is true "
                f"before FightStep's strike test (records {sorted(never_recs)}); in the short-44 set {got[4]} / {got[5]}")


def quat_mul(p, q):
    px, py, pz, pw = p
    qx, qy, qz, qw = q
    w = s32(u32(pw * qw - px * qx - py * qy - pz * qz)) >> 14
    x = s32(u32(pw * qx + px * qw + py * qz - pz * qy)) >> 14
    y = s32(u32(pw * qy + py * qw + pz * qx - px * qz)) >> 14
    z = s32(u32(pw * qz + pz * qw + px * qy - py * qx)) >> 14
    return [s16(x), s16(y), s16(z), s16(w)]


def quat_to_matrix(q):
    """SLUS 0x8001005C, as anim_models.m_1005c computes it, returning the nine u16 stores in matrix order"""
    fm = lambda p, r: s32(u32((p * r) >> 16))
    x, y, z, w = q
    n = u32(fm(x, x) + fm(y, y) + fm(z, z) + fm(w, w))
    s = s32(u32((((0x20000 // n) & 0xFFFFFFFF) << 16) | ((u32((0x20000 % n) << 16) // n) & 0xFFFF)))
    xs, ys, zs = fm(x, s), fm(y, s), fm(z, s)
    wx, yz, xx, yy, zz = fm(w, xs), fm(y, zs), fm(x, xs), fm(y, ys), fm(z, zs)
    wz, xy, wy, xz = fm(w, zs), fm(x, ys), fm(w, ys), fm(x, zs)
    M = {0: 0x10000 - (yy + zz), 2: xy - wz, 4: xz + wy, 6: xy + wz, 8: 0x10000 - (xx + zz), 10: yz - wx,
         12: xz - wy, 14: wx + yz, 16: 0x10000 - (xx + yy)}
    return [(s32(u32(M[o])) >> 4) & 0xFFFF for o in range(0, 18, 2)]


# ----------------------------------------------------------------------------------------------------------- live

WATCH = list(P.WATCHES) + [(0x801EE000, 0xD000, "anim"), (0x801E7000, 0x4000, "parts"), (0x1F800000, 0x400, "spad")]
MODEL_FUNCS = [f for f, _, _, _ in AM.MODELS]
EXTRA_PROBES = [0x800C09B0, 0x800C0470 + 8, 0x800C05E8 + 8, 0x800C1464 + 8, 0x800C3E9C, 0x800C4574 + 8,
                0x8008AB00, 0x80091468]
# entered in no run (asserted, like impact.NOT_RUN): Stop is RiderDismount's; StopIfPlaying / Resume are the
# pedestrian code's (0x800CB58C / 0x800CB5CC). RangedStart is not in this set: the interpreter models the MDEC, so
# launch40 runs past the MDEC status read 0x1F801824 and calls RangedStart once from 0x800B259C (a0 0x800CF5D8,
# clip 6, flags 0, first 4), and its model is compared like any other.
NOT_RUN = {0x8005BE0C, 0x8005BE20, 0x8005BE44}

RUNS = [("rr-plain", I.RR, None, 30, "0xFFFF", False, "rr-race untouched: four looping clips, the player's stopped"),
        ("quick-plain", I.QUICK, None, 20, "0xFFFF", False, "quick untouched: transitions, a loop start")]
for _n in ("crash", "punch-cop", "knock-cop", "stance-l1", "ko-forced", "launch38", "launch39", "launch40",
           "launch41", "launch42", "launch90"):
    RUNS.append(next(r for r in I.RUNS if r[0] == _n))


def forced_fall(bike, req, fc, words):
    """quick: a FORCED knock-off request on bike k's rider (+0x228 |= req) with the bike's flagsC |= fc (impact's
    knockoff_requests), and bike words FORCED (bike + off := value) - none of it a placement"""
    def f(ram):
        return I.knockoff_requests({bike: (req, fc)})(ram) + [(I.BIKE(bike) + o, 4, u32(v)) for o, v in words]
    return f


RUNS += [("pitch38", I.QUICK, forced_fall(9, 0x8000, 0x100, [(0x28C, I.FX(1.5))]), 10, "0xFFFF", False,
          "bike 9's rider knocked off with flagsC bit 8 (stance 38), the bike's +0x28C FORCED to 1.5: "
          "FallScrubPitch ends the fall at once")]


def stack_index(tr):
    d = {}
    for sq, pc, a, n, v in tr.writes:
        if 0x801FE000 <= a < 0x80200000 or 0x1F800000 <= a < 0x1F800400:
            for k in range(n):
                d.setdefault(a + k, []).append((sq, (v >> (8 * k)) & 0xFF))
    return d


STAT = Counter()


def install_stack_check(tr):
    idx = stack_index(tr)

    def chk(m, target, ad, n):
        if m.ci >= len(m.gcalls):
            return
        seq = m.gcalls[m.ci][0]
        for k in range(n):
            a = u32(ad + k)
            L = idx.get(a, [])
            i = bisect.bisect_right(L, (seq + 1, 999)) - 1
            g = L[i][1] if i >= 0 else tr.ram0[a & 0x1FFFFF]
            if g != m.mem[a & 0x1FFFFF]:
                raise P.Mismatch(f"call #{m.ci + 1} {target:#x}: frame buffer byte {a:#x} model "
                                 f"{m.mem[a & 0x1FFFFF]:#x} guest {g:#x}")
            STAT["buffer bytes"] += 1
    orig = P.Machine.__init__

    def init(self, *a, **k):
        orig(self, *a, **k)
        self.stack_check = chk
    P.Machine.__init__ = init
    return orig


def do_run(spec):
    name, src, build, frames, pad, explore, note = spec
    srcdir = os.path.join(P.STATE_DIR, src)
    ram0 = open(os.path.join(srcdir, "ram.bin"), "rb").read()
    edits = build(ram0) if build else []
    os.makedirs(WORK, exist_ok=True)
    keep = I.WORK
    I.WORK = WORK                                              # impact.prepare writes under its module WORK
    try:
        st, ram = I.prepare("run-" + name, srcdir, edits)
    finally:
        I.WORK = keep
    out = os.path.join(WORK, "tr-" + name)
    out, stop, so = I.run_trace("run-" + name, st, frames, MODEL_FUNCS, pad=pad, watches=WATCH, explore=explore,
                                callee_entries=True, probes=EXTRA_PROBES, out=out)
    return I.load_trace(out, ram), stop, edits, st, out


def check_live(bench, only=None, keep=False, verbose=True):
    agg = {f: Counter() for f in MODEL_FUNCS}
    facts = {}
    watched = I.watched_in(WATCH)
    for spec in RUNS:
        if only and spec[0] not in only:
            continue
        tr, stop, edits, st, out = do_run(spec)
        orig = install_stack_check(tr)
        line = []
        try:
            for f, name, frame, model in AM.MODELS:
                res = I.run_model(tr, model, f, frame, watched, strict=True)
                okc = sum(1 for r in res if r[1] is True)
                badr = [r for r in res if r[1] is False]
                agg[f]["calls"] += okc + len(badr)
                agg[f]["match"] += okc
                if res:
                    line.append(f"{name} {okc}/{okc + len(badr)}")
                for r in badr[:2]:
                    print(f"    MISMATCH {name} {spec[0]} seq {r[0]}: {r[2]}")
        finally:
            P.Machine.__init__ = orig
        facts[spec[0]] = run_facts(spec[0], tr)
        if verbose:
            print(f"  {spec[0]} ({spec[1]}, {len(edits)} edits, {spec[3]} frames, pad {spec[4]}): {(stop or ['?'])[0][:60]}")
            print("    " + ", ".join(line))
        if not keep:
            shutil.rmtree(out, ignore_errors=True)
            shutil.rmtree(st, ignore_errors=True)
    for f, name, frame, model in AM.MODELS:
        a = agg[f]
        if only and not a["calls"]:
            continue
        if f in NOT_RUN:
            bench.check(a["calls"] == cfg(0, -1), f"{name} {f:#x}: entered in no run ({a['calls']} calls) - "
                                                  f"transcription only")
            continue
        bench.check(a["calls"] > 0 and a["match"] == a["calls"], f"{name} {f:#x}: model == interpreter on "
                                                                  f"{a['match']}/{a['calls']} calls")
    if verbose:
        print(f"  frame-buffer bytes compared behind pointer arguments: {STAT['buffer bytes']}; register arguments "
              f"compared at the callee's entry probe: {I.ARGSTAT['entry']}, left uncompared: {I.ARGSTAT['unchecked']}")
    return facts


# ---------------------------------------------------------------------------------------------------------- facts

def run_facts(name, tr):
    out = {}
    gsp = struct.unpack_from("<I", tr.ram0, 0x5B2F8)[0]
    # dt: every AdvanceFrame call's a1 against game_state+0x1C at that moment
    seqs = [s for s, _ in tr.probes.get(0x8005CB04, [])]
    snaps = P.snapshots(tr, seqs[:400]) if seqs else {}
    dts = []
    for s, r in tr.probes.get(0x8005CB04, [])[:400]:
        g = struct.unpack_from("<i", snaps[s], (gsp + 0x1C) & 0x1FFFFF)[0]
        dts.append((r["a1"], u32(g)))
    out["dt"] = dts
    # ChannelFree returns and the gate's answer right after
    out["free"] = []
    for s, r in tr.probes.get(0x8005BEF4, []):
        p = tr.post(0x800C41F8 + 8, s, r["sp"])
        out["free"].append(p[1]["v0"] if p else None)
    out["gate"] = []
    for s, r in tr.probes.get(0x800C3E9C, []):
        p = tr.post(0x800C4574 + 8, s, r["sp"])
        if p:
            out["gate"].append((s, p[1]["v0"]))
    # frozen objects: at AnimationPass entry, playing objects with flags bit 3, and whether AdvanceFrame ran on them
    froz = skipped = 0
    passes = [s for s, _ in tr.probes.get(0x8005E1D8, [])]
    adv = sorted((s, r["a0"]) for s, r in tr.probes.get(0x8005CB04, []))
    snaps = P.snapshots(tr, passes) if passes else {}
    for k, s in enumerate(passes):
        ram = snaps[s]
        base, cnt = struct.unpack_from("<I", ram, POOL & 0x1FFFFF)[0], struct.unpack_from("<I", ram, (POOL + 12) & 0x1FFFFF)[0]
        nxt = passes[k + 1] if k + 1 < len(passes) else 1 << 62
        ran = {a for sq, a in adv if s < sq < nxt}
        for i in range(cnt):
            A = base + 0x83C * i
            f = struct.unpack_from("<I", ram, (A + 36) & 0x1FFFFF)[0]
            if f & 2 and f & 8:
                froz += 1
                skipped += A not in ran
    out["frozen"] = (froz, skipped)
    # the fight's strike: FightStep returning 1 against the rider's clip frame and the node's hitFrame
    strikes = []
    rets = [0x800C0470 + 8, 0x800C05E8 + 8, 0x800C1464 + 8]
    ents = tr.probes.get(0x800C09B0, [])
    sn = P.snapshots(tr, [s for s, _ in ents]) if ents else {}
    for s, r in ents:
        ex = None
        for rs in rets:
            p = tr.post(rs, s, r["sp"])
            if p and (ex is None or p[0] < ex[0]):
                ex = p
        if not ex:
            continue
        ram = sn[s]
        rd = lambda a, f="<I": struct.unpack_from(f, ram, a & 0x1FFFFF)[0]
        rider = rd(r["a0"] + 0x354)
        A = rd(rider + 0x21C)
        n = ram[(rider + 0x23E + ram[(rider + 0x23A) & 0x1FFFFF]) & 0x1FFFFF]
        rec = ram[(rider + 0x239) & 0x1FFFFF]
        fight = rd(0x8005AD4C)
        hitf = ram[(rd(fight + 12 * rec + 8) + 12 * n + 11) & 0x1FFFFF] if n != 63 and rec < 40 else None
        strikes.append((ex[1]["v0"], rd(A + 16, "<i"), hitf, ram[(rider + 0x222) & 0x1FFFFF], rd(A + 36)))
    out["strikes"] = strikes
    # Rand / GetRCnt called from inside the range
    out["rand"] = sum(1 for c in tr.calls if c[2] in (RAND, GETRCNT) and LO <= c[1] < HI)
    out["scrub"] = [(pc, a) for sq, pc, a, n, v in tr.writes if pc in (0x800C3308 + 4, 0x800C33A8, 0x800C354C,
                                                                       0x800C35C0, 0x800C3700, 0x800C3754)]
    return out


def check_facts(bench, facts):
    allf = list(facts.items())
    dts = [x for _, f in allf for x in f["dt"]]
    vals = sorted({a for a, _ in dts})
    bench.check(dts and all(a == g for a, g in dts) and all(a % cfg(5, 10) == 0 and 5 <= a <= 30 for a in vals),
                f"time base: every AdvanceFrame gets dt = game_state+0x1C ({len(dts)} calls; values {vals}: 5 ticks a "
                f"vblank - 10 at 30 frames a second, 15 on a three-vblank frame); a clip advances floor((acc + dt) / "
                f"rate) keys")
    if "punch-cop" in facts:
        f = facts["punch-cop"]
        busy = [v for v in f["free"] if v == 0]
        bench.check(len(busy) >= cfg(1, 99), f"punch-cop: ChannelFree said 'busy' {len(busy)} time(s) of "
                                             f"{len(f['free'])}; the gate refused those events (returns {sorted(set(v for _, v in f['gate']))})")
        st = f["strikes"]
        hits = [x for x in st if x[0] == 1]
        good = all(fr >= hf and f222 == 0 for _, fr, hf, f222, _ in hits if hf is not None)
        miss = [x for x in st if x[0] == 0 and x[2] is not None and x[1] >= x[2] and x[3] == 0 and x[4] & 2]
        bench.check(hits and good and len(hits) == cfg(len(hits), 0),
                    f"punch-cop: FightStep returned 1 {len(hits)} time(s), each with the clip frame at or past the "
                    f"node's hitFrame and the strike byte clear (frame, hitFrame) {[(x[1], x[2]) for x in hits]}; "
                    f"{len(st)} FightStep calls")
    fz = [f["frozen"] for _, f in allf]
    tf, ts = sum(a for a, _ in fz), sum(b for _, b in fz)
    bench.check(tf > 0 and ts == tf - cfg(0, 1), f"frozen clips (flags bit 3, set by the clock writers): {tf} playing "
                                                 f"object-passes, AdvanceFrame skipped on {ts}")
    sc = sorted({pc for _, f in allf for pc, _ in f["scrub"]})
    bench.check(len(sc) >= 3, f"the clock writers set bit 3 in the launch runs (store pcs {[hex(x) for x in sc]})")
    rc = sum(f["rand"] for _, f in allf)
    bench.check(rc == cfg(0, 1), f"no call to Rand or GetRCnt from inside the range in any run ({rc})")


# ----------------------------------------------------------------------------------------------------------- main

def main(argv):
    ap = argparse.ArgumentParser(prog="anim.py")
    ap.add_argument("cmd", choices=["info", "live", "verify"])
    ap.add_argument("--mutate", action="store_true")
    ap.add_argument("--only", default=None, help="comma-separated run names")
    ap.add_argument("--keep", action="store_true", help="keep snapshot copies and traces under work\\anim\\probe")
    a = ap.parse_args(argv)
    MUTATE["on"] = a.mutate
    AM.MUT["on"] = a.mutate
    D.MUT["on"] = a.mutate
    bench = Bench()
    print("skeleton:")
    check_skeleton(bench)
    print("data:")
    check_data(bench)
    if a.cmd in ("live", "verify"):
        print("live:")
        facts = check_live(bench, only=set(a.only.split(",")) if a.only else None, keep=a.keep)
        print("facts:")
        check_facts(bench, facts)
    print(f"anim: {bench.n} checks, {bench.fail} failures")
    return 1 if (a.cmd == "verify" and bench.fail and not a.mutate) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
