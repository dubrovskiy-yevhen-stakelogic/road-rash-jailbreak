"""Scout probe for THE ENGINE NOTE of Road Rash: Jailbreak (USA, SLUS_01053).

The vsync tick RETUNES five looped engine voices. The two per-frame functions that CREATE those
voices and set the ramp targets are `SLUS 0x800169D0` (the engine note) and `SLUS 0x80016E4C`
(the road / skid layer).  This probe
checks the transcription of both - plus their one-time initialiser `SLUS 0x800167A4` and the
vsync tick `SLUS 0x80019990` they feed - three independent ways:

  * `static`  - the call skeleton, the callee sets, the unique call sites, the constants and
                the field offsets are re-derived from the instruction words of the player's own
                EXE (SHA-1 checked);
  * `banks`   - the sub-loop structure the code's loop-address writes select is found in the
                player's own `DATA\\RASHNZ_E.DAT` (engine banks = file entries 3, 4, 5);
  * `snap`    - the per-bike setup values (`E+0x2C`, `+0x3C`, `+0x44`, `+0x48`, `+0x50`, the
                gear table at `0x800D6BD8`) are recomputed from the bike's stat block in every
                savestate, and the voices the initialiser created are recognised by the pitch
                they were started with;
  * `trace` + `replay` - the in-process interpreter runs real frames from a savestate with a
                watch on every word these functions touch and a pc probe on every callee; the
                replay then runs a Python transcription of each function against that record.
                Every load of the model must be a load the guest made (same address, same
                size, per-address order) and takes the guest's value; every store the model
                makes must equal the guest's stores in order; every call must go to the same
                site with the same register arguments.  Nothing the guest read may be left
                unconsumed.

`--mutate` perturbs load-bearing claims (in the static table and in the replay model) and
must turn the verdict into failures.

Reads ONLY work\\disc_us, work\\oracle\\state and the trace directories this probe itself
writes under work\\sound\\engine_note (all gitignored).  Never copies game bytes into the repo.

Usage (from the project root):

    python tools\\scout\\engine_note.py static
    python tools\\scout\\engine_note.py banks
    python tools\\scout\\engine_note.py snap
    python tools\\scout\\engine_note.py trace --name race_thr --state rr-race --pad 0xBFFF
    python tools\\scout\\engine_note.py trace --name race_init --state rr-race --init-engine
    python tools\\scout\\engine_note.py replay --name race_thr [--mutate]
    python tools\\scout\\engine_note.py verify [--mutate]

`verify` ends with a single line a gate can match:

    engine_note: <n> checks, <m> failures
"""

from __future__ import annotations

import argparse
import csv
import os
import shutil
import struct
import subprocess
import sys
from collections import defaultdict, deque

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import exe as EX  # noqa: E402

ROOT = EX.ROOT
DISC = EX.DISC
STATE_DIR = os.path.join(ROOT, "work", "oracle", "state")
OUT_DIR = os.path.join(ROOT, "work", "sound", "engine_note")
STATES = ("rr-race", "rr-pack", "rr-grid", "quick")
SLUS_SHA1 = "67ed165a2c517d4e6106fb0dfa324d66dd9a76f1"
RRVERIFY = os.path.join(ROOT, "build_m0", "rrverify.exe")

GP = 0x8005AC8C
G_ENGINE = GP + 1952          # 0x8005B42C -> EngineRec[numPlayers], 132 bytes each
G_ROAD_BANK = GP + 1940       # 0x8005B420  bank of the road/skid layer (1 in every capture)
G_ROAD_MUTE = GP + 1960       # 0x8005B434  1 = road/skid layer silenced
G_TOGGLE = GP + 1888          # 0x8005B3EC  which player the vsync tick services
GAME_STATE_PTR = 0x8005B2F8
AUDIO_GATE = 0x8005ACA8
GEAR_TAB = 0x800D6BD8         # s32[10], rebuilt per bike by 0x8001654C
SLIDERS = 0x800D6C00          # s32[7]; [0] = engine volume, [3] = effects volume
PARAMS_BASE = 0x800525F4      # 20-byte engine parameter records, one per 9 bike models

F_NOTE = 0x800169D0           # EngineNote(p)
F_ROAD = 0x80016E4C           # RoadNote(p)
F_START = 0x800167A4          # EngineStart(p)
F_TICK = 0x80019990           # AudioVSyncTick()
F_AUDIOFRAME = 0x80018FAC

FIXMUL = 0x8001FC90
RAND = 0x8001FC58
GETRCNT = 0x80043F00
S3D = 0x80019E40
GSP = 0x8001F934              # GetSoundPitch(bank, sound, desc)
SV = 0x8001F174               # StartVoice(bank, sound, reverbOff, reserved, SoundParams*)
STOPV = 0x8001F7EC            # StopVoice(handle)
SETLOOP = 0x8001F874          # SetVoiceLoopOffset(byteOffset, handle)
PMOD = 0x8001F900             # SetVoicePitchMod(handle, on)
UV = 0x80019C54               # UpdateVoice(handle, SoundParams*)
UV2 = 0x8001F6A4              # the same 328 bytes, a second copy
PS3D = 0x80017BA0
FX1 = 0x80027778
FX2 = 0x80027974
SVC = 0x8001EE94

# function extents [start, jr-ra pc] and the prologue/epilogue pcs that spill to the watched
# part of the stack frame (excluded: they save/restore callee-saved registers, not data)
FUNCS = {
    "note": (F_NOTE, 0x80016E44, 96, ()),
    "road": (F_ROAD, 0x80017694, 96, (0x80016E8C, 0x80017690)),
    "start": (F_START, 0x800169C8, 88, (0x800167C8, 0x800169C4)),
    "tick": (F_TICK, 0x80019C4C, 56, ()),
}
CALLEES = (FIXMUL, RAND, GETRCNT, S3D, GSP, SV, STOPV, SETLOOP, PMOD, UV, UV2, PS3D, FX1, FX2,
           SVC, F_START)


def s32(x):
    x &= 0xFFFFFFFF
    return x - (1 << 32) if x & 0x80000000 else x


def u32(x):
    return x & 0xFFFFFFFF


def mul(a, b):                 # mult + mflo: the low 32 bits, signed
    return s32(s32(a) * s32(b))


def sra(x, n):
    return s32(x) >> (n & 31)


def absw(x):                   # sra 31 / addu / xor
    x = s32(x)
    m = x >> 31
    return s32((x + m) ^ m)


def clamp127(x):               # the branchless clamp the compiler emits: max(x,0) + min(127-x,0)
    x = s32(x)
    a = x & ~(x >> 31)
    t = s32(127 - x)
    return s32(a + ((t >> 31) & t))


def fixmul(a, b):              # SLUS 0x8001FC90: bits 16..47 of the 64-bit product
    return s32((s32(a) * s32(b)) >> 16)


# ---------------------------------------------------------------------------
# Claims.  --mutate perturbs each of these; the static table and the replay model both
# read them from here, so a perturbed claim has to be caught by the evidence.
# ---------------------------------------------------------------------------

def claims(mutate: str | None) -> dict:
    c = {
        "stride": 132, "idle": 896, "jitA": 101, "jitB": 11, "shift": 512, "blip": 96,
        "blipClamp": 126, "rateShift": 1, "engineSlider": SLIDERS, "fxSlider": SLIDERS + 12,
        "tickPitchClamped": True, "lsaxShift": 7, "reverbReg": 204, "pmonReg": 200,
        "modStart": 128, "gearTab": GEAR_TAB,
    }
    if mutate:
        m = {
            "stride": ("stride", 136), "idle": ("idle", 897), "jitA": ("jitA", 100),
            "rateShift": ("rateShift", 2), "slider": ("engineSlider", SLIDERS + 12),
            "tickPitch": ("tickPitchClamped", False), "lsax": ("lsaxShift", 8),
            "reverb": ("reverbReg", 205), "pmon": ("pmonReg", 202), "mod": ("modStart", 0x80 + 1),
        }
        if mutate == "all":
            for k, v in m.values():
                c[k] = v
        else:
            k, v = m[mutate]
            c[k] = v
    return c


MUTATIONS = ("stride", "idle", "jitA", "rateShift", "slider", "tickPitch", "lsax", "reverb",
             "pmon", "mod")


class Bench:
    def __init__(self, quiet=False):
        self.n = 0
        self.bad = []
        self.quiet = quiet

    def check(self, cond, what, detail=""):
        self.n += 1
        if not cond:
            self.bad.append(what)
            if not self.quiet:
                print("  FAIL  %s%s" % (what, ("  [%s]" % detail) if detail else ""))
        return bool(cond)


