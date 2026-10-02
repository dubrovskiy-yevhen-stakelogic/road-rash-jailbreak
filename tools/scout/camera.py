"""Scout probe for the RACE CAMERA of Road Rash: Jailbreak (USA, SLUS_01053).

The camera is `ViewUpdate RASHCDG 0x800881B4` and the fourteen functions under it (the aim, the
render frame, the transition, the lead, the springs ...), the view record they keep at
0x800CD898 + 1132 p, and `DATA\\CAMERA*.CA`. This probe re-derives their behaviour independently of
the C++ tree:

  * SKELETON: image hashes; the extent, frame and exact callee multiset of all fifteen functions; the
    three callers of ViewUpdate; no COP2 instruction anywhere in the tree; the two Rand and the two
    GetRCnt sites; the literal words the arithmetic rests on; each callee's status (ported = has a
    row in the bench's row tables, seam = declared as an oracle callee of `view_update`), read from
    tools\\rrverify\\verify_physics.cpp and rows_*.inc, never typed in;
  * CAMERA.CA: the loader's 224-byte read into 0x800CD7B8 and the file-name rule; an independent
    parse of the four files, compared field by field with `rrtool camera` (the C++ parser
    src\\rrformats\\camera_ca.cpp) and byte for byte with RAM in every race image;
  * LIVE: `<build>\\rrverify.exe trace` resumes of the rr-race and quick snapshots, and of copies of
    them whose only difference is DATA in the view record (another camera, the look-behind bit, a
    re-cut, the rider-follow bit, the ground-settle bit, the director; and four that make a literal
    decisive: the followers kicked off rest, a short followed bike, a sideways eye, an emptied zone
    list - each window is itself a check, `reach`). The original code is never touched. Seven functions are run as MODELS against every call the interpreter made (writes
    outside the model's own frame in order, every call with its arguments, v0); ViewUpdate's own
    arithmetic is checked as identities on its entry and exit state (the targets from CAMERA.CA,
    the speed follower, the eye and look-point placement); the path through ViewUpdate and its
    direct callee sequence are censused per call; the Rand calls of the tree are counted; and the
    order "ViewDistance reads the eye before ViewUpdate rewrites it" is measured.

Reads ONLY work\\disc_us, work\\oracle\\state and work\\oracle\\vr_capture\\ramdumps; writes ONLY under
work\\camera (snapshot copies and trace outputs, deleted by `clean`). Never copies game bytes into
the repo.

    python tools\\scout\\camera.py info             # skeleton + CAMERA.CA only
    python tools\\scout\\camera.py live             # the traced runs, verbose
    python tools\\scout\\camera.py verify           # camera: <n> checks, <m> failures
    python tools\\scout\\camera.py verify --mutate  # every mutation must be caught by its own check
    python tools\\scout\\camera.py clean            # delete work\\camera\\probe
"""

from __future__ import annotations

import argparse
import bisect
import copy
import csv
import glob
import os
import re
import shutil
import struct
import subprocess
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import exe as E  # noqa: E402
import pairs as P  # noqa: E402

ROOT = E.ROOT
# The disc image: RRJB_DISC, else the first line of disc.txt in the repository root.
DISC_BIN = os.environ.get("RRJB_DISC") or (open(os.path.join(ROOT, "disc.txt"), encoding="utf-8").readline().strip()
                                           if os.path.exists(os.path.join(ROOT, "disc.txt")) else "")
STATE_DIR = os.path.join(ROOT, "work", "oracle", "state")
DUMP_DIR = os.path.join(ROOT, "work", "oracle", "vr_capture", "ramdumps")
WORK = os.path.join(ROOT, "work", "camera", "probe")
# RRJB_PROBE_BUILD names the build directory that holds rrverify.exe (default below).
RRVERIFY = os.path.join(ROOT, os.environ.get("RRJB_PROBE_BUILD") or "build_m0", "rrverify.exe")
OVL_BASE = 0x8005B5E8

u32, s32, iabs = P.u32, P.s32, P.iabs
Mismatch = P.Mismatch

# ---------------------------------------------------------------------------------------------
# addresses (RASHCDG unless marked SLUS)
# ---------------------------------------------------------------------------------------------
VIEW_UPDATE, AIM, ORIENT, TRANSITION = 0x800881B4, 0x80087420, 0x80086E1C, 0x8008676C
LEAD, TO_FRAME, TO_POLAR, ANGLE_CHASE = 0x800871A8, 0x80086584, 0x800863EC, 0x80086C00
SET_MODE, CHASE_SPRING, ANGLE_SPRING, HERMITE = 0x8008A998, 0x80086B1C, 0x80086D54, 0x8002FA28
ZONES, RESET_FLAGS, SPRING_RESET = 0x80088140, 0x8008AAB0, 0x80086AF8
SHOT_SETUP, SPLINE_SLOPES, RACE_OVER, GETRCNT = 0x800853E4, 0x8002F634, 0x80018C1C, 0x80043F00
FIXMUL, FIXDIV, RAND = 0x8001FC90, 0x80010028, 0x8001FC58
VIEW_DISTANCE = 0x8008DBCC
VIEW0 = 0x800CD898
VIEW_BYTES = 1132
CAM_FILE = 0x800CD7B8
SINCOS = 0x8005624C
GAME_STATE = 0x8005B2F8
PLAYER1 = 0x8005B38C
SHA1 = {"SLUS": "67ed165a2c517d4e6106fb0dfa324d66dd9a76f1",
        "G": "cfe43a7786759f2cb9c57751cf99e84d1074782c",
        "I": "9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06"}

MUTATE = {"key": None}
# inputs that make a literal observable, counted by the models while they run (never by cfg values):
# a literal whose window no traced call enters cannot be told from its mutant, so each window is a check
REACH = Counter()


def cfg(key, orig, mutated):
    return mutated if MUTATE["key"] == key else orig


# ---------------------------------------------------------------------------------------------
# THE CLAIMS - what this probe checks; `--mutate` perturbs them one at a time
# ---------------------------------------------------------------------------------------------
def claims():
    return {
        # function: (image, end, frame bytes, callee multiset)
        "tree": {
            VIEW_UPDATE: ("G", 0x8008A998, 208, {0x8001FC90: 53, 0x80010028: 10, 0x80086584: 8, 0x80087420: 4,
                                                  0x800863EC: 4, 0x8002F634: 4, 0x8002ECB8: 4, 0x8002EAD8: 4,
                                                  0x8002FA28: 3, 0x8001FCB0: 3, 0x80088140: 2, 0x80086E1C: 2,
                                                  0x8008676C: 2, 0x80043F00: 2, 0x800374D4: 2, 0x800A7BF8: 1,
                                                  0x8008AAB0: 1, 0x8008A998: 1, 0x80086B1C: 1, 0x80086AF8: 1,
                                                  0x800853E4: 1, 0x8002EE50: 1, 0x80020018: 1, 0x8001FC58: 1,
                                                  0x80018C1C: 1}),
            AIM: ("G", 0x80088140, 160, {0x8001FC90: 2, 0x800871A8: 2, 0x8002EAD8: 2, 0x80086D54: 2,
                                        0x8002E698: 2, 0x8001FC58: 1, 0x8002E6F8: 1, 0x8002E548: 1,
                                        0x8002EED8: 1, 0x80020018: 1, 0x8001FF3C: 1, 0x80086C00: 1,
                                        0x80010028: 2, 0x8004D2A4: 1, 0x8003FA18: 1}),
            ORIENT: ("G", 0x800871A8, 80, {0x8001FC90: 6, 0x80020018: 5, 0x8004CF74: 3, 0x8004D2A4: 1}),
            TRANSITION: ("G", 0x80086AF8, 104, {0x8001FC90: 10, 0x80010028: 2, 0x8002FA28: 1}),
            LEAD: ("G", 0x80087420, 56, {0x8002F0F4: 1, 0x8004CF74: 1, 0x8001FC90: 1, 0x80010028: 2,
                                         0x8002E810: 1}),
            TO_FRAME: ("G", 0x8008676C, 24, {}),
            TO_POLAR: ("G", 0x80086584, 32, {0x80020018: 1, 0x80010028: 2}),
            ANGLE_CHASE: ("G", 0x80086D54, 40, {0x8001FC90: 2}),
            ANGLE_SPRING: ("G", 0x80086E1C, 40, {0x8001FC90: 3}),
            SET_MODE: ("G", 0x8008AAB0, 0, {}),
            CHASE_SPRING: ("G", 0x80086C00, 32, {0x8001FC90: 4}),
            HERMITE: ("SLUS", 0x8002FAD4, 48, {0x8001FC90: 2}),
            ZONES: ("G", 0x800881B4, 32, {0x8003A9D8: 1}),
            RESET_FLAGS: ("G", 0x8008AB00, 0, {}),
            SPRING_RESET: ("G", 0x80086B1C, 0, {}),
        },
        # the unported callees: executed by the oracle in the bench (declared on `view_update`)
        "seams": {SHOT_SETUP, SPLINE_SLOPES, RACE_OVER, GETRCNT},
        "callers": {0x800123B8, 0x800123E0, 0x800125D4},
        "rand_sites": {0x8008A624, 0x80087724},
        "getrcnt_sites": {0x80088AC4, 0x80089808},
        # CAMERA.CA
        "cam_read": 224,
        "cam_records": 4,
        "cam_stride": 56,
        "cam_sites": {0x800859B4, 0x80086B40, 0x80087554, 0x80087EF0, 0x80087F9C, 0x800897C4, 0x8008985C,
                      0x80089988},
        "cam_fields": {"yawRate": 0, "eyeZ": (1, 2), "eyeY": (3, 4), "lookZ": (5, 6), "lagK": (7, 9),
                       "lagC": (8, 10), "springC": 11, "springK": 12, "shake": 13},
        "suffix": {(1, False): "", (1, True): "S", (2, False): "2", (2, True): "2S"},
        # the race-frame path through ViewUpdate
        "race_path": [0x80089628, 0x800897A4, 0x80089C68, 0x80089CCC, 0x80089DAC, 0x80089E50, 0x8008A5BC,
                      0x8008A958],
        "race_not": [0x800881F8, 0x80088BB0, 0x80089FF8, 0x8008A60C],
        "race_callees": [CHASE_SPRING, AIM, 0x8002ECB8, 0x8002EAD8, 0x8002ECB8, ORIENT, 0x800374D4, ZONES],
        # literals
        "lit_spring_settled": 656,
        "lit_lag_clamp": (-0x640000, 0xCCCC),
        "lit_shake_speed": 0x23C36,
        "lit_rider_far": 61,
        "lit_lead_min": 6553,
        "lit_rand_mod": 39321,
        "lit_rand_base": 13107,
        "lit_ground_lift": (0x20000, 0x1FFFF),
    }


C = claims()

# (image, address, word, what): literal words the transcription rests on
WORDS = [
    ("G", 0x800881B4, 0x27BDFF30, "addiu sp,sp,-208 - ViewUpdate's frame"),
    ("G", 0x800881E8, 0x8E820304, "lw v0,772(s4) - +0x304, the director switch"),
    ("G", 0x80088208, 0x8E85021C, "lw a1,540(s4) - +0x21C, the mode"),
    ("G", 0x8008820C, 0x8E920238, "lw s2,568(s4) - +0x238, the followed entity"),
    ("G", 0x80088B5C, 0x3C02270F, "lui v0,0x270f - the +0x2BC clock stops at 0x270FFFFF"),
    ("G", 0x800897C4, 0x2442D7B8, "addiu v0,v0,-10312 - CAMERA.CA at 0x800CD7B8"),
    ("G", 0x800899B0, 0x2442D7B8, "addiu v0,v0,-10312 - the ninth CAMERA.CA site, sharing 0x80089980's lui"),
    ("G", 0x800897F8, 0x34423C36, "ori v0,v0,0x3c36 - the shake speed threshold 0x23C36"),
    ("G", 0x80089838, 0x3C05003C, "lui a1,0x3c - speed / 60.0"),
    ("G", 0x80089A50, 0x3C040064, "lui a0,0x64 - the lag clamp -100.0"),
    ("G", 0x80089A58, 0x3403CCCC, "li v1,0xcccc - the lag clamp 0.8"),
    ("G", 0x800896A4, 0x2842003D, "slti v0,v0,61 - the rider-distance reset threshold"),
    ("G", 0x80086BB8, 0x28630290, "slti v1,v1,656 - 'the distance spring has settled'"),
    ("G", 0x80087730, 0x34638AAB, "ori v1,v1,0x8aab - Rand / 39321 (0x35558AAB)"),
    ("G", 0x8008775C, 0x24423333, "addiu v0,v0,13107 - the lead factor floor 0.2"),
    ("G", 0x80087F64, 0x344286A7, "ori v0,v0,0x86a7 - the angle spring stiffness 0x186A7"),
    ("G", 0x80087F7C, 0x2402753F, "li v0,30015 - the angle spring damping"),
    ("G", 0x80087908, 0x24064666, "li a2,18022 - the look point raised 0.275 for kinds 6..8, 15..17"),
    ("G", 0x80087C34, 0x2A42028F, "slti v0,s2,655 - 'the rider is on the bike' distance"),
    ("G", 0x800872B0, 0x3C020001, "lui v0,0x1 - with 0x800872B4 `ori v0,v0,0xffff`: the lead snaps below 0x1FFFF"),
    ("G", 0x800872B4, 0x3442FFFF, "ori v0,v0,0xffff - (the delay slot of 0x800872AC makes it 0x1FFFF, not 0xFFFF)"),
    ("G", 0x80087358, 0x2A021999, "slti v0,s0,6553 - the lead's minimum length"),
    ("G", 0x80087220, 0x3C05000E, "lui a1,0xe - the lead for a huge distance, 14.0"),
    ("G", 0x8008A630, 0x3463FFFF, "ori v1,v1,0xffff - Rand & 0x1FFFF, the ground-settle lift"),
    ("G", 0x8008A8F4, 0x3442C9C4, "ori v0,v0,0xc9c4 - the settle speed 0x1C9C4"),
    ("G", 0x8008A78C, 0x2409E667, "li t1,-6553 - the road-normal clamp"),
    ("I", 0x80069664, 0x240600E0, "li a2,224 - CAMERA.CA: only the first 224 bytes are read"),
    ("I", 0x80069660, 0x24A5D7B8, "addiu a1,a1,-10312 - ... into 0x800CD7B8"),
    ("I", 0x800675B4, 0x24020002, "li v0,2 - two players select the '2' names"),
    ("I", 0x800676F8, 0x2406009F, "li a2,159 - view 0's handle 0x9F (SLUS 0x8002F308)"),
]

# the ViewUpdate blocks probed for the path census
PATH_PROBES = [0x800881F8, 0x80088B58, 0x80088BB0, 0x80089214, 0x80089628, 0x8008979C, 0x800897A4,
               0x80089808, 0x80089914, 0x80089AA0, 0x80089AD0, 0x80089C68, 0x80089C88, 0x80089C9C,
               0x80089CCC, 0x80089DAC, 0x80089E50, 0x80089FF8, 0x8008A5BC, 0x8008A60C, 0x8008A958]


# ---------------------------------------------------------------------------------------------
# images
# ---------------------------------------------------------------------------------------------
_IM = {}


def images():
    if not _IM:
        _IM["SLUS"] = E.load_exe()
        _IM["G"] = E.load_overlay("RASHCDG.BIN", OVL_BASE)
        _IM["I"] = E.load_overlay("RASHCDI.BIN", OVL_BASE)
    return _IM


def word(img, a):
    return images()[img].word(a)