# ---------------------------------------------------------------------------
# static: the instruction words
# ---------------------------------------------------------------------------

def load_slus():
    img = EX.load_exe()
    return img


def w_at(img, a):
    return struct.unpack_from("<I", img.data, a - img.base)[0]


def jal_t(w):
    return 0x80000000 | ((w & 0x03FFFFFF) << 2) if (w >> 26) == 3 else None


def imm16(w):
    return s32((w & 0xFFFF) << 16) >> 16


def jal_targets(img, lo, hi):
    out = []
    for a in range(lo, hi, 4):
        t = jal_t(w_at(img, a))
        if t is not None:
            out.append((a, t))
    return out


def all_jal_sites(images, target):
    out = []
    for name, img in images:
        n = len(img.data) // 4
        for i, (w,) in enumerate(struct.iter_unpack("<I", img.data[: n * 4])):
            if jal_t(w) == target:
                out.append((name, img.base + 4 * i))
    return out


def cmd_static(args, b: Bench | None = None, cfg=None):
    own = b is None
    b = b or Bench()
    cfg = cfg or claims(getattr(args, "mutate_name", None))
    img = load_slus()
    print("SLUS_010.53 sha1 %s" % img.sha1)
    b.check(img.sha1 == SLUS_SHA1, "SLUS sha1")
    ovl = [("SLUS", img)] + [(n, EX.load_overlay(n, 0x8005B5E8)) for n in
                             ("RASHCDF.BIN", "RASHCDG.BIN", "RASHCDI.BIN")]

    # the only callers
    for tgt, want in ((F_NOTE, [("SLUS", 0x80019204)]), (F_ROAD, [("SLUS", 0x8001920C)]),
                      (F_START, [("SLUS", 0x80016E24)]), (F_AUDIOFRAME, [("SLUS", 0x800121B8)])):
        got = all_jal_sites(ovl, tgt)
        b.check(got == want, "the only call site of 0x%08X is %s" % (tgt, want), str(got))

    # extents: prologue and single jr ra
    for name, (lo, jr, frame, _) in FUNCS.items():
        b.check((0x27BD0000 | (-frame & 0xFFFF)) in [w_at(img, lo + 4 * i) for i in range(3)],
                "%s 0x%08X opens addiu sp,-%d" % (name, lo, frame))
        b.check(w_at(img, jr) == 0x03E00008, "%s ends with jr ra at 0x%08X" % (name, jr))
        jrs = [a for a in range(lo, jr, 4) if w_at(img, a) == 0x03E00008]
        b.check(not jrs, "%s has no earlier jr ra" % name, str(jrs))
    b.check(0x80016E4C - F_NOTE == 1148 and 0x8001769C - F_ROAD == 2128,
            "sizes: 0x800169D0 is 1148 bytes, 0x80016E4C is 2128 bytes")
    b.check(w_at(img, 0x8001769C) & 0xFFFF0000 == 0x27BD0000, "0x8001769C opens the next function")

    # callee sets, in site order
    want = {
        "note": [(0x80016A14, FIXMUL), (0x80016A80, FIXMUL), (0x80016AB4, FIXMUL),
                 (0x80016B04, S3D), (0x80016B88, GSP), (0x80016BBC, SV), (0x80016BC8, STOPV),
                 (0x80016C40, SV), (0x80016C4C, STOPV), (0x80016C78, GETRCNT),
                 (0x80016CA8, GETRCNT), (0x80016D2C, SETLOOP), (0x80016D6C, SETLOOP),
                 (0x80016E24, F_START)],
        "road": [(0x8001708C, PS3D), (0x800171AC, FIXMUL), (0x800171B4, RAND),
                 (0x80017258, FIXMUL), (0x80017260, RAND), (0x800173E4, GSP),
                 (0x80017430, STOPV), (0x80017450, SV), (0x80017470, UV2), (0x800174B8, S3D),
                 (0x80017524, GSP), (0x80017564, STOPV), (0x80017584, SV), (0x800175A4, UV2),
                 (0x800175CC, FX1), (0x80017634, FX2), (0x8001766C, FX2)],
        "start": [(0x80016804, S3D), (0x8001687C, SV), (0x8001689C, SV), (0x800168AC, PMOD),
                  (0x800168D0, SV), (0x800168F8, STOPV), (0x80016904, STOPV),
                  (0x80016910, PMOD), (0x8001695C, SV)],
        "tick": [(0x80019ADC, UV), (0x80019B3C, GSP), (0x80019B64, UV), (0x80019BD4, UV),
                 (0x80019C18, UV), (0x80019C30, SVC)],
    }
    for name, (lo, jr, _, _) in FUNCS.items():
        got = jal_targets(img, lo, jr + 8)
        b.check(got == want[name], "%s: the exact call list" % name,
                str([(hex(a), hex(t)) for a, t in got]))

    # the record stride 132 = ((p<<5)+p)<<2 in all four functions
    for lo in (F_NOTE, F_ROAD, F_START, 0x800199E4):
        ws = [w_at(img, lo + 4 * i) for i in range(12)]
        sll5 = any((w & 0xFC00003F) == 0 and ((w >> 6) & 31) == 5 for w in ws)
        sll2 = any((w & 0xFC00003F) == 0 and ((w >> 6) & 31) == 2 for w in ws)
        b.check(sll5 and sll2 and cfg["stride"] == 33 * 4,
                "record stride 132 = (33p)<<2 at 0x%08X" % lo)

    # constants, each at the instruction that carries it
    def imm_at(a):
        return imm16(w_at(img, a))
    table = [
        (0x80016B3C, cfg["idle"], "slti s2,896: the idle floor of the level target"),
        (0x80016B54, cfg["idle"], "li s2,896 (delay slot, both arms)"),
        (0x80016D94, cfg["shift"], "+512 on an upshift"),
        (0x80016DA0, -cfg["shift"], "-512 on a downshift"),
        (0x80016DA4, cfg["blip"], "+96 load blip on a downshift"),
        (0x80016DB0, cfg["blipClamp"], "clamped to 126, not 127"),
        (0x80016A68, cfg["gearTab"] - 0x800D0000, "gear table 0x800D6BD8"),
        (0x80016B14, cfg["engineSlider"] - 0x800D0000, "engine volume slider 0x800D6C00"),
        (0x80017398, cfg["fxSlider"] - 0x800D0000, "road layer uses the effects slider 0x800D6C0C"),
        (0x800168BC, cfg["modStart"], "the modulator voice starts at pitch 128"),
    ]
    for a, v, what in table:
        b.check(imm_at(a) == v, what, "0x%08X imm %d" % (a, imm_at(a)))
    # the two jitter multipliers are shift chains: 101x = (((3x)<<3)+x)<<2)+x, 11x = ((3x)<<2)-x
    b.check([((w_at(img, a) >> 6) & 31) for a in (0x80016C84, 0x80016C8C, 0x80016C94, 0x80016C9C)]
            == [1, 3, 2, 8] and cfg["jitA"] == ((3 * 8 + 1) * 4 + 1),
            "jitter A = (101*(RCnt&0xFF))>>8")
    b.check([((w_at(img, a) >> 6) & 31) for a in (0x80016CB4, 0x80016CBC, 0x80016CC4)] == [1, 2, 8]
            and w_at(img, 0x80016CC0) == 0x00621823 and cfg["jitB"] == 3 * 4 - 1,
            "jitter B = (11*(RCnt&0xFF))>>8")
    # the ramp rates are half the distance: sra 1 at 0x80016E0C and 0x80016E14
    b.check(all((w_at(img, a) & 0xFFE0003F) == 0x00000003 and ((w_at(img, a) >> 6) & 31) ==
                cfg["rateShift"] for a in (0x80016E0C, 0x80016E14)), "ramp rate = (target-level)>>1")
    # loop-offset writes: sll a0,a0,7 in the delay slot of both SETLOOP calls
    for a in (0x80016D30, 0x80016D70):
        b.check((w_at(img, a) & 0xFFFFF83F) == 0x00042000 and ((w_at(img, a) >> 6) & 31) ==
                cfg["lsaxShift"], "loop offset = sel << 7 at 0x%08X" % a)
    # 0x8001F874 writes SpuVoiceAttr.mask = 0x10000 (LSAX) and attr+0x20 (loop address)
    b.check(w_at(img, 0x8001F8D4) == 0x3C020001 and w_at(img, 0x8001F8E0) == 0xAFA30030,
            "0x8001F874 sets attr mask 0x00010000 and attr+0x20 = desc->spuAddr + offset")
    # libspu _SpuSetAnyVoice(on, mask, reg, reg+1): reverb (EON) and pitch-mod (PMON)
    b.check(imm_at(0x80050680) == cfg["reverbReg"] and imm_at(0x80050688) == 205,
            "0x80050678 -> 0x800506A8(on, mask, 204, 205): 0x1F801C00+2*204 = 0x1F801D98 (EON)")
    b.check(imm_at(0x80051090) == cfg["pmonReg"] and imm_at(0x80051098) == 201,
            "0x80051088 -> 0x800506A8(on, mask, 200, 201): 0x1F801C00+2*200 = 0x1F801D90 (PMON)")
    b.check(jal_t(w_at(img, 0x8001F91C)) == 0x80051088, "0x8001F900 calls 0x80051088")
    b.check(jal_t(w_at(img, 0x8001F858)) == 0x80050678 and w_at(img, 0x8001F854) == 0x02002021 and
            imm_at(0x8001F838) == 1,
            "StopVoice 0x8001F7EC calls 0x80050678(1, 1<<ch) after KeyOff and ReleaseVoice")
    b.check(jal_t(w_at(img, 0x8001F840)) == 0x8001EB44 and jal_t(w_at(img, 0x8001F848)) == 0x8001FB58,
            "StopVoice: KeyOff 0x8001EB44, ReleaseVoice 0x8001FB58")
    # the second UpdateVoice is word-identical to the first
    diff = [i for i in range(82) if w_at(img, UV + 4 * i) != w_at(img, UV2 + 4 * i)]

    def jt(w, at):
        return (at & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
    same = diff == [0xD8 // 4] and all(
        (w_at(img, f + 0xD8) >> 26) == 2 and jt(w_at(img, f + 0xD8), f + 0xD8) == f + 0x100
        for f in (UV, UV2))
    b.check(same, "0x8001F6A4 is UpdateVoice 0x80019C54 again: 82 words, equal but for the one"
            " absolute `j` at +0xD8, which targets +0x100 in both", str(diff))
    # the vsync tick reads the CLAMPED level for pitch: lw v0,48(s0) at 0x80019A88 comes
    # after sw a2,48(s0) at 0x80019A40
    b.check(w_at(img, 0x80019A40) == 0xAE060030 and w_at(img, 0x80019A88) == 0x8E020030 and
            cfg["tickPitchClamped"], "tick: pitch uses the stored (clamped) level")
    b.check(w_at(img, 0x80019A4C) == 0x02401821 and w_at(img, 0x80019AC0) == 0x02720018,
            "tick: layer-0 volume uses the UNclamped second level (s2 = f60+f68)")
    if own:
        print("engine_note static: %d checks, %d failures" % (b.n, len(b.bad)))
    return b


# ---------------------------------------------------------------------------
# banks: the sub-loop layout the loop-address writes select
# ---------------------------------------------------------------------------

def cmd_banks(args, b: Bench | None = None, cfg=None):
    own = b is None
    b = b or Bench()
    cfg = cfg or claims(getattr(args, "mutate_name", None))
    d = open(os.path.join(DISC, "DATA", "RASHNZ_E.DAT"), "rb").read()
    ents = [struct.unpack_from("<3I", d, 12 * i) for i in range(8)]
    for e in (3, 4, 5):
        off, bb, sb = ents[e]
        base = 96 + off
        bank = d[base:base + bb]
        samp = d[base + bb:base + bb + sb]
        spus = []
        for i in range(bank[4]):
            o = struct.unpack_from("<I", bank, 0x10 + 4 * i)[0]
            spus.append(struct.unpack_from("<I", bank, o + 12)[0])
        order = sorted(set(spus)) + [sb]
        res = {}
        for snd in (2, 3):
            s = spus[snd]
            nxt = min(x for x in order if x > s)
            starts = [p - s for p in range(s, nxt, 16) if samp[p + 1] & 4]
            ends = [p - s for p in range(s, nxt, 16) if samp[p + 1] & 1]
            res[snd] = (nxt - s, starts, ends)
        mod_sel = [x << cfg["lsaxShift"] for x in (0, 1)]
        lay3_sel = [x << cfg["lsaxShift"] for x in (0, 2, 4)]
        print("entry %d: sound 2 (modulator) %d bytes, loop starts %s; sound 3 (layer 3) %d bytes,"
              " loop starts %s" % (e, res[2][0], res[2][1], res[3][0], res[3][1]))
        b.check(res[2][1] == [0, 128] and res[2][2] == [112, 240], "entry %d sound 2 = two 128-byte loops" % e)
        b.check(res[3][1] == [0, 128, 256, 512] and res[3][2] == [112, 240, 496, 1008],
                "entry %d sound 3 = loops of 128, 128, 256, 512 bytes" % e)
        b.check(all(x in res[2][1] for x in mod_sel), "entry %d: modulator offsets %s are loop starts" % (e, mod_sel))
        b.check(all(x in res[3][1] for x in lay3_sel), "entry %d: layer-3 offsets %s are loop starts" % (e, lay3_sel))
    if own:
        print("engine_note banks: %d checks, %d failures" % (b.n, len(b.bad)))
    return b


# ---------------------------------------------------------------------------
# snap: the per-bike setup values against every savestate
# ---------------------------------------------------------------------------

class Ram:
    def __init__(self, path):
        self.d = open(path, "rb").read()

    def w(self, a):
        return s32(struct.unpack_from("<I", self.d, a & 0x1FFFFF)[0])

    def u(self, a):
        return struct.unpack_from("<I", self.d, a & 0x1FFFFF)[0]


def recip(v):                  # 0x80000000 divu ((v>>1) + ((v-2)>>31))
    a = u32((v >> 1) + (s32(v - 2) >> 31))
    return s32(0x80000000 // a) if a else None


def cmd_snap(args, b: Bench | None = None, cfg=None):
    own = b is None
    b = b or Bench()
    cfg = cfg or claims(getattr(args, "mutate_name", None))
    for st in STATES:
        r = Ram(os.path.join(STATE_DIR, st, "ram.bin"))
        gs = r.u(GAME_STATE_PTR)
        npl = r.w(gs + 0x30)
        E0 = r.u(G_ENGINE)
        S = 0x800D6870
        V = r.u(S + 0xC)
        for p in range(npl):
            E = E0 + cfg["stride"] * p
            bike = r.u(E)
            st_ = r.u(bike + 0x22C)
            ok_bike = bike == r.u(0x8005B268 + 4 * p)
            b.check(ok_bike, "%s p%d: E+0x00 is the player's bike" % (st, p))
            P = r.u(E + 0x2C)
            b.check(P == PARAMS_BASE + 20 * int(r.w(gs + 0x48 + 4 * p) / 9),
                    "%s p%d: E+0x2C = 0x800525F4 + 20*(model/9)" % (st, p))
            b.check(r.w(E + 0x3C) == fixmul(r.w(P) << 4, recip(r.w(st_ + 0xB4))),
                    "%s p%d: E+0x3C = FixMul(params[0]<<4, 2^31/(maxRevs/2))" % (st, p))
            cc = r.w(st_ + 0xCC)

            def rec2(q, h):
                return s32(0x80000000 // u32(q + (s32(h - 2) >> 31)))
            b.check(r.w(E + 0x44) == rec2(cc >> 2, cc >> 1) and r.w(E + 0x48) == rec2(cc >> 3, cc >> 2),
                    "%s p%d: E+0x44/+0x48 = 2^31/(stats[+0xCC]/4), 2^31/(stats[+0xCC]/8)" % (st, p))
            v = r.w(st_ + 0x190)
            t = (v >> 2) + (v >> 1)
            b.check(r.w(E + 0x50) == rec2(t >> 1, t), "%s p%d: E+0x50 from stats[+0x190]" % (st, p))
            for g in range(10):
                x = fixmul(r.w(st_ + 0xBC), r.w(st_ + 0x14 + 4 * g))
                if x == 0:
                    continue
                want = fixmul(r.w(st_ + 0x14), s32(0x80000000 // u32((x >> 1) + (s32(x - 2) >> 31))))
                b.check(r.w(cfg["gearTab"] + 4 * g) == want, "%s gear table [%d]" % (st, g))
            b.check(r.w(E + 0x70) == (127 * r.w(cfg["engineSlider"])) >> 7,
                    "%s p%d: E+0x70 = 127*engineSlider>>7 (rider on the bike: distance 0)" % (st, p))

            def voice(h):
                ch = u32(h) >> 27
                v = V + 44 * ch
                return (ch, (r.u(v + 4) & 0x07FFFFFF) == (u32(h) & 0x07FFFFFF), r.u(v + 0x20) & 0xFFFF)
            c1c, ok1c, p1c = voice(r.w(E + 0x1C))
            c20, ok20, p20 = voice(r.w(E + 0x20))
            c10, ok10, p10 = voice(r.w(E + 0x10))
            b.check(ok1c and ok20 and ok10, "%s: layers 0, 3 and 4 are live handles" % st)
            b.check(c1c == c20 + 1, "%s: layer 3 is on the channel just above its modulator (PMON)" % st,
                    "%d vs %d" % (c1c, c20))
            b.check(p20 == cfg["modStart"], "%s: the modulator voice was started at pitch 128" % st, hex(p20))
            b.check(p10 == 0 and p1c == 0,
                    "%s: layers 0 and 3 were started at pitch (E+0x30=0)*E+0x78>>16 = 0" % st)
    if own:
        print("engine_note snap: %d checks, %d failures" % (b.n, len(b.bad)))
    return b


# ---------------------------------------------------------------------------
# trace: run the interpreter with the watches and probes the replay needs
# ---------------------------------------------------------------------------

def jal_sites_of(img, name):
    lo, jr, _, _ = FUNCS[name]
    return [a for a, _ in jal_targets(img, lo, jr + 8)]


def cmd_trace(args):
    img = load_slus()
    src = os.path.join(STATE_DIR, args.state)
    out = os.path.join(OUT_DIR, args.name)
    os.makedirs(out, exist_ok=True)
    state = src
    if args.init_engine or args.mode7 or args.poke:
        state = os.path.join(out, "state")
        if os.path.isdir(state):
            shutil.rmtree(state)
        shutil.copytree(src, state)
        r = Ram(os.path.join(state, "ram.bin"))
        ram = bytearray(r.d)
        E = r.u(G_ENGINE)
        if args.init_engine:
            ram[(E + 4) & 0x1FFFFF] = 0          # E+0x04 = 0: the next frame runs EngineStart
        if args.mode7:
            struct.pack_into("<I", ram, (E + 0x7C) & 0x1FFFFF, 7)   # what 0x80018440(p,1) writes
        bike = r.u(E)
        for spec in args.poke or []:
            # BASE+OFF=VALUE[:SIZE], BASE in E | bike | rider | abs
            lhs, rhs = spec.split("=")
            size = 4
            if ":" in rhs:
                rhs, sz = rhs.split(":")
                size = int(sz)
            base, off = lhs.split("+")
            at = {"E": E, "bike": bike, "rider": r.u(bike + 0x354), "abs": 0}[base] + int(off, 0)
            val = int(rhs, 0) & ((1 << (8 * size)) - 1)
            ram[(at & 0x1FFFFF):(at & 0x1FFFFF) + size] = val.to_bytes(size, "little")
            print("  poke 0x%08X/%d = 0x%X" % (at, size, val))
        open(os.path.join(state, "ram.bin"), "wb").write(bytes(ram))
    r = Ram(os.path.join(state, "ram.bin"))
    gs = r.u(GAME_STATE_PTR)
    npl = r.w(gs + 0x30)
    E = r.u(G_ENGINE)
    watches = [(E, 132 * npl, "eng"), (G_ROAD_BANK, 4, "g1940"), (G_ENGINE, 4, "g1952"),
               (G_ROAD_MUTE, 4, "g1960"), (G_TOGGLE, 4, "g1888"), (AUDIO_GATE, 4, "gate"),
               (GAME_STATE_PTR, 4, "gsptr"), (gs + 0x30, 4, "npl"), (GEAR_TAB, 40, "geartab"),
               (SLIDERS, 16, "sliders"),
               (0x801FFF08, 52, "stack_note"), (0x801FFEB0, 48, "stack_start"),
               (0x80055D7C, 12, "stack_tick")]
    params = set()
    bikes = set()
    for p in range(npl):
        bk = r.u(E + 132 * p)
        bikes.add(bk)
        params.add(r.u(E + 132 * p + 0x2C))
    for P in sorted(params):
        watches.append((P, 20, "params"))
    for bk in sorted(bikes):
        for off, ln in ((0x24, 4), (0xB8, 12), (0x184, 4), (0x1C8, 12), (0x1E0, 4), (0x1FC, 4),
                        (0x216, 1), (0x228, 0x14), (0x24C, 0x14), (0x28C, 4), (0x2A4, 4),
                        (0x2B8, 8), (0x351, 7)):
            watches.append((bk + off, ln, "bike"))
        R = r.u(bk + 0x354)
        for off, ln in ((0xB8, 12), (0x1E0, 4), (0x1FC, 4), (0x220, 2), (0x228, 4), (0x25C, 4)):
            watches.append((R + off, ln, "rider"))
        stt = r.u(bk + 0x22C)
        watches.append((stt + 0xCC, 4, "stats"))
    probes = []
    for name, (lo, jr, _, _) in FUNCS.items():
        probes.append((lo, name + "_in"))
        probes.append((jr, name + "_out"))
        for a in jal_sites_of(img, name):
            probes.append((a + 8, "ret_%08X" % a))
    for c in CALLEES:
        if c != F_START:
            probes.append((c, "callee_%08X" % c))
    cmd = [args.rrverify, "trace", "--state", state, "--frames", str(args.frames), "--no-cop2",
           "--no-gpu", "--pad", args.pad, "--out", out]
    for a, n, name in watches:
        cmd += ["--watch", "0x%08X:%d:%s" % (a, n, name)]
    seen = set()
    for a, name in probes:
        if a in seen:
            continue
        seen.add(a)
        cmd += ["--probe", "0x%08X:%s" % (a, name)]
    print("running %s trace --state %s --frames %s --pad %s (%d watches, %d probes)"
          % (os.path.relpath(args.rrverify, ROOT), os.path.relpath(state, ROOT), args.frames,
             args.pad, len(watches), len(seen)))
    open(os.path.join(out, "watches.txt"), "w").write(
        "\n".join("0x%08X:%d:%s" % w for w in watches) + "\n")
    res = subprocess.run(cmd, capture_output=True, text=True)
    txt = res.stdout
    open(os.path.join(out, "trace_stdout.txt"), "w").write(txt)
    for line in txt.splitlines():
        if any(k in line for k in ("stopped", "vblanks", "buffer swaps", "_in ", "_out ",
                                   "wrote", "truncat")):
            print("  " + line.strip())
    return res.returncode


# ---------------------------------------------------------------------------
# replay: the transcriptions, run against the recorded memory traffic
# ---------------------------------------------------------------------------

REGN = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5",
        "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp",
        "sp", "fp", "ra"]


class Mismatch(Exception):
    pass


class Trace:
    def __init__(self, d):
        self.mem = []
        seen = set()
        with open(os.path.join(d, "watch.csv")) as f:
            for row in csv.DictReader(f):
                key = (row["seq"], row["pc"], row["address"], row["kind"])
                if key in seen:
                    continue
                seen.add(key)
                self.mem.append((int(row["seq"]), int(row["pc"], 16), row["kind"] == "write",
                                 int(row["address"], 16), int(row["size"]), int(row["value"], 16)))
        self.mem.sort()
        self.probes = defaultdict(list)       # pc -> [(seq, regs dict)]
        with open(os.path.join(d, "probes.csv")) as f:
            for row in csv.DictReader(f):
                regs = {k: int(row[k], 16) for k in REGN}
                self.probes[int(row["pc"], 16)].append((int(row["seq"]), regs))


class Ctx:
    """One replayed call.  Loads take the guest's value, stores and calls are compared."""

    def __init__(self, tr, fname, seq0, seq1, sp, cfg, cov):
        lo, jr, frame, skip = FUNCS[fname]
        self.tr, self.fname, self.cfg, self.cov = tr, fname, cfg, cov
        self.seq0, self.seq1 = seq0, seq1
        self.sp = u32(sp - frame)
        self.reads = defaultdict(deque)
        self.writes = []
        for (seq, pc, wr, addr, size, val) in tr.mem_window(seq0, seq1):
            if not (lo <= pc <= jr + 4) or pc in skip:
                continue
            if wr:
                self.writes.append((addr, size, val))
            else:
                self.reads[addr].append((size, val))
        self.mwrites = []
        self.mwritten = {}
        self.cursor = seq0
        self.ncalls = 0
        self.nloads = 0
        self.lo, self.jr = lo, jr

    def _ld(self, a, size):
        a = u32(a)
        q = self.reads.get(a)
        if not q:
            raise Mismatch("%s: model loads 0x%08X/%d, the guest made no (further) such load" % (self.fname, a, size))
        gsz, val = q.popleft()
        if gsz != size:
            raise Mismatch("%s: load 0x%08X size %d, guest size %d" % (self.fname, a, size, gsz))
        if a in self.mwritten and self.mwritten[a] != val:
            raise Mismatch("%s: re-load of 0x%08X gives 0x%X, model stored 0x%X" % (self.fname, a, val, self.mwritten[a]))
        self.nloads += 1
        return val

    def lw(self, a):
        return s32(self._ld(a, 4))

    def lhu(self, a):
        return self._ld(a, 2) & 0xFFFF

    def lh(self, a):
        v = self._ld(a, 2) & 0xFFFF
        return v - 0x10000 if v & 0x8000 else v

    def lbu(self, a):
        return self._ld(a, 1) & 0xFF

    def lb(self, a):
        v = self._ld(a, 1) & 0xFF
        return v - 0x100 if v & 0x80 else v

    def sw(self, a, v):
        self._st(a, 4, u32(v))

    def sb(self, a, v):
        self._st(a, 1, v & 0xFF)

    def _st(self, a, size, v):
        a = u32(a)
        if self.tr.is_watched(a):
            self.mwrites.append((a, size, v))
            self.mwritten[a] = v

    def call(self, site, target, args):
        ra = site + 8
        hits = self.tr.probes.get(target, [])
        ent = None
        for seq, regs in hits:
            if seq <= self.cursor or seq > self.seq1:
                continue
            if regs["ra"] == ra:
                ent = (seq, regs)
                break
        if ent is None:
            raise Mismatch("%s: model calls 0x%08X from 0x%08X, the guest did not" % (self.fname, target, site))
        seq, regs = ent
        for i, v in enumerate(args):
            if v is None:
                continue
            g = regs["a%d" % i]
            if u32(g) != u32(v):
                raise Mismatch("%s: call 0x%08X at 0x%08X: a%d guest 0x%08X model 0x%08X"
                               % (self.fname, target, site, i, g, u32(v)))
        ret = None
        for s2, regs2 in self.tr.probes.get(ra, []):
            if s2 > seq:
                ret = (s2, regs2)
                break
        if ret is None:
            raise Mismatch("%s: no return probe after 0x%08X" % (self.fname, site))
        self.cursor = ret[0]
        self.ncalls += 1
        self.mwritten.clear()     # a callee may write anything, e.g. its out-parameters
        self.cov.add("call@%08X" % site)
        return s32(ret[1]["v0"])

    def finish(self):
        left = sum(len(q) for q in self.reads.values())
        if left:
            ex = [hex(a) for a, q in self.reads.items() if q][:6]
            raise Mismatch("%s: %d guest load(s) the model never made: %s" % (self.fname, left, ex))
        if self.mwrites != self.writes:
            for i, (m, g) in enumerate(zip(self.mwrites, self.writes)):
                if m != g:
                    raise Mismatch("%s: store #%d model (0x%08X,%d,0x%X) guest (0x%08X,%d,0x%X)"
                                   % (self.fname, i, m[0], m[1], m[2], g[0], g[1], g[2]))
            raise Mismatch("%s: %d model stores vs %d guest stores" % (self.fname, len(self.mwrites), len(self.writes)))
        # every call the guest made from a site of this function was modelled
        n = 0
        for site in self.tr.sites[self.fname]:
            for c in self.tr.callees_by_ra.get(site + 8, []):
                if self.seq0 < c <= self.seq1:
                    n += 1
        if n != self.ncalls:
            raise Mismatch("%s: guest made %d calls, model %d" % (self.fname, n, self.ncalls))


def m_note(c, p):
    K = c.cfg
    sp = c.sp
    E = u32(c.lw(G_ENGINE) + K["stride"] * p)
    bike = c.lw(E)
    revs = c.lw(bike + 0x25C)
    f3c = c.lw(E + 0x3C)
    v1 = c.call(0x80016A14, FIXMUL, [sra(revs, 4), f3c])
    bike = c.lw(E)
    R = c.lw(bike + 0x354)
    rs = c.lw(R + 0x25C)
    a0 = 0
    if u32(rs) < 3:
        a0 = 1 if c.lhu(R + 0x220) != 0 else 0
    s2 = v1 if a0 else 0
    s1 = -1 if u32(rs) < 2 else 0
    g = c.lb(bike + 0x351)
    load = c.lw(bike + 0x24C)
    tab = c.lw(K["gearTab"] + 4 * g)
    v = c.call(0x80016A80, FIXMUL, [load, tab])
    if sra(v, 9) < 127:
        bike = c.lw(E)
        g = c.lb(bike + 0x351)
        load = c.lw(bike + 0x24C)
        tab = c.lw(K["gearTab"] + 4 * g)
        v = c.call(0x80016AB4, FIXMUL, [load, tab])
        s1 = s1 & sra(v, 9)
        c.cov.add("note:load<127")
    else:
        s1 = s1 & 0x7F
        c.cov.add("note:load>=127")
    bk = c.lw(E)
    vz = c.lw(bk + 0x1D0)
    c.sw(sp + 20, sp + 56)
    c.sw(sp + 24, sp + 60)
    c.sw(sp + 28, sp + 64)
    c.sw(sp + 32, 0)
    c.sw(sp + 16, vz)
    x = c.lw(bk + 0xB8)
    z = c.lw(bk + 0xC0)
    vx = c.lw(bk + 0x1C8)
    c.call(0x80016B04, S3D, [p, x, z, vx])
    vol = c.lw(sp + 56)
    m = c.lw(K["engineSlider"])
    pan = c.lw(sp + 60)
    pit = c.lw(sp + 64)
    c.sw(E + 0x74, pan)
    c.sw(E + 0x78, pit)
    c.sw(E + 0x70, sra(mul(vol, m), 7))
    if s2 < K["idle"]:
        f18 = c.lw(E + 0x18)
        s2 = K["idle"]
        if f18 == 0:
            c.cov.add("note:start-idle-layer2")
            f60 = c.lw(E + 0x60)
            f70 = c.lw(E + 0x70)
            c.sw(sp + 44, sra(mul(f70, 127 - f60), 7))
            c.sw(sp + 48, c.lw(E + 0x74))
            bank = c.lw(E + 8)
            gp_ = c.call(0x80016B88, GSP, [bank, 4, 0])
            f78 = c.lw(E + 0x78)
            c.sw(sp + 40, sra(mul(f78, gp_), 16))
            c.sw(sp + 16, sp + 40)
            bank = c.lw(E + 8)
            h = c.call(0x80016BBC, SV, [bank, 4, 0, 1])
            f14 = c.lw(E + 0x14)
            c.sw(E + 0x18, h)
            c.call(0x80016BC8, STOPV, [f14])
            c.sw(E + 0x14, 0)
        else:
            c.cov.add("note:idle-running")
    else:
        f14 = c.lw(E + 0x14)
        if f14 == 0:
            c.cov.add("note:start-rev-layer1")
            f60 = c.lw(E + 0x60)
            f70 = c.lw(E + 0x70)
            c.sw(sp + 44, sra(mul(f70, 127 - f60), 7))
            c.sw(sp + 48, c.lw(E + 0x74))
            f30 = c.lw(E + 0x30)
            f78 = c.lw(E + 0x78)
            c.sw(sp + 40, sra(mul(f30, f78), 16))
            c.sw(sp + 16, sp + 40)
            bank = c.lw(E + 8)
            h = c.call(0x80016C40, SV, [bank, 1, 0, 1])
            f18 = c.lw(E + 0x18)
            c.sw(E + 0x14, h)
            c.call(0x80016C4C, STOPV, [f18])
            c.sw(E + 0x18, 0)
        else:
            c.cov.add("note:rev-running")
        bk = c.lw(E)
        surf = c.lbu(bk + 0x216)
        if not (u32(surf - 1) < 2):
            c.cov.add("note:jitter")
            x = c.call(0x80016C78, GETRCNT, [0xF2000002]) & 0xFF
            s2 = s32(s2 + ((K["jitA"] * x) >> 8))
            x = c.call(0x80016CA8, GETRCNT, [0xF2000002]) & 0xFF
            s1 = s32(s1 + ((K["jitB"] * x) >> 8))
        else:
            c.cov.add("note:no-jitter")
    bk = c.lw(E)
    df = c.lw(bk + 0x250)
    ld = c.lw(bk + 0x24C)
    sel = None
    if ld < sra(df, 1):
        fl = c.lw(bk + 0x234)
        if not (fl & 0x20):
            P = c.lw(E + 0x2C)
            v = c.lw(P + 8)
            sel = 0
    if sel is None:
        P = c.lw(E + 0x2C)
        v = c.lw(P + 4)
        sel = 1
    c.cov.add("note:mod-sel%d" % sel)
    c.sw(E + 0x40, v)
    h20 = c.lw(E + 0x20)
    c.call(0x80016D2C, SETLOOP, [sel << K["lsaxShift"], h20])
    bk = c.lw(E)
    fl = c.lw(bk + 0x234)
    sel3 = 4 if fl & 0x200 else (2 if fl & 0x20 else 0)
    c.cov.add("note:l3-sel%d" % sel3)
    h1c = c.lw(E + 0x1C)
    c.call(0x80016D6C, SETLOOP, [sel3 << K["lsaxShift"], h1c])
    bk = c.lw(E)
    f6c = c.lw(E + 0x6C)
    g = c.lb(bk + 0x351)
    if f6c < g:
        s2 = s32(s2 + K["shift"])
        c.cov.add("note:upshift")
    elif g < f6c:
        s2 = s32(s2 - K["shift"])
        s1 = s32(s1 + K["blip"])
        if not (s1 < 127):
            s1 = K["blipClamp"]
        c.cov.add("note:downshift")
    bk = c.lw(E)
    P = c.lw(E + 0x2C)
    g = c.lb(bk + 0x351)
    c.sw(E + 0x6C, g)
    mx = c.lw(P)
    if s2 < mx:
        mx = s2
    else:
        c.cov.add("note:level-capped")
    s2 = mx
    en = c.lbu(E + 4)
    if en:
        f30 = c.lw(E + 0x30)
        f60 = c.lw(E + 0x60)
        c.sw(E + 0x34, s2)
        c.sw(E + 0x64, s1)
        c.sw(E + 0x38, s32(s2 - f30) >> K["rateShift"])
        c.sw(E + 0x68, s32(s1 - f60) >> K["rateShift"])
    else:
        c.cov.add("note:first-call->EngineStart")
        c.call(0x80016E24, F_START, [p])


def m_start(c, p):
    K = c.cfg
    sp = c.sp
    E = u32(c.lw(G_ENGINE) + K["stride"] * p)
    if c.lbu(E + 4):
        return
    c.cov.add("start:ran")
    bk = c.lw(E)
    c.sw(sp + 20, sp + 56)
    c.sw(sp + 16, 0)
    c.sw(sp + 24, sp + 60)
    c.sw(sp + 28, 0)
    c.sw(sp + 32, 0)
    x = c.lw(bk + 0xB8)
    z = c.lw(bk + 0xC0)
    c.call(0x80016804, S3D, [p, x, z, 0])
    c.sw(sp + 48, c.lw(sp + 60))
    f30 = c.lw(E + 0x30)
    f78 = c.lw(E + 0x78)
    vol = c.lw(sp + 56)
    m = c.lw(K["engineSlider"])
    c.sw(sp + 40, sra(mul(f30, f78), 16))
    f60 = c.lw(E + 0x60)
    v56 = sra(mul(vol, m), 7)
    c.sw(sp + 56, v56)
    c.sw(sp + 44, sra(mul(v56, f60), 7))
    c.sw(sp + 16, sp + 40)
    h0 = c.call(0x8001687C, SV, [c.lw(E + 8), 0, 0, 1])
    c.sw(E + 0x10, h0)
    c.sw(sp + 44, 0)
    c.sw(sp + 16, sp + 40)
    h3 = c.call(0x8001689C, SV, [c.lw(E + 8), 3, 0, 1])
    c.sw(E + 0x1C, h3)
    c.call(0x800168AC, PMOD, [h3, 1])
    c.sw(E + 0x40, K["modStart"])
    c.sw(sp + 40, K["modStart"])
    c.sw(sp + 16, sp + 40)
    h4 = c.call(0x800168D0, SV, [c.lw(E + 8), 2, 0, 1])
    h3b = c.lw(E + 0x1C)
    c.sw(E + 0x20, h4)
    if s32((u32(h3b) >> 27) - (u32(h4) >> 27)) != 1:
        c.cov.add("start:not-adjacent")
        c.call(0x800168F8, STOPV, [h4])
        c.call(0x80016904, STOPV, [c.lw(E + 0x1C)])
        c.call(0x80016910, PMOD, [c.lw(E + 0x1C), 0])
        c.sw(E + 0x20, -1)
        c.sw(E + 0x1C, -1)
    else:
        c.cov.add("start:adjacent")
    f60 = c.lw(E + 0x60)
    v56 = c.lw(sp + 56)
    c.sw(sp + 40, -1)
    c.sw(sp + 44, sra(mul(v56, 127 - f60), 7))
    c.sw(sp + 16, sp + 40)
    h2 = c.call(0x8001695C, SV, [c.lw(E + 8), 4, 0, 1])
    f30 = c.lw(E + 0x30)
    f60 = c.lw(E + 0x60)
    v56 = c.lw(sp + 56)
    pan = c.lw(sp + 60)
    c.sw(E + 0x18, h2)
    for off in (0x14, 0x38, 0x68, 0x7C, 0x4C, 0x24, 0x28):
        c.sw(E + off, 0)
    c.sw(E + 0x54, -1)
    c.sw(E + 0x58, -1)
    c.sw(E + 0x5C, 0)
    c.sb(E + 4, 1)
    c.sw(E + 0x34, f30)
    c.sw(E + 0x64, f60)
    c.sw(E + 0x70, v56)
    c.sw(E + 0x74, pan)


def m_road(c, p):
    K = c.cfg
    sp = c.sp
    s1 = 0
    s3 = 0
    s2 = -1
    E = u32(c.lw(G_ENGINE) + K["stride"] * p)
    bike = c.lw(E)
    s4 = -1
    surf = c.lbu(bike + 0x216)
    f7c = c.lw(E + 0x7C)
    s5 = 0 if u32(surf - 1) < 2 else 1
    if f7c > 0:
        c.cov.add("road:mode")
        if f7c & 1:
            spd = c.lw(bike + 0x1E0)
            s1 = 127 if 0x7FFFF < spd else sra(spd, 12)
            if absw(c.lw(bike + 0xBC) - c.lw(bike + 0x1FC)) < 16384:
                t = absw(c.lw(bike + 0x28C))
                if s5:
                    s2 = 16 if t < 16385 else 18
                else:
                    s2 = 12 if t < 16385 else 17
            else:
                s2 = -1
            spd = c.lw(c.lw(E) + 0x1E0)
            if not (32767 < spd):
                s2 = -1
                c.sw(E + 0x7C, c.lw(E + 0x7C) & -2)
        f = c.lw(E + 0x7C)
        if f & 2:
            v1 = c.lw(E)
            if c.lw(v1 + 0x184) & 1:
                a1 = 1
                v1 = c.lw(E)
            elif c.lb(v1 + 0x216) < 3:
                a1 = 0
            else:
                a1 = 1
                v1 = c.lw(E)
            R = c.lw(v1 + 0x354)
            spd = c.lw(R + 0x1E0)
            s3 = 127 if 0x7FFFF < spd else sra(spd, 12)
            fl = c.lw(R + 0x228)
            s4 = a1 + 19
            if fl & 0x40000000:
                if not (absw(c.lw(R + 0xBC) - c.lw(R + 0x1FC)) < 16384):
                    s4 = -1
            if c.lw(E + 0x58) < 0 and s4 >= 0:
                b2 = c.lw(E)
                sb = c.lb(b2 + 0x216)
                R2 = c.lw(b2 + 0x354)
                c.call(0x8001708C, PS3D, [c.lw(R2 + 0xB8), c.lw(R2 + 0xC0), 102 if sb == 4 else 55, 0])
            spd = c.lw(c.lw(c.lw(E) + 0x354) + 0x1E0)
            if not (32767 < spd):
                s4 = -1
                c.sw(E + 0x7C, c.lw(E + 0x7C) & -3)
    else:
        f238 = c.lw(bike + 0x238)
        if f238 & 0x400:
            s2 = -1
            c.cov.add("road:0x400")
        else:
            v1 = (u32(c.lw(bike + 0x24)) >> 25) & 3
            go = True
            if v1 == 0 and c.lw(E + 0x5C) == 0:
                go = False
            if go:
                c.cov.add("road:crash17")
                s2 = 17
                if v1 != 0:
                    c.sw(E + 0x5C, 4)
                else:
                    c.sw(E + 0x5C, c.lw(E + 0x5C) - 1)
                s1 = clamp127(s32(c.lw(E + 0x5C) << 5))
            a0 = c.lw(E)
            a1 = c.lw(a0 + 0x2BC)
            if 0x11FFF < a1:
                t = c.lw(a0 + 0x28C)
                s1 = sra(a1, 10)
                if not (absw(t) < 16385):
                    c.cov.add("road:slide12")
                    s2 = 12
                    a1b = c.lw(a0 + 0x2A4)
                    f44 = c.lw(E + 0x44)
                    s1 = sra(c.call(0x800171AC, FIXMUL, [f44, absw(a1b)]), 9)
                    r = c.call(0x800171B4, RAND, [])
                    s1 = clamp127(s1 - 32 + (r & 0x3F))
                    if s1 < 40:
                        f4c = c.lw(E + 0x4C)
                        c.sw(E + 0x4C, f4c + 1)
                        if not (f4c < 8):
                            c.sw(E + 0x4C, 0)
                            s2 = -1
                    else:
                        c.sw(E + 0x4C, 0)
                    b2 = c.lw(E)
                    t = c.lw(b2 + 0x2A4)
                    cc = c.lw(c.lw(b2 + 0x22C) + 0xCC)
                    a1c = s32(absw(t) - sra(cc, 1))
                    s4 = 14
                    if a1c < 0:
                        a1c = 0
                    f48 = c.lw(E + 0x48)
                    s3 = sra(c.call(0x80017258, FIXMUL, [f48, a1c]), 9)
                    r = c.call(0x80017260, RAND, [])
                    s3 = clamp127(s3 - 32 + (r & 0x3F))
                    f30 = c.lw(E + 0x30)
                    f34 = c.lw(E + 0x34)
                    c.sw(E + 0x30, f30 + s3)
                    c.sw(E + 0x34, f34 + s3)
                else:
                    c.cov.add("road:13")
                    s2 = 13
                    if not (s1 < 128):
                        s1 = 127
            else:
                v1 = c.lw(a0 + 0x2B8)
                if 0xC7FF < v1:
                    c.cov.add("road:14")
                    s2 = 14
                    s1 = clamp127(sra(v1 - 0xC800, 7))
            if s5:
                c.cov.add("road:offroad")
                if c.lw(E + 0x5C) == 0 and s2 != 17:
                    s2 = 16
                    s1 = s1 if s3 < s1 else s3
                s4 = 15
                spd = c.lw(c.lw(E) + 0x1E0)
                s3 = 127 if 0xFFFFF < spd else sra(spd, 14)
            rs = c.lw(c.lw(c.lw(E) + 0x354) + 0x25C)
            if not (u32(rs - 1) < 3):
                s1 = 0
    # the tail
    t0 = c.lw(E + 0x70)
    m = c.lw(K["fxSlider"])
    g = c.lw(G_ROAD_MUTE)
    a3 = sra(mul(t0, m), 7)
    a3 = s32(a3 + ((-g) & (-a3)))
    bank = c.lw(G_ROAD_BANK)
    c.sw(sp + 56, t0)
    c.sw(sp + 56, a3)
    c.sw(sp + 44, sra(mul(a3, s1), 7))
    v = c.call(0x800173E4, GSP, [bank, s2, 0])
    f78 = c.lw(E + 0x78)
    c.sw(sp + 40, sra(mul(f78, v), 16))
    c.sw(sp + 48, c.lw(E + 0x74))
    if s2 != c.lw(E + 0x54):
        h = c.lw(E + 0x24)
        c.sw(E + 0x54, s2)
        c.cov.add("road:A-change")
        if h:
            c.call(0x80017430, STOPV, [h])
        if s2 >= 0:
            bank = c.lw(G_ROAD_BANK)
            c.sw(sp + 16, sp + 40)
            c.sw(E + 0x24, c.call(0x80017450, SV, [bank, s2, 0, 1]))
    else:
        h = c.lw(E + 0x24)
        if h:
            c.call(0x80017470, UV2, [h, sp + 40])
    if c.lw(E + 0x7C) != 0:
        R = c.lw(c.lw(E) + 0x354)
        c.sw(sp + 20, sp + 56)
        c.sw(sp + 16, 0)
        c.sw(sp + 24, sp + 60)
        c.sw(sp + 28, 0)
        c.sw(sp + 32, 0)
        c.call(0x800174B8, S3D, [p, c.lw(R + 0xB8), c.lw(R + 0xC0), 0])
        vol = c.lw(sp + 56)
        m = c.lw(K["fxSlider"])
        c.sw(sp + 48, c.lw(sp + 60))
        g = c.lw(G_ROAD_MUTE)
        v1 = sra(mul(vol, m), 7)
        c.sw(sp + 56, s32(v1 + ((-g) & (-v1))))
    vv = c.lw(sp + 56)
    bank = c.lw(G_ROAD_BANK)
    c.sw(sp + 44, sra(mul(vv, s3), 7))
    v = c.call(0x80017524, GSP, [bank, s4, 0])
    f78 = c.lw(E + 0x78)
    c.sw(sp + 40, sra(mul(f78, v), 16))
    if s4 != c.lw(E + 0x58):
        h = c.lw(E + 0x28)
        c.sw(E + 0x58, s4)
        c.cov.add("road:B-change")
        if h:
            c.call(0x80017564, STOPV, [h])
        if s4 >= 0:
            bank = c.lw(G_ROAD_BANK)
            c.sw(sp + 16, sp + 40)
            c.sw(E + 0x28, c.call(0x80017584, SV, [bank, s4, 0, 1]))
    else:
        h = c.lw(E + 0x28)
        if h:
            c.call(0x800175A4, UV2, [h, sp + 40])
    b = c.lw(E)
    if c.lw(b + 0x234) & 0x18000000:
        c.call(0x800175CC, FX1, [b, 1, 600, 0])
        b = c.lw(E)
    else:
        spawned = False
        if c.lw(b + 0x184) & 1:
            if not (c.lh(b + 0x1E2) < 13):
                c.call(0x80017634, FX2, [b, b + 0xB8, 0])
                b = c.lw(E)
                spawned = True
        if not spawned:
            b = c.lw(E)
            if 0x11FFF < c.lw(b + 0x2BC):
                if c.lw(b + 0x234) & 8:
                    c.call(0x80017634, FX2, [b, b + 0xB8, 0])
                    b = c.lw(E)
    if 0x11FFF < c.lw(b + 0x2B8):
        if c.lw(b + 0x234) & 4:
            c.call(0x8001766C, FX2, [b, b + 0xB8, 2])


def m_tick(c, _p):
    K = c.cfg
    sp = c.sp
    if not (c.lw(AUDIO_GATE) & 4):
        c.cov.add("tick:gated")
        c.call(0x80019C30, SVC, [])
        return
    gs = c.lw(GAME_STATE_PTR)
    if c.lw(gs + 0x30) != 2:
        t = c.lw(G_TOGGLE)
        if t != 0:
            c.sw(G_TOGGLE, t ^ 1)
            c.cov.add("tick:skip-odd")
            c.call(0x80019C30, SVC, [])
            return
    t = c.lw(G_TOGGLE)
    E = u32(c.lw(G_ENGINE) + K["stride"] * t)
    rate = c.lw(E + 0x38)
    lvl = c.lw(E + 0x30)
    tgt = c.lw(E + 0x34)
    l1 = s32(lvl + rate)
    prod = mul(l1 - tgt, rate)
    c.sw(E + 0x30, l1)
    a2 = l1 if prod < 0 else tgt
    if prod >= 0 and a2 != l1:
        c.cov.add("tick:ramp1-clamped")
    tgt2 = c.lw(E + 0x64)
    rate2 = c.lw(E + 0x68)
    prod2 = mul(l1 - tgt2, rate2)
    lv2 = c.lw(E + 0x60)
    c.sw(E + 0x30, a2)
    s2 = s32(lv2 + rate2)
    c.sw(E + 0x60, s2)
    v1 = s2 if prod2 < 0 else tgt2
    if prod2 >= 0 and v1 != s2:
        c.cov.add("tick:ramp2-clamped")
    en = c.lbu(E + 4)
    c.sw(E + 0x60, v1)
    if en:
        bk = c.lw(E)
        pan = c.lw(E + 0x74)
        fl = c.lw(bk + 0x234)
        c.sw(sp + 24, pan)
        hurt = (u32(fl) >> 9) & 1
        c.cov.add("tick:hurt%d" % hurt)
        lv = c.lw(E + 0x30) if K["tickPitchClamped"] else l1
        if not K["tickPitchClamped"]:
            c.lw(E + 0x30)
        pv = s32(lv + ((-hurt) & sra(lv, 2)))
        c.sw(sp + 16, pv)
        f78 = c.lw(E + 0x78)
        pv = sra(mul(pv, f78), 16)
        c.sw(sp + 16, pv)
        f70 = c.lw(E + 0x70)
        c.sw(sp + 20, f70 if hurt else sra(mul(f70, s2), 7))
        c.call(0x80019ADC, UV, [c.lw(E + 0x10), sp + 16])
        if c.lw(E + 0x14):
            c.cov.add("tick:layer1")
            if hurt:
                pp = c.lw(sp + 16)
                c.sw(sp + 20, f70)
                c.sw(sp + 16, sra(pp, 1) + sra(pp, 2))
            else:
                c.sw(sp + 20, sra(mul(f70, 127 - s2), 7))
            c.call(0x80019B64, UV, [c.lw(E + 0x14), sp + 16])
        else:
            c.cov.add("tick:layer2")
            gp_ = c.call(0x80019B3C, GSP, [c.lw(E + 8), 4, 0])
            f78 = c.lw(E + 0x78)
            c.sw(sp + 20, f70)
            c.sw(sp + 16, sra(mul(f78, gp_), 16))
            c.call(0x80019B64, UV, [c.lw(E + 0x18), sp + 16])
        v = sra(c.lw(E + 0x30), 4)
        if not (v < 128):
            v = 127
        c.sw(sp + 20, sra(mul(f70, v), 7))
        P = c.lw(E + 0x2C)
        pp = c.lw(sp + 16)
        pp = sra(mul(pp, c.lw(P + 0x0C)), 16)
        c.sw(sp + 16, pp)
        c.sw(sp + 16, sra(mul(pp, c.lw(E + 0x78)), 16))
        c.call(0x80019BD4, UV, [c.lw(E + 0x1C), sp + 16])
        c.sw(sp + 20, 0)
        P = c.lw(E + 0x2C)
        lv = c.lw(E + 0x30)
        shv = c.lw(P + 0x10)
        v = s32(sra(lv, shv) + c.lw(E + 0x40))
        c.sw(sp + 16, v)
        c.sw(sp + 16, sra(mul(v, c.lw(E + 0x78)), 16))
        c.call(0x80019C18, UV, [c.lw(E + 0x20), sp + 16])
    t = c.lw(G_TOGGLE)
    c.sw(G_TOGGLE, t ^ 1)
    c.call(0x80019C30, SVC, [])


MODELS = {"note": m_note, "road": m_road, "start": m_start, "tick": m_tick}


def load_trace(d):
    tr = Trace(d)
    img = load_slus()
    tr.sites = {n: jal_sites_of(img, n) for n in FUNCS}
    # the watched byte set, re-read from the command line the trace was made with
    tr.ranges = []
    seqs = [m[0] for m in tr.mem]
    tr._seqs = seqs
    import bisect

    def mem_window(a, b_):
        i = bisect.bisect_left(seqs, a)
        j = bisect.bisect_right(seqs, b_)
        return tr.mem[i:j]
    tr.mem_window = mem_window
    spec = open(os.path.join(d, "watches.txt")).read().split()
    for s in spec:
        a, n = s.split(":")[:2]
        tr.ranges.append((int(a, 16), int(a, 16) + int(n)))
    tr.is_watched = lambda a: any(lo <= a < hi for lo, hi in tr.ranges)
    tr.callees_by_ra = defaultdict(list)
    for tgt in CALLEES:
        for seq, regs in tr.probes.get(tgt, []):
            tr.callees_by_ra[regs["ra"]].append(seq)
    return tr


def replay(d, cfg, verbose=True):
    tr = load_trace(d)
    cov = set()
    stats = {}
    fails = []
    for name, (lo, jr, frame, _) in FUNCS.items():
        ins = tr.probes.get(lo, [])
        outs = [s for s, _ in tr.probes.get(jr, [])]
        n_ok = n = loads = stores = calls = 0
        for seq0, regs in ins:
            nxt = [s for s in outs if s > seq0]
            if not nxt:
                continue          # the run stopped inside this call
            seq1 = nxt[0]
            n += 1
            c = Ctx(tr, name, seq0, seq1, regs["sp"], cfg, cov)
            try:
                MODELS[name](c, s32(regs["a0"]))
                c.finish()
                n_ok += 1
                loads += c.nloads
                stores += len(c.mwrites)
                calls += c.ncalls
            except Mismatch as e:
                fails.append(str(e))
        stats[name] = (n, n_ok, loads, stores, calls)
        if verbose:
            print("  %-5s 0x%08X  calls replayed %4d  exact %4d   loads %6d  stores %5d  callee calls %5d"
                  % (name, lo, n, n_ok, loads, stores, calls))
    if verbose:
        for f in fails[:8]:
            print("  MISMATCH " + f)
        print("  branch coverage: " + ", ".join(sorted(x for x in cov if not x.startswith("call@"))))
    return stats, fails, cov


def cmd_replay(args):
    cfg = claims(args.mutate_name)
    d = os.path.join(OUT_DIR, args.name)
    print("replay %s%s" % (os.path.relpath(d, ROOT), (" (mutated: %s)" % args.mutate_name) if args.mutate_name else ""))
    stats, fails, _ = replay(d, cfg)
    n = sum(v[0] for v in stats.values())
    print("engine_note replay: %d calls, %d mismatches" % (n, len(fails)))
    return 1 if fails else 0


# ---------------------------------------------------------------------------
# verify: everything, with the traces that exist
# ---------------------------------------------------------------------------

def run_all(mutate):
    cfg = claims(mutate)
    b = Bench(quiet=bool(mutate))

    class A:
        mutate_name = mutate
    cmd_static(A, b, cfg)
    cmd_banks(A, b, cfg)
    cmd_snap(A, b, cfg)
    runs = sorted(x for x in os.listdir(OUT_DIR)) if os.path.isdir(OUT_DIR) else []
    for r in runs:
        d = os.path.join(OUT_DIR, r)
        if not os.path.exists(os.path.join(d, "watch.csv")):
            continue
        stats, fails, _ = replay(d, cfg, verbose=not mutate)
        for name, (n, ok, *_rest) in stats.items():
            if n:
                b.check(ok == n and not fails, "replay %s: %s %d/%d exact" % (r, name, ok, n))
    return b


def cmd_verify(args):
    if args.mutate:
        total_bad = 0
        undetected = []
        for m in MUTATIONS:
            b = run_all(m)
            print("  mutation %-10s -> %d failure(s)" % (m, len(b.bad)))
            total_bad += len(b.bad)
            if not b.bad:
                undetected.append(m)
        print("engine_note: %d mutations, %d undetected, %d failures in total"
              % (len(MUTATIONS), len(undetected), total_bad))
        return 1 if total_bad else 0
    b = run_all(None)
    print("engine_note: %d checks, %d failures" % (b.n, len(b.bad)))
    return 1 if b.bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for n in ("static", "banks", "snap"):
        s = sub.add_parser(n)
        s.add_argument("--mutate", dest="mutate_name", choices=MUTATIONS + ("all",))
    s = sub.add_parser("trace")
    s.add_argument("--name", required=True)
    s.add_argument("--state", default="rr-race")
    s.add_argument("--frames", default="40")
    s.add_argument("--pad", default="0xBFFF")
    s.add_argument("--init-engine", action="store_true", help="copy the state with E+0x04 = 0")
    s.add_argument("--mode7", action="store_true", help="copy the state with E+0x7C = 7")
    s.add_argument("--poke", action="append", help="BASE+OFF=VAL[:SIZE] on a copy of the state;"
                   " BASE is E, bike, rider or abs")
    s.add_argument("--rrverify", default=RRVERIFY)
    s = sub.add_parser("replay")
    s.add_argument("--name", required=True)
    s.add_argument("--mutate", dest="mutate_name", choices=MUTATIONS + ("all",))
    s = sub.add_parser("verify")
    s.add_argument("--mutate", action="store_true")
    args = ap.parse_args()
    if args.cmd == "static":
        b = cmd_static(args)
        sys.exit(1 if b.bad else 0)
    if args.cmd == "banks":
        b = cmd_banks(args)
        sys.exit(1 if b.bad else 0)
    if args.cmd == "snap":
        b = cmd_snap(args)
        sys.exit(1 if b.bad else 0)
    if args.cmd == "trace":
        sys.exit(cmd_trace(args))
    if args.cmd == "replay":
        sys.exit(cmd_replay(args))
    if args.cmd == "verify":
        sys.exit(cmd_verify(args))


if __name__ == "__main__":
    main()