def jals(img, lo, hi):
    out = []
    for a in range(lo, hi, 4):
        w = word(img, a)
        if (w >> 26) == 3:
            out.append((a, ((a + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)))
    return out


def all_jals_to(target):
    out = []
    for name in ("SLUS", "G"):
        im = images()[name]
        for a, w in im.words():
            if (w >> 26) == 3 and (((a + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)) == target:
                out.append(a)
    return out


def is_cop2(w):
    op = w >> 26
    return op in (0x12, 0x32, 0x3A)


def rd(ram, a, fmt="<i"):
    return struct.unpack_from(fmt, ram, a & 0x1FFFFF)[0]


def load_ported():
    """the bench's row table: entry -> row name (ported), read, never typed"""
    src = ""
    for f in [os.path.join(ROOT, "tools", "rrverify", "verify_physics.cpp")] + \
            sorted(glob.glob(os.path.join(ROOT, "tools", "rrverify", "rows_*.inc"))):
        try:
            src += open(f, encoding="utf-8", errors="replace").read()
        except OSError:
            pass
    consts = {m.group(1): int(m.group(2), 16) for m in re.finditer(r"\b(k\w+)\s*=\s*(0x[0-9A-Fa-f]+)", src)}
    out = {}
    for m in re.finditer(r'(?:Row\{|CamRow\(|AnimRow\(|idRow\(|nextRow\(|neighbourRow\()\s*"(\w+)",\s*"[^"]*",\s*'
                         r'(?:[\w:]+::)?(k\w+|0x[0-9A-Fa-f]+)', src):
        ref = m.group(2)
        a = int(ref, 16) if ref.startswith("0x") else consts.get(ref)
        if a is not None and m.group(1) != "sound_emitter_is_invisible":
            out.setdefault(a, m.group(1))
    return out, src, consts


def declared_seams(src, consts):
    """the oracle callees rows_camera.inc declares on `view_update`"""
    cam = open(os.path.join(ROOT, "tools", "rrverify", "rows_camera.inc"), encoding="utf-8").read()
    i = cam.find('"view_update"')
    seg = cam[i:]
    out = set()
    for m in re.finditer(r"OracleCallee\{(k\w+)", seg):
        if m.group(1) in consts:
            out.add(consts[m.group(1)])
    return out


# ---------------------------------------------------------------------------------------------
# the fixed-point leaves, re-derived (and each checked against the interpreter's answers)
# ---------------------------------------------------------------------------------------------
def fixmul(a, b):
    return u32((s32(a) * s32(b)) >> 16)


def fixdiv(a, b):
    d = u32(b) >> 1
    q = 0xFFFFFFFF if d == 0 else 0x80000000 // d
    return u32((u32(a) * q) >> 16)


def sdiv(a, b):
    a, b = s32(a), s32(b)
    if a > 0:
        return fixdiv(a, b) if b > 0 else u32(-s32(fixdiv(a, -b)))
    return u32(-s32(fixdiv(-a, b))) if b > 0 else fixdiv(-a, -b)


def approx_len3(x, y, z):
    v = sorted((iabs(x), iabs(y), iabs(z)))
    mx, s = v[2], v[0] + v[1]
    return s32(mx - (mx >> 4) + (s >> 2) + (s >> 3))


class Bench:
    def __init__(self):
        self.n = 0
        self.fail = 0
        self.failed_ids = set()

    def check(self, ok, what, cid=None):
        self.n += 1
        if not ok:
            self.fail += 1
            self.failed_ids.add(cid or what)
            print("  FAIL", what)
        return ok


# ---------------------------------------------------------------------------------------------
# SKELETON
# ---------------------------------------------------------------------------------------------
def check_skeleton(bench, verbose=True):
    im = images()
    for k, h in SHA1.items():
        bench.check(im[k].sha1 == h, f"sha1 {k} {im[k].sha1}", "sha1")
    tree = cfg("tree", C["tree"], {**C["tree"], ORIENT: ("G", 0x800871A8, 80, {0x8001FC90: 6, 0x80020018: 4,
                                                                          0x8004CF74: 3, 0x8004D2A4: 1})})
    total = 0
    for f, (img, end, frame, callees) in sorted(tree.items()):
        n = (end - f) // 4
        total += n
        census = Counter(t for _, t in jals(img, f, end))
        bench.check(census == Counter(callees), f"callees of {f:#x}: {dict(census)}", "tree")
        if frame:
            pro = [a for a in range(f, min(f + 16, end), 4) if word(img, a) == u32(0x27BD0000 | (-frame & 0xFFFF))]
            bench.check(bool(pro), f"{f:#x}: addiu sp,sp,-{frame} in its prologue", "tree")
            bench.check(word(img, end - 4) == u32(0x27BD0000 | frame) and word(img, end - 8) == 0x03E00008,
                        f"{f:#x}: jr ra; addiu sp,sp,{frame} ends at {end:#x}", "tree")
        else:
            rets = [a for a in range(f, end, 4) if word(img, a) == 0x03E00008]
            bench.check(bool(rets) and rets[-1] == end - 8, f"{f:#x}: a leaf ending at {end:#x}", "tree")
        cop2 = [a for a in range(f, end, 4) if is_cop2(word(img, a))]
        bench.check(not cop2, f"{f:#x}: no COP2 instruction ({len(cop2)})", "cop2")
    if verbose:
        print(f"  tree: {len(tree)} functions, {total} instructions")
    callers = set(all_jals_to(VIEW_UPDATE))
    bench.check(callers == cfg("callers", C["callers"], C["callers"] | {0x80012600}),
                f"callers of ViewUpdate {sorted(hex(a) for a in callers)}", "callers")
    in_tree = lambda a: any(f <= a < e for f, (_, e, _, _) in C["tree"].items())
    rand = {a for a in all_jals_to(RAND) if in_tree(a)}
    bench.check(rand == cfg("rand_sites", C["rand_sites"], {0x8008A624}), f"Rand sites {sorted(map(hex, rand))}",
                "rand_sites")
    grc = {a for a in all_jals_to(GETRCNT) if in_tree(a)}
    bench.check(grc == cfg("getrcnt_sites", C["getrcnt_sites"], {0x80089808}), f"GetRCnt sites {sorted(map(hex, grc))}",
                "getrcnt_sites")
    words = cfg("words", WORDS, WORDS[:-1] + [("I", 0x800676F8, 0x2406009E, "mutated")])
    for img, a, w, what in words:
        bench.check(word(img, a) == w, f"{img} {a:#x}: {what}", "words")
    # every callee: ported (a bench row) or a declared seam, and nothing else
    ported, src, consts = load_ported()
    seams = declared_seams(src, consts)
    want_seams = cfg("seams", C["seams"], C["seams"] - {GETRCNT})
    bench.check(seams == want_seams, f"view_update declares {sorted(map(hex, seams))} as oracle callees", "seams")
    status = {}
    for f, (img, end, frame, callees) in tree.items():
        for t in callees:
            status[t] = "seam" if t in want_seams else ported.get(t)
    missing = sorted(hex(t) for t, s in status.items() if s is None)
    bench.check(not missing, f"every callee of the tree is ported or a seam (unaccounted: {missing})", "status")
    rows = {f: ported.get(f) for f in tree}
    unrowed = sorted(hex(f) for f, r in rows.items() if r is None)
    bench.check(not unrowed, f"all fifteen functions have a bench row (missing {unrowed})", "rows")
    if verbose:
        print("  callee status:")
        for t in sorted(status):
            print(f"    {t:#010x}  {status[t]}")
        print("  rows:", ", ".join(f"{f:#x}={r}" for f, r in sorted(rows.items())))


# ---------------------------------------------------------------------------------------------
# CAMERA.CA
# ---------------------------------------------------------------------------------------------
def parse_camera(data):
    n = cfg("cam_records", C["cam_records"], 3)
    stride = C["cam_stride"]
    out = []
    for r in range(n):
        w = struct.unpack_from("<14i", data, stride * r)
        f = C["cam_fields"]
        out.append({"yawRate": w[f["yawRate"]], "eyeZ": tuple(w[i] for i in f["eyeZ"]),
                    "eyeY": tuple(w[i] for i in f["eyeY"]), "lookZ": tuple(w[i] for i in f["lookZ"]),
                    "lagK": tuple(w[i] for i in cfg("lagK", f["lagK"], (7, 8))),
                    "lagC": tuple(w[i] for i in f["lagC"]), "springC": w[f["springC"]],
                    "springK": w[f["springK"]], "shake": w[f["shake"]], "raw": w})
    return out


def find_rrtool():
    for b in ("build_m0", "build"):
        exe = os.path.join(ROOT, b, "rrtool.exe")
        if not os.path.exists(exe):
            continue
        r = subprocess.run([exe, "camera", DISC_BIN], capture_output=True, text=True)
        if r.returncode == 0 and "record 0" in r.stdout:
            return exe, r.stdout
    return None, ""


def check_camera_file(bench, verbose=True):
    files = {}
    for name in ("CAMERA.CA", "CAMERAS.CA", "CAMERA2.CA", "CAMERA2S.CA"):
        p = os.path.join(E.DISC, "DATA", name)
        files[name] = open(p, "rb").read()
    bench.check(len(files["CAMERA.CA"]) == 1128 and all(len(files[n]) == 416 for n in files if n != "CAMERA.CA"),
                f"sizes {[len(v) for v in files.values()]}", "cam_sizes")
    # the loader: `li a2,224` into 0x800CD7B8 (WORDS), and the four records the camera indexes by mode
    read = cfg("cam_read", C["cam_read"], 256)
    bench.check(read == C["cam_records"] * C["cam_stride"], f"{read} bytes read = 4 x 56", "cam_read")
    # every CAMERA.CA read site: `lui 0x800d; addiu -10312` preceded by the x56 (`sll 3; subu; sll 3`)
    sites = set()
    g = images()["G"]
    for a, w in g.words():
        if w & 0xFFE00000 == 0x24400000 and False:
            pass
    for x in E.scan_xrefs(g, CAM_FILE):
        sites.add(x["second"])
    bench.check(sites == cfg("cam_sites", C["cam_sites"], C["cam_sites"] - {0x80086B40}),
                f"CAMERA.CA address formed at {sorted(map(hex, sites))}", "cam_sites")
    x56 = 0
    for s in sorted(sites):
        blk = [word("G", a) for a in range(s - 40, s + 44, 4)]
        has = any(b & 0xFC00003F == 0x00000000 and (b >> 6) & 31 == 3 for b in blk) and \
            any(b & 0xFC0007FF == 0x00000023 for b in blk)
        x56 += has
    bench.check(x56 == len(sites), f"{x56} of {len(sites)} sites index it by x*56",
                "cam_stride")
    # the name rule, re-derived from the RASHCDI strings: "2S", "2", "S", "" at 0x8005B900..
    ib = images()["I"]
    strs = [ib.data[ib.off(a):ib.off(a) + 4].split(b"\0")[0].decode() for a in (0x8005B900, 0x8005B904, 0x8005B908,
                                                                                0x8005B90C)]
    want = cfg("suffix", C["suffix"], {**C["suffix"], (2, True): "S2"})
    bench.check(strs == [want[(2, True)], want[(2, False)], want[(1, True)], want[(1, False)]],
                f"name suffixes {strs}", "suffix")
    parsed = {n: parse_camera(d) for n, d in files.items()}
    exe, out = find_rrtool()
    bench.check(exe is not None, "rrtool camera (the C++ parser) runs", "rrtool")
    if exe:
        cur, got = None, defaultdict(list)
        for ln in out.splitlines():
            m = re.match(r"DATA/(\S+) (\d+) bytes", ln)
            if m:
                cur = m.group(1)
                continue
            m = re.match(r"\s+record (\d+) (.*)", ln)
            if m and cur:
                nums = list(map(int, re.findall(r"-?\d+", m.group(2))))
                got[cur].append(nums)
        agree = 0
        for n, recs in parsed.items():
            for r, rec in enumerate(recs):
                mine = [rec["yawRate"], *rec["eyeZ"], *rec["eyeY"], *rec["lookZ"], *rec["lagK"], *rec["lagC"],
                        rec["springC"], rec["springK"], rec["shake"]]
                agree += (r < len(got[n]) and got[n][r] == mine)
        bench.check(agree == 16 and sum(len(v) for v in got.values()) == 16,
                    f"C++ parser agrees with the probe on {agree} of 16 records", "rrtool")
        names = dict(re.findall(r"players (\d kind \d+): (\S+)", out))
        rule_ok = all(names.get(f"{p} kind {k}") == "DATA\\CAMERA" + want[(p, k in (6, 8, 15, 17))] + ".CA"
                      for p in (1, 2) for k in (0, 6, 8, 9, 15, 17, 18))
        bench.check(rule_ok, "C++ CameraFileName follows the loader's rule", "suffix")
    # resident bytes = the file's first 224, in every race image
    imgs = sorted(glob.glob(os.path.join(STATE_DIR, "*", "ram.bin"))) + sorted(glob.glob(os.path.join(DUMP_DIR, "*.bin")))
    same = 0
    races = 0
    for p in imgs:
        ram = open(p, "rb").read()
        if rd(ram, VIEW0 + 0x238, "<I") < 0x80000000 or rd(ram, VIEW0 + 0x304) != 0:
            continue
        races += 1
        o = CAM_FILE & 0x1FFFFF
        same += ram[o:o + read] == files["CAMERA.CA"][:read]
    bench.check(races == 18 and same == races, f"CAMERA.CA resident byte for byte in {same} of {races} race images",
                "cam_resident")
    if verbose:
        for r, rec in enumerate(parsed["CAMERA.CA"]):
            print(f"  CAMERA.CA record {r}: eyeZ {rec['eyeZ'][0] / 65536:+.3f}/{rec['eyeZ'][1] / 65536:+.3f} "
                  f"eyeY {rec['eyeY'][0] / 65536:+.3f} lookZ {rec['lookZ'][0] / 65536:+.3f} "
                  f"spring k {rec['springK'] / 65536:.1f} c {rec['springC'] / 65536:.1f}")
    return parsed


# ---------------------------------------------------------------------------------------------
# LIVE: the traced runs
# ---------------------------------------------------------------------------------------------
WATCHES = [(0x800CC000, 0xE000, "views"), (0x801B3000, 0xB400, "pools"), (0x8005A000, 0x2000, "glob"),
           (0x801FF000, 0x1000, "stack")]


def view_edit(fields):
    """edits of view record 0: {offset: (and_mask, or_value)} or {offset: value}"""
    def f(ram):
        out = []
        for off, v in fields.items():
            a = VIEW0 + off
            old = rd(ram, a, "<I")
            new = (old & v[0]) | v[1] if isinstance(v, tuple) else u32(v)
            out.append((a, None, new))
        return out
    return f


def far_rider(ram):
    """the eye 80 units away from a rider in state 3, and the crash-camera bit: ResetFlags' trigger"""
    bike = rd(ram, VIEW0 + 0x238, "<I")
    rider = rd(ram, bike + 0x354, "<I")
    return view_edit({0x224: (0xFFFFFFFF, 0x8000), 0xB8: u32(rd(ram, VIEW0 + 0xB8) + (80 << 16))})(ram) +         [(rider + 0x25C, None, 3)]


def kick(ram):
    """the three followers off their rest: the distance spring displaced so that its first
    acceleration -FixMul(k, x) is -628 (inside [600, 656), where 'settled' depends on the literal 656), its
    rate zeroed; the speed-lag +0x2E0 at 1.0 (above the clamp 0.8); the yaw +0x260 15 units off its target
    (rr-race's yaw sits on it), whose first chase step is then exactly 3 (rate 7.0, dt 0x884)"""
    v = VIEW0
    k = rd(ram, CAM_FILE + 56 * rd(ram, v + 0x21C) + 48)
    x = (628 << 16) // k
    while s32(fixmul(k, x)) < 628:
        x += 1
    bike = rd(ram, v + 0x238, "<I")
    xoff = 0x2E4 if rd(ram, bike + 0x234, "<I") & 0x800 else 0x2DC      # 0x80089914: the spring's x
    return view_edit({xoff: x, 0x2D0: 0, 0x2E0: 0x10000, 0x260: u32(rd(ram, v + 0x260) - 15)})(ram)


LEAD_BIKE = 6   # rr-race's bike slot 6: stopped; its +0xB8 is re-placed unchanged and its +0x1F8 never written


def lead_base(ram):
    """rider-follow (+0x224 bit 15) with a lead factor +0x460 already drawn (0x4DFF, the rider run's Rand
    draw), following bike slot 6 (stopped). The eye is moved next to it (1.7 above), or the rider-distance
    test (0x800896A4, 61) re-cuts through CameraResetFlags, which clears bit 15"""
    e = rd(ram, 0x8005B3A0, "<I") + 1096 * LEAD_BIKE
    eye = {0xB8 + 4 * i: u32(rd(ram, e + 0xB8 + 4 * i) - (0x1B333 if i == 1 else 0)) for i in range(3)}
    return view_edit({0x224: (0xFFFFFFFF, 0x8000), 0x238: e, 0x460: 0x4DFF, **eye})(ram)


def lead_near(ram):
    """lead_base with bike 6 shorter. A stopped bike is placed every frame as +0xB8 = MulAdd(+0x1F8, up
    +0x20A, +0x304) (0x8007B4F0), with +0x304 = min(+0x304 + 0.1, -(+0x130) / 2) (0x8007CB78), so
    |+0xB8 - +0x1F8| is its half height: 0.18 as captured, 11824 > 6553. Its +0x130 set to 12600 makes
    it ~6300, and CameraLead's length then lies in [6000, 6553), where the minimum length 6553 decides"""
    e = rd(ram, 0x8005B3A0, "<I") + 1096 * LEAD_BIKE
    return lead_base(ram) + [(e + 0x130, None, 12600)]


def recut_side(ram):
    """the re-cut, with the eye 1.5 units to the side: ToPolar then sees a yaw far from 0"""
    return view_edit({0x224: (~0x44, 2), 0xB8: u32(rd(ram, VIEW0 + 0xB8) + 0x18000)})(ram)


def no_zones(ram):
    """one side of the view's road record +0x1EC without roadside zones (the count byte of that side, 0).
    RoadsideZones runs twice on the view per frame: inside RoadRebind with the lateral +0x158 as is, then
    from CameraZones with it negated, and SLUS 0x8003AA9C picks the record's side (+0 or +2, count at +1)
    by that sign. Emptying only the side the CameraZones call reads leaves bit 8 set by the first call and
    cleared by the second, and CameraZones must OR the old bit back"""
    rec = rd(ram, VIEW0 + 0x1EC, "<I")
    cnt = rec + (1 if rd(ram, VIEW0 + 0x158) > 0 else 3)     # the side of the NEGATED lateral
    a = cnt & ~3
    return [(a, None, rd(ram, a, "<I") & ~(0xFF << (8 * (cnt & 3))))]


RUNS = [  # name, source, frames, edit, pad (active low)
    ("rr-race", "rr-race", 14, None, 0xFFFF),
    ("quick", "quick", 14, None, 0xFFFF),
    ("select", "rr-race", 10, None, 0xFFFE),
    ("mode1", "rr-race", 10, view_edit({0x21C: 1, 0x220: 1, 0x224: (~0x18, 6)}), 0xFFFF),
    ("mode2", "rr-race", 10, view_edit({0x21C: 2, 0x220: 2, 0x224: (~0x18, 6)}), 0xFFFF),
    ("mode3", "rr-race", 10, view_edit({0x21C: 3, 0x220: 3, 0x224: (~0x18, 6)}), 0xFFFF),
    ("lookback", "rr-race", 8, None, 0xDFFF),
    ("recut", "rr-race", 16, view_edit({0x224: (~0x44, 2)}), 0xFFFF),
    ("rider", "rr-race", 8, view_edit({0x224: (0xFFFFFFFF, 0x8000), 0x24: (~0x100, 0)}), 0xFFFF),
    ("ground", "rr-race", 8, view_edit({0x224: (0xFFFFFFFF, 0x80000)}), 0xFFFF),
    ("director", "rr-race", 16, view_edit({0x304: 1, 0x21C: 7, 0x228: 0, 0x310: 0x7FFF0000}), 0xFFFF),
    ("leave", "rr-race", 6, view_edit({0x304: 1, 0x21C: 12, 0x220: 0, 0x228: 0, 0x2F4: VIEW0 + VIEW_BYTES + 0x24}),
     0xFFFF),
    ("reset", "rr-race", 6, far_rider, 0xFFFF),
    ("kick", "rr-race", 6, kick, 0xFFFF),
    ("lead", "rr-race", 6, lead_near, 0xFFFF),
    ("side", "rr-race", 6, recut_side, 0xFFFF),
    ("zones", "rr-race", 6, no_zones, 0xFFFF),
]

MODELLED = {}  # filled below: address -> (model, frame, end, image)


def probe_points():
    ps = set(PATH_PROBES) | {VIEW_UPDATE, VIEW_DISTANCE, SHOT_SETUP}
    for f, (img, end, frame, _) in C["tree"].items():
        ps.add(f)
        ps.update(a + 8 for a, _ in jals(img, f, end))
    for f in list(C["tree"]) + [VIEW_UPDATE]:
        ps.update(a + 8 for a in all_jals_to(f))
    return sorted(ps)


def prepare(name, src, edit):
    dst = os.path.join(WORK, "state-" + name)
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    shutil.copytree(os.path.join(STATE_DIR, src), dst)
    p = os.path.join(dst, "ram.bin")
    ram = bytearray(open(p, "rb").read())
    edits = edit(bytes(ram)) if edit else []
    for a, delta, value in edits:
        struct.pack_into("<I", ram, a & 0x1FFFFF, u32(value))
    open(p, "wb").write(ram)
    return dst, bytes(ram), edits


class Trace(P.Trace):
    def __init__(self, outdir, ram):
        super().__init__(outdir, ram)
        self.reads = []
        for r in csv.DictReader(open(os.path.join(outdir, "watch.csv"))):
            if r["kind"] == "read":
                self.reads.append((int(r["seq"]), int(r["pc"], 16), int(r["address"], 16), int(r["size"])))


_TRACES = {}


def run_trace(name, src, frames, edit, pad=0xFFFF):
    if name in _TRACES:
        return _TRACES[name]
    os.makedirs(WORK, exist_ok=True)
    state, ram, edits = prepare(name, src, edit)
    out = os.path.join(WORK, "tr-" + name)
    cmd = [RRVERIFY, "trace", "--state", state, "--frames", str(frames), "--no-cop2", "--no-gpu",
           "--pad", f"{pad:#06x}", "--calls", "--out", out]
    for a, n, w in WATCHES:
        cmd += ["--watch", f"{a:#x}:{n}:{w}"]
    for p in probe_points():
        cmd += ["--probe", f"{p:#x}:p{p:x}"]
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
    stop = [ln.split(None, 1)[1].strip() for ln in r.stdout.splitlines() if ln.startswith(("stopped", "buffer swaps"))]
    tr = Trace(out, ram)
    tr.name, tr.stop, tr.edits = name, stop, edits
    _TRACES[name] = tr
    return tr


def calls_of(tr, f):
    """[(entry seq, regs, exit seq, exit regs)] for every call of f"""
    rets = [a + 8 for a in all_jals_to(f)]
    out = []
    for seq, regs in tr.probes.get(f, []):
        ex = None
        for rs in rets:
            p = tr.post(rs, seq, regs["sp"])
            if p and (ex is None or p[0] < ex[0]):
                ex = p
        out.append((seq, regs, ex[0] if ex else None, ex[1] if ex else None))
    return out


def run_model(tr, f, model):
    """P.run_model with explicit extents and return sites (SLUS and RASHCDG callers)"""
    img, end, frame, _ = C["tree"][f]
    out = []
    base = bytearray(tr.ram0)
    bi = 0
    W = tr.writes
    for seq, regs, exs, exr in calls_of(tr, f):
        if exs is None:
            out.append((seq, None, "truncated"))
            continue
        while bi < len(W) and W[bi][0] < seq:
            P.put(base, W[bi][2], W[bi][3], W[bi][4])
            bi += 1
        m = P.Machine(tr, bytearray(base), seq, exs, regs, f, end, frame)
        m.cover = set()
        try:
            v0 = model(m, regs["a0"], regs["a1"], regs["a2"], regs["a3"])
            i0, i1 = bisect.bisect_right(tr.wseq, seq), bisect.bisect_left(tr.wseq, exs)
            g = [(a, n, v) for sq, pc, a, n, v in W[i0:i1]
                 if f <= pc < end and not any(s0 < sq < s1 and pc != d for s0, s1, d in m.windows)
                 and not (m.sp <= a < regs["sp"])]
            mw = [w for w in m.mwrites if not (m.sp <= w[0] < regs["sp"]) and watched(w[0])]
            if m.ci != len(m.gcalls):
                c = m.gcalls[m.ci]
                raise Mismatch(f"model made {m.ci} calls, guest {len(m.gcalls)} (next {c[2]:#x} from {c[1]:#x})")
            if mw != g:
                i = next((i for i, (x, y) in enumerate(zip(mw, g)) if x != y), min(len(mw), len(g)))
                raise Mismatch(f"write #{i}: model {[(hex(a), n, hex(v)) for a, n, v in mw[i:i + 2]]} "
                               f"guest {[(hex(a), n, hex(v)) for a, n, v in g[i:i + 2]]} ({len(mw)} vs {len(g)})")
            if v0 is not None and u32(v0) != exr["v0"]:
                raise Mismatch(f"v0 model {u32(v0):#x} guest {exr['v0']:#x}")
            out.append((seq, True, f"{len(g)} writes, {m.ci} calls"))
        except Mismatch as e:
            out.append((seq, False, str(e)))
    return out


def watched(a):
    return any(lo <= a < lo + n for lo, n, _ in WATCHES)


# ---------------------------------------------------------------------------------------------
# the models (every address RASHCDG unless marked)
# ---------------------------------------------------------------------------------------------
def fm(m, a, b):
    """a FixMul call: the guest's answer, checked against the re-derived one"""
    v = m.call(FIXMUL, a, b)
    if v != fixmul(a, b):
        raise Mismatch(f"FixMul({u32(a):#x},{u32(b):#x}) = {v:#x}, re-derived {fixmul(a, b):#x}")
    return v


def fd(m, a, b):
    v = m.call(FIXDIV, a, b)
    if v != fixdiv(a, b):
        raise Mismatch(f"FixDiv({u32(a):#x},{u32(b):#x}) = {v:#x}, re-derived {fixdiv(a, b):#x}")
    return v


def m_sdiv(m, a, b):
    """the sign pattern round FixDiv (0x80089040 and others)"""
    a, b = s32(a), s32(b)
    if a > 0:
        return fd(m, a, b) if b > 0 else u32(-s32(fd(m, a, -b)))
    return u32(-s32(fd(m, -a, b))) if b > 0 else fd(m, -a, -b)


def cam_rec(m, mode):
    return u32(CAM_FILE + mode * cfg("cam_stride", 56, 52))


def m_chase_spring(m, v, dt, x, a3):                               # 0x80086B1C
    rec = cam_rec(m, m.lw(v + 0x21C))
    kx = fm(m, m.lw(rec + cfg("spring_k_off", 48, 44)), x)
    cv = fm(m, m.lw(v + 0x2D0), m.lw(rec + 44))
    acc = u32(-s32(kx) - s32(cv))
    m.sw(v + 0x2C4, acc)
    rate = u32(m.lw(v + 0x2D0) + fm(m, acc, dt))
    m.sw(v + 0x2D0, rate)
    m.sw(v + 0x2DC, u32(m.lw(v + 0x2DC) + fm(m, rate, dt)))
    f = m.lw(v + 0x224)
    if 600 <= iabs(m.lws(v + 0x2C4)) < C["lit_spring_settled"]:
        REACH["spring_window"] += 1
    settled = iabs(m.lws(v + 0x2C4)) < cfg("lit_spring_settled", C["lit_spring_settled"], 600)
    m.sw(v + 0x224, (f | 0x800000) if settled else (f & 0xFF7FFFFF))


def m_spring_reset(m, v, a1, a2, a3):                              # 0x80086AF8
    speed = m.lw(m.lw(v + 0x238) + 0x1E0)
    for o in (0x2E0, 0x2D0, 0x2DC, 0x2C4):
        m.sw(v + o, 0)
    m.sw(v + 0x2D4, speed)


def m_angle_chase(m, p, target, dt, rate):                         # 0x80086C00
    t = s32(target)
    p0 = m.lws(p)
    d0 = s32(t - p0)
    wrap = -4096 if d0 < -2048 else 0
    m.sw(p, u32(p0 + 4096 + wrap) if d0 >= 2049 else u32(p0 + wrap))
    pp = m.lws(p)
    d = s32(t - pp)
    step = 0
    if rate:
        step = s32(fm(m, fm(m, rate, u32(d << 16)), dt)) >> 16
    if iabs(step) == 3:
        REACH["chase_step3"] += 1
    # the floor 3 (`slti 3`): a step of 3 is taken as is, below it the chase moves 2. Its mutant is 4
    # (a step of exactly 3 then moves 2); the mutant 2 is unobservable
    if iabs(step) >= cfg("chase_floor", 3, 4):
        a0 = s32(step + pp)
    elif d > 0:
        a0 = s32(pp + 2)
    elif d < 0:
        a0 = s32(pp - 2)
    else:
        a0 = pp
    clamp = (a0 < t and not (pp < t)) or (not (a0 < t) and not (t < pp) and t < a0)
    m.sw(p, u32(t if clamp else a0))


def m_angle_spring(m, p, target, dt, rate):                        # 0x80086D54, k and c at sp+16/+20
    k, c = m.arg(4), m.arg(5)
    t = s32(target)
    p0 = m.lws(p)
    d0 = s32(t - p0)
    wrap = -4096 if d0 < -2048 else 0
    m.sw(p, u32(p0 + 4096 + wrap) if d0 >= 2049 else u32(p0 + wrap))
    spring = fm(m, k, u32(s32(t - m.lws(p)) << 16))
    damp = fm(m, c, m.lw(rate))
    r = u32(m.lw(rate) + s32(spring) - s32(damp))
    m.sw(rate, r)
    m.sw(p, u32(m.lws(p) + (s32(fm(m, r, dt)) >> 16)))


def m_hermite(m, t, h00, h01, h10):                                # SLUS 0x8002FA28, h11 at sp+16
    h11 = m.arg(4)
    t2 = fm(m, t, t)
    t3 = fm(m, t2, t)
    a = u32(t3 - t2)
    m.sw(h11, a)
    b = u32(a - t2)
    m.sw(h10, b)
    c = u32(-s32(b) - s32(m.lw(h11)))
    m.sw(h01, c)
    m.sw(h00, u32(0x10000 - c))
    m.sw(h10, u32(m.lw(h10) + t))


def m_lead(m, a, b, k, v):                                         # 0x800871A8, out at sp+16
    out = m.arg(4)
    d = [u32(m.lw(a + 4 * i) - m.lw(b + 4 * i)) for i in range(3)]
    ss = m.call(0x8002F0F4, u32(m.sp + 16))
    length, want = s32(ss), 0xE0000
    if not (0x3FFEFFFF < length):
        length = s32(u32(m.call(0x8004CF74, ss) << 2))
        want = s32(fm(m, k, length))
    base = 0x70000 + (-0x30000 if (m.lw(v + 0x24) >> 8) & 1 else 0)
    o = m.lws(v + 0x468)
    if o:
        x = s32(o * 7 + base)
        m.sw(v + 0x468, u32((x + 7 if x < 0 else x) >> 3))
    else:
        m.sw(v + 0x468, u32(base))
    cap = m.lws(v + 0x468)
    cur = m.lws(v + 0x464)
    if want < cap:
        if cur == 0 or (not (0x1FFFF < want) and (m.lw(v + 0x228) & 0x400)):
            m.sw(v + 0x464, u32(want))
            m.sw(v + 0x228, m.lw(v + 0x228) | 0x400)
        else:
            x = s32(cur + 3 * want)
            m.sw(v + 0x464, u32((x + 3 if x < 0 else x) >> 2))
            m.sw(v + 0x228, m.lw(v + 0x228) & ~0x400)
    else:
        if cur:
            x = s32(3 * cur + cap)
            m.sw(v + 0x464, u32((x + 3 if x < 0 else x) >> 2))
        else:
            m.sw(v + 0x464, u32(cap))
    if 6000 <= length < C["lit_lead_min"]:
        REACH["lead_window"] += 1
    if length < cfg("lit_lead_min", C["lit_lead_min"], 6000):
        length = C["lit_lead_min"]
    q = m_sdiv(m, m.lw(v + 0x464), length)
    m.call(0x8002E810, q, u32(m.sp + 16), u32(m.sp + 16))
    for i in range(3):
        m.sw(out + 4 * i, u32(m.lw(b + 4 * i) + fixmul(q, d[i])))


def m_set_mode(m, v, mode, a2, a3):                                # 0x8008A998
    same = mode == m.lw(v + 0x220)
    if mode < 4 and m.lw(v + 0x21C) != mode:
        m.sw(v + 0x220, mode)
        m.sw(v + 0x224, (m.lw(v + 0x224) & ~0x18) | (2 if same else 6))
        t = m.lw(v + 0x238)
        if m.lhu(t + 0xAC) >> 5:
            m.sw(v + 0x238, m.lw(0x8005B268 + 4 * (1 if m.lhu(v + 0xAC) == 0x9E else 0)))
    f228 = m.lw(v + 0x228)
    m.sw(v + 0x304, 0 if mode < 7 else 1)
    if f228 & 1:
        m.sw(v + 0x224, m.lw(v + 0x224) | 0x100)
    a2 = m.lw(v + 0x228) & ~5
    m.sw(v + 0x228, a2)
    m.sw(v + 0x228, (a2 | 0x80) if m.lw(v + 0x21C) == 12 else (a2 & ~0x380))
    m.sw(v + 0x21C, mode)
    if same and (m.lw(v + 0x228) & 0x40):
        m.sw(v + 0x304, 1)
        m.sw(v + 0x228, m.lw(v + 0x228) | 0x10)


def m_zones(m, v, a1, a2, a3):                                     # 0x80088140
    had = (m.lw(v + 0x24) >> 8) & 1
    m.sw(v + 0x158, u32(-m.lws(v + 0x158)))
    m.call(0x8003A9D8, v)
    w = m.lw(v + 0x24)
    if had and not (w >> 8) & 1:
        REACH["zones_restore"] += 1
    bit = ((w >> 8) & 1) | cfg("zones_or", had, 0)
    m.sw(v + 0x158, u32(-m.lws(v + 0x158)))
    m.sw(v + 0x24, (w & ~0x100) | (bit << 8))


def m_to_frame(m, v, o, p, out):                                   # 0x80086584
    d = [u32(m.lw(p + 4 * i) - m.lw(o + 4 * i)) for i in range(3)]
    for k, row in enumerate((0x24C, 0x252, 0x258)):
        acc = 0
        for i in range(3):
            acc += s32(fixmul(d[i], u32(m.lhs(v + row + 2 * i) << 4)))
        m.sw(out + 4 * k, u32(acc))


def m_to_polar(m, p, a1, a2, a3):                                  # 0x800863EC
    a = s32(m.call(0x80020018, m.lw(p), m.lw(p + 8)))
    if m.lws(p + 8) < 0:
        a = a - 2048 if a >= 0 else a + 2048
    i4 = (u32(a) & 0xFFF) * 4
    if u32(iabs(a) - 513) < 1023:
        r = m_sdiv(m, m.lw(p), u32(m.lhs(SINCOS + i4) << 4))
    else:
        r = m_sdiv(m, m.lw(p + 8), u32(m.lhs(SINCOS + i4 + 2) << 4))
    m.sw(p + 8, r)
    if (a * 25736) >> 8 != (a * 25735) >> 8:
        REACH["polar_angle"] += 1
    m.sw(p, u32((a * cfg("rad_mul", 25736, 25735)) >> 8))


def m_reset_flags(m, v, a1, a2, a3):                               # 0x8008AAB0
    f = m.lw(v + 0x224)
    if f & 0x8000:
        m.sw(v + 0x224, f | 0x10006)
        f = m.lw(v + 0x224)
    if f & 0x40000:
        m.sw(v + 0x224, f | 6)
    m.sw(v + 0x224, m.lw(v + 0x224) & 0xFFF17FFF)


MODELS = {CHASE_SPRING: m_chase_spring, SPRING_RESET: m_spring_reset, ANGLE_CHASE: m_angle_chase,
          ANGLE_SPRING: m_angle_spring, HERMITE: m_hermite, LEAD: m_lead, SET_MODE: m_set_mode,
          ZONES: m_zones, TO_FRAME: m_to_frame, TO_POLAR: m_to_polar, RESET_FLAGS: m_reset_flags}


# ---------------------------------------------------------------------------------------------
# ViewUpdate as identities on its entry and exit state
# ---------------------------------------------------------------------------------------------
def snapshot_at(tr, seq):
    mem = bytearray(tr.ram0)
    for sq, pc, a, n, v in tr.writes:
        if sq >= seq:
            break
        P.put(mem, a, n, v)
    return mem


class Mem:
    def __init__(self, b):
        self.b = b

    def w(self, a):
        return struct.unpack_from("<I", self.b, a & 0x1FFFFF)[0]

    def s(self, a):
        return s32(self.w(a))

    def h(self, a):
        return struct.unpack_from("<h", self.b, a & 0x1FFFFF)[0]


def blend16to32(mem, a, b, wa, wb):
    """SLUS 0x8002ECB8: FixMul(wa, a << 4) + FixMul(wb, b << 4)"""
    return [u32(s32(fixmul(wa, u32(mem.h(a + 2 * i) << 4))) + s32(fixmul(wb, u32(mem.h(b + 2 * i) << 4))))
            for i in range(3)]


def muladd(base, mem, d, t):
    """SLUS 0x8002EAD8: base + FixMul(d << 4, t)"""
    return [u32(base[i] + s32(fixmul(u32(mem.h(d + 2 * i) << 4), t))) for i in range(3)]


def view_identities(tr, pre, post, regs, recs, tally, seq=0, exs=0):
    """ViewUpdate's own arithmetic on one race-path call; raises Mismatch"""
    A, B = Mem(pre), Mem(post)
    v, dt = regs["a0"], regs["a1"]
    bike = A.w(v + 0x238)
    mode = A.w(v + 0x21C)
    alt = A.w(v + 0x224) & 1
    rec = recs[mode]
    col = cfg("alt_column", alt, 1 - alt)
    # the targets (0x800897A4): eyeY minus the spring (or plus the shake), eyeZ plus the lag, lookZ
    if B.w(v + 0x27C) != 0 or B.w(v + 0x2A0) != 0:
        raise Mismatch("targets +0x27C/+0x2A0 not zeroed")
    shake = B.s(v + 0x2A4)
    if shake == 0:
        want = s32(rec["eyeY"][col] - B.s(v + 0x2DC))
    else:
        want = s32(rec["eyeY"][col] + shake)
        tally["shake"] += 1
    if B.s(v + 0x280) != want:
        raise Mismatch(f"+0x280 {B.s(v + 0x280):#x} want eyeY[{col}] - spring {want:#x}")
    if B.s(v + 0x2A8) != rec["lookZ"][col]:
        raise Mismatch(f"+0x2A8 {B.s(v + 0x2A8):#x} want lookZ[{col}] {rec['lookZ'][col]:#x}")
    if not alt:
        # the speed follower (0x80089974), from the entry state
        speed = A.s(bike + 0x1E0)
        k_i, c_i = (0, 0) if A.s(bike + 0x1E4) > 0 else (1, 1)
        c, k = rec["lagC"][c_i], rec["lagK"][k_i]
        f2d4, f2e0 = A.s(v + 0x2D4), A.s(v + 0x2E0)
        acc = s32(-s32(fixmul(c, f2e0)) - s32(fixmul(s32(f2d4 - speed), k)))
        if f2e0 > 0:
            acc = s32(acc - s32(fixmul(f2e0, u32(c << 1))))
        f2d4 = s32(f2d4 + s32(fixmul(acc, dt)))
        a1 = s32(f2e0 + s32(fixmul(s32(f2d4 - speed), dt)))
        lo, hi = cfg("lit_lag_clamp", C["lit_lag_clamp"], (-0x640000, 0xC000))
        if a1 > 0xC000:
            tally["lag_window"] += 1
        lag = min(max(a1, lo), hi)
        got = (B.s(v + 0x2C8), B.s(v + 0x2D4), B.s(v + 0x2E0), B.s(v + 0x284))
        if got != (acc, f2d4, lag, s32(rec["eyeZ"][col] + lag)):
            raise Mismatch(f"speed follower {tuple(map(hex, got))} want {tuple(map(hex, (acc, f2d4, lag)))}")
        tally["follower"] += 1
    else:
        if B.s(v + 0x284) != rec["eyeZ"][col]:
            raise Mismatch("+0x284 != eyeZ (look-behind)")
    # the eye (0x80089DAC): look-at + lateral x + forward z, then + up * height, all in the aim frame
    f = B.w(v + 0x224)
    if (f & 0x40004) != 0x40000:
        x, h, z = B.w(v + 0x270), B.s(v + 0x274), B.w(v + 0x278)
        # 0x80089CCC: the offset (x, z) is read there, before CameraAim; it is cartesian when +0x224 bit 5
        # is set, else POLAR: +0x270 a yaw in 16.16 radians, +0x278 the radius (x = r sin, z = r cos)
        at = [s for s, _ in tr.probes.get(0x80089CCC, []) if seq < s < exs]
        if at:
            M = Mem(snapshot_at(tr, at[0]))
            x, z = M.w(v + 0x270), M.w(v + 0x278)
            if not M.w(v + 0x224) & 0x20:
                if x:
                    i = ((x * 163) & 0xFFFFFFFF) >> 14 & 0xFFF
                    x, z = fixmul(z, u32(Mem(tr.ram0).h(SINCOS + 4 * i) << 4)), \
                        fixmul(z, u32(Mem(tr.ram0).h(SINCOS + 4 * i + 2) << 4))
                    tally["polar_eye"] += 1
                else:
                    x = 0
        if B.w(v + 0x224) & 0x1000000:
            x = u32(-s32(x))
        e = blend16to32(B, v + 0x258, v + 0x24C, z, x)
        e = muladd(e, B, v + 0x252, h)
        want = [u32(B.w(v + 0x23C + 4 * i) + e[i]) for i in range(3)]
        got = [B.w(v + 0xB8 + 4 * i) for i in range(3)]
        if B.w(v + 0x224) & 0x80000 == 0 and A.w(v + 0x224) & 0x80000 == 0:
            if got[0] != want[0] or got[2] != want[2] or (got[1] != want[1]):
                raise Mismatch(f"eye {list(map(hex, got))} want look-at + frame x offset {list(map(hex, want))}")
            tally["eye"] += 1
    # the look point (0x80089E50): look-at + (x2, 0, z2) in the aim frame (+ up * y2)
    lx, lz, ly = B.w(v + 0x294), B.w(v + 0x29C), B.s(v + 0x298)
    if B.w(v + 0x224) & 0x1000000:
        lx = u32(-s32(lx))
    lp = blend16to32(B, v + 0x258, v + 0x24C, lz, lx)
    if ly:
        lp = muladd(lp, B, v + 0x252, ly)
    want = [u32(B.w(v + 0x23C + 4 * i) + lp[i]) for i in range(3)]
    got = [B.w(v + 0x22C + 4 * i) for i in range(3)]
    if got != want:
        raise Mismatch(f"look point {list(map(hex, got))} want {list(map(hex, want))}")
    tally["look"] += 1


def path_of(tr, seq, exs):
    return [pc for pc in PATH_PROBES for s, _ in tr.probes.get(pc, []) if seq < s < exs]


def direct_callees(tr, seq, exs, sp):
    """the calls ViewUpdate itself made (its own frame), in order"""
    i0, i1 = bisect.bisect_right(tr.cseq, seq), bisect.bisect_left(tr.cseq, exs)
    return [c[2] for c in tr.calls[i0:i1] if VIEW_UPDATE <= c[1] < 0x8008A998 and c[4] == u32(sp - 208)
            and c[2] != FIXMUL]


def tree_rands(tr, seq, exs):
    i0, i1 = bisect.bisect_right(tr.cseq, seq), bisect.bisect_left(tr.cseq, exs)
    in_tree = lambda a: any(f <= a < e for f, (_, e, _, _) in C["tree"].items())
    return [c[1] for c in tr.calls[i0:i1] if c[2] == RAND and in_tree(c[1])]


def check_live(bench, recs, verbose=True):
    traces = {name: run_trace(name, src, fr, ed, pad) for name, src, fr, ed, pad in RUNS}
    for name, tr in traces.items():
        ok = any("budget" in s for s in tr.stop) or (any("MDEC" in s for s in tr.stop) and len(calls_of(tr, VIEW_UPDATE)) >= 1)
        bench.check(ok, f"{name}: the run ended on its frame budget (or in the unmodelled MDEC after 3 frames) {tr.stop}", "runs")
    # ---- the models, on every call in every run
    total = Counter()
    for name, tr in traces.items():
        for f, model in MODELS.items():
            res = run_model(tr, f, model)
            ok = [r for r in res if r[1] is True]
            bad = [r for r in res if r[1] is False]
            total[f] += len(ok)
            if bad:
                bench.check(False, f"{name}: model {f:#x}: {bad[0][2]} ({len(bad)} of {len(res)})", mid(f))
    for f in MODELS:
        need = cfg("model_min", 1, 10**6)
        bench.check(total[f] >= need, f"model {f:#x} reproduced {total[f]} calls", mid(f))
    if verbose:
        print("  models:", ", ".join(f"{f:#x} {total[f]}" for f in MODELS))
    # ---- the windows where a literal decides (else its mutant is indistinguishable): each must be entered
    for key, what in (("spring_window", "ChaseSpring: |acceleration| in [600, 656), the 'settled' literal decides"),
                      ("chase_step3", "AngleChase: a step of exactly 3, the floor literal decides"),
                      ("lead_window", "CameraLead: length in [6000, 6553), the minimum-length literal decides"),
                      ("polar_angle", "ToPolar: a yaw where 25736 and 25735 give different radians"),
                      ("zones_restore", "CameraZones: RoadsideZones cleared a set bit 8, the OR restores it")):
        bench.check(REACH[key] >= 1, f"{what}: {REACH[key]} call(s)", "reach")
    if verbose:
        print("  windows entered:", dict(REACH))
    # ---- ViewUpdate: path, callees, identities, Rand
    tally = Counter()
    race_frames = 0
    for name, tr in traces.items():
        for seq, regs, exs, exr in calls_of(tr, VIEW_UPDATE):
            if exs is None:
                continue
            pre, post = snapshot_at(tr, seq), snapshot_at(tr, exs)
            A = Mem(pre)
            path = path_of(tr, seq, exs)
            callees = direct_callees(tr, seq, exs, regs["sp"])
            rands = tree_rands(tr, seq, exs)
            tally[f"rand_{name}"] += len(rands)
            tally[f"calls_{name}"] += 1
            if A.w(regs["a0"] + 0x304) == 0 and A.w(regs["a0"] + 0x21C) < 4:
                try:
                    view_identities(tr, pre, post, regs, recs, tally, seq, exs)
                except Mismatch as e:
                    bench.check(False, f"{name} seq {seq}: {e}", "identities")
            if name in ("rr-race", "quick"):
                race_frames += 1
                want = cfg("race_path", C["race_path"], C["race_path"] + [0x8008A60C])
                bench.check(all(p in path for p in want) and not any(p in path for p in C["race_not"]),
                            f"{name} seq {seq}: race path {list(map(hex, path))}", "race_path")
                bench.check(callees == cfg("race_callees", C["race_callees"], C["race_callees"][::-1]),
                            f"{name} seq {seq}: direct callees {list(map(hex, callees))}", "race_callees")
                bench.check(not rands, f"{name} seq {seq}: no Rand in the camera ({len(rands)})", "race_rand")
            if name == "ground" and A.w(regs["a0"] + 0x224) & 0x80000:
                tally["ground_calls"] += 1
                v = regs["a0"]
                bench.check(0x8008A60C in path and not (Mem(post).w(v + 0x224) & 0x80000),
                            "ground: the settle arm ran and cleared 0x80000", "ground")
                # +0x24 bit 8 is sticky on the view (CameraZones ORs it back), so no Rand: the lift is 2.0
                bench.check(not rands and Mem(post).w(v + 0x24) & 0x100,
                            f"ground: +0x24 bit 8 set, so no Rand ({list(map(hex, rands))})", "ground")
                for sq, pc, a, n, val in tr.writes:
                    if not (seq < sq < exs and pc == 0x8008A868 and a == v + 0xBC):
                        continue
                    M = Mem(snapshot_at(tr, sq))
                    sl = M.w(v + 0x154)
                    d = [u32(M.w(v + 0xB8) - M.w(sl + 20)), u32(-M.s(sl + 24)), u32(M.w(v + 0xC0) - M.w(sl + 28))]
                    t1 = s32(M.h(sl + 10) << 4)
                    if not (t1 < -6552):
                        t1 = -6553
                    dot = u32(sum(s32(fixmul(d[i], u32(M.h(sl + 8 + 2 * i) << 4))) for i in range(3)))
                    lift = cfg("lit_ground_lift", C["lit_ground_lift"], (0x18000, 0))[0]
                    want = u32(-lift - s32(sdiv(dot, t1)))
                    bench.check(val == want, f"ground: eye y {val:#x} = the slice plane under it, lifted 2.0 "
                                             f"({want:#x})", "ground")
                    tally["ground_y"] += 1
            if name == "rider":
                v = regs["a0"]
                here = [r for r in rands if r == 0x80087724]
                if here and A.w(v + 0x460) == 0:
                    tally["rider_rand"] += len(here)
                    i0, i1 = bisect.bisect_right(tr.cseq, seq), bisect.bisect_left(tr.cseq, exs)
                    rc = [c for c in tr.calls[i0:i1] if c[2] == RAND and c[1] == 0x80087724][0]
                    post_rand = tr.post(0x8008772C, rc[0], rc[4])
                    r = post_rand[1]["v0"]
                    k = r - (r // cfg("lit_rand_mod", C["lit_rand_mod"], 39320)) * C["lit_rand_mod"] + C["lit_rand_base"]
                    bench.check(Mem(post).w(v + 0x460) == u32(k), f"rider: +0x460 {Mem(post).w(v + 0x460):#x} = "
                                f"0.2 + (Rand {r:#x} % 39321) / 65536 ({k:#x})", "rider_rand")
            if name == "director":
                if 0x800881F8 in path:
                    tally["director_calls"] += 1
    bench.check(race_frames >= 16, f"{race_frames} race-frame ViewUpdate calls censused", "race_path")
    bench.check(tally["lag_window"] >= 1, f"speed follower: {tally['lag_window']} call(s) with the lag above 0.75, "
                                          "where the clamp 0.8 decides", "reach")
    bench.check(tally["eye"] >= 60 and tally["look"] >= 60 and tally["follower"] >= 40,
                f"identities held: eye {tally['eye']}, look point {tally['look']}, follower {tally['follower']}",
                "identities")
    bench.check(tally["ground_calls"] >= 1 and tally["ground_y"] >= 1,
                f"ground-settle arm reached {tally['ground_calls']} time(s), {tally['ground_y']} eye heights", "ground")
    bench.check(tally["rider_rand"] >= 1, f"rider-follow: the lead factor's Rand at 0x80087724 ran "
                                          f"{tally['rider_rand']} time(s)", "rider_rand")
    bench.check(tally["director_calls"] >= 1, f"director arm ran {tally['director_calls']} time(s)", "director")
    # the director run: the cut to the shot list, ShotSetup called with a record of the table
    tr = traces["director"]
    shots = [regs for _, regs in tr.probes.get(SHOT_SETUP, [])]
    ok_rec = all(0x800D83B4 <= regs["a1"] < 0x800D83B4 + 26 * 20 and regs["a0"] == VIEW0 for regs in shots)
    bench.check(bool(shots) and ok_rec, f"director: ShotSetup called {len(shots)} time(s) on the shot table", "director")
    # ---- the mode runs: the camera settles on CAMERA.CA record k
    for name, k in (("mode1", 1), ("mode2", 2), ("mode3", 3), ("rr-race", 0)):
        tr = traces[name]
        last = [c for c in calls_of(tr, VIEW_UPDATE) if c[2]][-1]
        B = Mem(snapshot_at(tr, last[2]))
        rec = recs[cfg("mode_record", k, (k + 1) % 4)]
        v = last[1]["a0"]
        bench.check(B.s(v + 0x2A8) == rec["lookZ"][0] and B.w(v + 0x21C) == k,
                    f"{name}: +0x21C {B.w(v + 0x21C)}, look target {B.s(v + 0x2A8):#x} = record {k} lookZ", "modes")
    tr = traces["lookback"]
    held = [c for c in calls_of(tr, VIEW_UPDATE) if c[2] and Mem(snapshot_at(tr, c[0])).w(c[1]["a0"] + 0x224) & 1]
    ok = bool(held)
    for c in held:
        B = Mem(snapshot_at(tr, c[2]))
        v = c[1]["a0"]
        ok &= B.s(v + 0x2A8) == recs[0]["lookZ"][cfg("alt_column", 1, 0)] and B.s(v + 0x284) == recs[0]["eyeZ"][1]
    bench.check(ok, f"lookback (pad bit 13 held): bit 0 set on {len(held)} call(s), each on the second column "
                    "(eye ahead, looking back)", "modes")
    # ---- the frame order: ViewDistance reads the eye before ViewUpdate rewrites it
    tr = traces["rr-race"]
    vd = [s for s, _ in tr.probes.get(VIEW_DISTANCE, [])]
    vu = [s for s, _, _, _ in calls_of(tr, VIEW_UPDATE)]
    eye_reads = [r for r in tr.reads if VIEW_DISTANCE <= r[1] < 0x8008DCA0 and VIEW0 + 0xB8 <= r[2] < VIEW0 + 0xC4]
    eye_writes = [w for w in tr.writes if w[1] == 0x80089E24 and w[2] == VIEW0 + 0xB8]
    before = 0
    for s in vu[1:]:
        prev_vd = [r for r in eye_reads if r[0] < s and r[0] > max([x for x in vu if x < s], default=0)]
        before += bool(prev_vd)
    order = cfg("order", "reads-first", "writes-first")
    if verbose:
        print(f"  frame order: ViewDistance ran {len(vd)} times and read the eye before {before} of the "
              f"{len(vu) - 1} following ViewUpdate calls; race-path calls censused: {race_frames}")
    bench.check(order == "reads-first" and len(vd) >= 10 and before >= len(vu) - 2 and eye_writes,
                f"ViewDistance ran {len(vd)} times and read the eye before {before} of {len(vu) - 1} ViewUpdates",
                "order")
    if verbose:
        print("  ViewUpdate calls:", {k: v for k, v in tally.items() if k.startswith("calls_")})
        print("  camera Rand calls:", {k: v for k, v in tally.items() if k.startswith("rand_")})
        print(f"  identities: eye {tally['eye']}, look {tally['look']}, follower {tally['follower']}, "
              f"shake {tally['shake']}")
    return traces


# ---------------------------------------------------------------------------------------------
# the view record in the captures
# ---------------------------------------------------------------------------------------------
def check_captures(bench, recs, verbose=True):
    imgs = sorted(glob.glob(os.path.join(STATE_DIR, "*", "ram.bin"))) + sorted(glob.glob(os.path.join(DUMP_DIR, "*.bin")))
    n = 0
    dist = []
    shakes = []
    for p in imgs:
        ram = open(p, "rb").read()
        v = VIEW0
        bike = rd(ram, v + 0x238, "<I")
        if bike < 0x80000000 or rd(ram, v + 0x304) != 0:
            continue
        n += 1
        ok = bike == rd(ram, PLAYER1, "<I") and rd(ram, v + 0x21C) == cfg("capture_mode", 0, 1) and \
            rd(ram, v + 0xAC, "<H") == 0x9F and rd(ram, v + 1132 + 0x238, "<I") == 0
        bench.check(ok, f"{os.path.basename(os.path.dirname(p)) or p}: view 0 follows player 1, mode 0, handle 0x9F", "captures")
        shake = rd(ram, v + 0x2A4)
        speed = rd(ram, bike + 0x1E0)
        if shake:                                   # the off-road shake (0x80089808), measured in the dumps
            s60 = fixdiv(speed, 0x3C0000) if speed > 0 else u32(-s32(fixdiv(-speed, 0x3C0000)))
            gain = recs[0]["shake"]
            rs = [r for r in range(cfg("shake_range", 256, 64))
                  if iabs(s32(fixmul(r, fixmul(s60, gain)))) == shake]
            label = os.path.basename(os.path.dirname(p)) if p.endswith("ram.bin") else os.path.basename(p)
            shakes.append((label, speed, shake, rs))
            bench.check(bool(rs) and rd(ram, bike + 0x184) & 1 and rd(ram, v + 0x280) == s32(recs[0]["eyeY"][0] + shake),
                        f"{label}: off-road at {speed / 65536:.2f}: shake {shake:#x} = "
                        f"|FixMul(r, FixMul(speed/60, gain))| for r in {rs}, +0x280 = eyeY + shake", "shake")
        eye = [rd(ram, v + 0xB8 + 4 * i) for i in range(3)]
        aim = [rd(ram, v + 0x23C + 4 * i) for i in range(3)]
        dist.append(sum(((e - a) / 65536) ** 2 for e, a in zip(eye, aim)) ** 0.5)
    bench.check(n == 18, f"{n} race images", "captures")
    bench.check(len(shakes) == 3, f"{len(shakes)} images caught the shake arm", "shake")
    if verbose:
        for name, speed, sh, rs in shakes:
            print(f"  shake in {name}: speed {speed / 65536:.2f}, +0x2A4 {sh:#x} ({sh / 65536:.5f} units), r {rs}")
    if verbose and dist:
        print(f"  eye to aim point: {min(dist):.2f} .. {max(dist):.2f} world units over {n} images")
    bench.check(dist and 2.5 <= min(dist) and max(dist) <= 4.0, f"eye to aim point {min(dist):.2f}..{max(dist):.2f}",
                "captures")


# ---------------------------------------------------------------------------------------------
# driver
# ---------------------------------------------------------------------------------------------
def mid(f):
    """the check id of a model: the one `check_live` reports"""
    return f"model_{f:x}"


MUTATIONS = {  # key -> the check id(s) that must catch it
    "tree": {"tree"}, "callers": {"callers"}, "rand_sites": {"rand_sites"}, "getrcnt_sites": {"getrcnt_sites"},
    "words": {"words"}, "seams": {"seams"}, "cam_records": {"cam_read", "rrtool"}, "cam_read": {"cam_read"},
    "cam_sites": {"cam_sites"}, "suffix": {"suffix"}, "lagK": {"rrtool"}, "cam_stride": {mid(CHASE_SPRING)},
    "spring_k_off": {mid(CHASE_SPRING)}, "lit_spring_settled": {mid(CHASE_SPRING)},
    "chase_floor": {mid(ANGLE_CHASE)}, "lit_lead_min": {mid(LEAD)}, "zones_or": {mid(ZONES)},
    "rad_mul": {mid(TO_POLAR)}, "alt_column": {"identities", "modes"}, "lit_lag_clamp": {"identities"},
    "race_path": {"race_path"}, "race_callees": {"race_callees"}, "mode_record": {"modes"}, "order": {"order"},
    "capture_mode": {"captures"}, "model_min": {mid(f) for f in MODELS},
    "lit_ground_lift": {"ground"}, "lit_rand_mod": {"rider_rand"}, "shake_range": {"shake"},
}


def run_all(cmd, verbose=True):
    bench = Bench()
    REACH.clear()
    if verbose:
        print("skeleton:")
    check_skeleton(bench, verbose)
    if verbose:
        print("CAMERA.CA:")
    parsed = check_camera_file(bench, verbose)
    recs = parse_camera(open(os.path.join(E.DISC, "DATA", "CAMERA.CA"), "rb").read())
    if len(recs) < 4:
        recs = recs + recs[:4 - len(recs)]
    if verbose:
        print("captures:")
    check_captures(bench, recs, verbose)
    if cmd in ("live", "verify"):
        if verbose:
            print("live:")
        check_live(bench, recs, verbose)
    return bench


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["info", "live", "verify", "clean"])
    ap.add_argument("--mutate", action="store_true")
    a = ap.parse_args(argv)
    if a.cmd == "clean":
        shutil.rmtree(WORK, ignore_errors=True)
        print("removed", WORK)
        return 0
    if not a.mutate:
        bench = run_all(a.cmd, verbose=True)
        print(f"camera: {bench.n} checks, {bench.fail} failures")
        return 1 if bench.fail else 0
    caught = 0
    for key, owners in MUTATIONS.items():
        MUTATE["key"] = key
        bench = run_all(a.cmd, verbose=False)
        hit = bool(bench.failed_ids & owners)
        caught += hit
        print(f"  mutation {key:20s} {'caught by ' + ','.join(sorted(bench.failed_ids & owners)) if hit else 'NOT CAUGHT'}"
              f"  ({bench.fail} failing checks)")
    MUTATE["key"] = None
    print(f"camera --mutate: {caught} of {len(MUTATIONS)} mutations caught by their own check, "
          f"{len(MUTATIONS) - caught} not caught")
    return 0 if caught == len(MUTATIONS) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
