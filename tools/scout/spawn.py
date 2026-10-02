"""Scout probe for THE POPULATION RUNTIME of Road Rash: Jailbreak (USA, SLUS_01053): how bikes, riders
and traffic appear and vanish around the player.


What it checks.

  * SKELETON (static, from the instruction words of our own images): both image hashes, the extent,
    frame size, callee set and caller set of every population function, the literal words the
    arithmetic rests on (window radii, gates, percentages, offsets), every Rand call site inside the
    population functions, and the ported / unported status of every callee - read out of the bench's
    own row table (tools\\rrverify\\verify_physics.cpp and rows_*.inc), not typed in here.
  * LIVE (models against the machine): the transcriptions are EXECUTABLE PYTHON MODELS,
    one statement per original load / store / call, in the original's order.  Each is run against EVERY
    invocation of the original inside traces of the oracle interpreter (`<build>\\rrverify.exe trace`
    with whole-region watches, the call log and pc probes).  For one invocation the model starts from
    the exact guest RAM at the original's entry and the entry registers; every callee it calls is
    SUPPLIED BY THE TRACE (the call is matched against the original's next call from the same body -
    target and argument registers - and the callee's stores and v0 are replayed).  Compared: the
    ordered stores of the body outside its own stack frame, the calls, and v0.  The harness is
    tools\\scout\\pairs.py's, imported, not copied.
  * RUNS: two unmodified snapshots, and snapshot COPIES under work\\spawn\\probe whose only difference
    from the original savestate is DATA - a bike's road coordinate moved next to the player, a live
    bike / car / downed rider moved 600 units away, the spawner's accumulators advanced.  The original
    code is never changed; the unmodified game then spawns, wakes, retires and despawns by itself.
  * FACTS read from those runs: which transitions happened, in which frame, with which Rand calls, in
    which order (the Rand census), and what the new entities hold.

`verify --mutate` perturbs each checked claim in turn (a radius, a gate, a percentage, a callee, a
field, a Rand site, a ported status, a live fact ...) and requires the check guarding THAT claim to
fail.  Model constants are read from the claims, so a mutated constant re-runs the model comparison.

Reads ONLY work\\disc_us and work\\oracle\\state; writes ONLY under work\\spawn (snapshot copies and
trace outputs).  Uses <build>\\rrverify.exe read-only.  Never copies game bytes into the repo.

    python tools\\scout\\spawn.py info             # skeleton only
    python tools\\scout\\spawn.py live             # runs + model comparison + facts, verbose
    python tools\\scout\\spawn.py rand             # the Rand census of every run, call by call
    python tools\\scout\\spawn.py verify           # spawn: <n> checks, <m> failures
    python tools\\scout\\spawn.py verify --mutate  # spawn: <n> mutations, <n> detected
"""

from __future__ import annotations

import argparse
import bisect
import copy
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
STATE_DIR = os.path.join(ROOT, "work", "oracle", "state")
WORK = os.path.join(ROOT, "work", "spawn")
# RRJB_PROBE_BUILD names the build directory that holds rrverify.exe (default below).
RRVERIFY = os.path.join(ROOT, os.environ.get("RRJB_PROBE_BUILD") or "build_m0", "rrverify.exe")
OVL_BASE = 0x8005B5E8

u32, s32, iabs = P.u32, P.s32, P.iabs
Mismatch = P.Mismatch

# ---------------------------------------------------------------------------------------------
# guest addresses (every one read out of the words; the claims below re-check the ones that matter)
# ---------------------------------------------------------------------------------------------

GS_PTR = 0x8005B2F8          # -> game_state (+0x04 race type, +0x10 race clock, +0x30 players, +0x3C bank)
PLAYERS = 0x8005B268         # -> player p's bike, 4 bytes a player
POOLS = 0x800CE4D0           # the pool table, 7 x 16 bytes (population.md 1)
POOL0_BASE_PTR = 0x8005B3A0  # -> pool 0 slot 0
RAND, SEED = 0x8001FC58, 0x8005B4A8
SPAWN_ACC = 0x8005B318       # SpawnerPass's accumulator (dt, 16.16)
TRAF_ACC = 0x8005B210        # the traffic scheduler's own accumulator
TRAF_ON = 0x8005ACC4         # traffic switch
TRAF_GATE = 0x800CE578       # s16, the spawner's own gate
TRAF_BLOCK = 0x800D8710      # the traffic parameter block (population.md 3.3)
LAST_LANE = 0x8005B358       # s8, the lane the last |lane| == 4 spawn drew
VIEWS = 0x800CD898           # per-player view record, 1132 bytes a player; +0xBA / +0xC2 = integer x / z
POOL3_CTRL = 0x800CF650      # live, next, high, (pad), then 16 x 512-byte slots
PLAYER = 0x801B65D4          # pool 0 slot 0 in all three race snapshots
BIKE_STRIDE, RIDER_STRIDE = 1096, 628
RIDER0 = 0x801BB2EC
CAR0 = 0x800CF660

WATCHES = [(0x801B3000, 0xB400, "ents"), (0x800CC000, 0xE000, "glob"), (0x801FE000, 0x2000, "stack"),
           (0x8005A000, 0x2000, "sglob")]


def watched(a, n):
    return any(lo <= a and a + n <= lo + ln for lo, ln, _ in WATCHES)


# ---------------------------------------------------------------------------------------------
# images
# ---------------------------------------------------------------------------------------------

word_at = P.word_at


def images():
    return P.images()


def in_slus(a):
    return not images()["G"].contains(a)


def jal_target(site):
    w = word_at(site)
    if (w >> 26) != 3:
        return None
    return ((site + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)


_ALLJAL = {}


def all_call_sites(target):
    """every `jal target` in RASHCDG and in SLUS"""
    if not _ALLJAL:
        for key in ("G", "SLUS"):
            for a, w in images()[key].words():
                if (w >> 26) == 3:
                    _ALLJAL.setdefault(((a + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2), []).append(a)
    return sorted(_ALLJAL.get(target, []))


def jals_in(lo, hi):
    return [a for a in range(lo, hi, 4) if (word_at(a) >> 26) == 3]


def callees(lo, hi):
    return {jal_target(a) for a in jals_in(lo, hi)}


def rand_sites(lo, hi):
    return [a for a in jals_in(lo, hi) if jal_target(a) == RAND]


def frame_of(f, end):
    """the prologue's `addiu sp,sp,-N` (it may sit in a delay slot), 0 for a leaf without one"""
    for a in range(f, min(f + 64, end), 4):
        w = word_at(a)
        if (w >> 16) == 0x27BD and (w & 0x8000):
            return 0x10000 - (w & 0xFFFF)
    return 0


def rd(ram, a, fmt="<i"):
    return struct.unpack_from(fmt, ram, a & 0x1FFFFF)[0]


# ---------------------------------------------------------------------------------------------
# the bench's row table (ported = has a row); read, never typed
# ---------------------------------------------------------------------------------------------

_ROWS = {}


def load_ported():
    """{entry: row name} for every acceptance row of the bench, read from the bench's own row table
    (`rrverify phys --list`: each row's entry field, whatever helper built the row - reading a fixed list of row
    helpers out of the sources would miss WRow / PoliceRow / BindRow / TdRow rows). Spliced regions and the diagnostic sound_emitter_is_invisible are
    not ports of a whole function; an empty table raises."""
    if not _ROWS:
        r = subprocess.run([RRVERIFY, "phys", "--list"], capture_output=True, text=True, cwd=ROOT)
        for m in re.finditer(r"^\s+(\w+)\s+0x([0-9A-Fa-f]{8})\s\s(.*)$", r.stdout, re.M):
            name, entry, sig = m.group(1), int(m.group(2), 16), m.group(3)
            if "spliced" in sig or name == "sound_emitter_is_invisible":
                continue
            _ROWS.setdefault(entry, name)
        if not _ROWS:
            raise RuntimeError(f"{RRVERIFY} phys --list printed no rows (exit {r.returncode})")
    return dict(_ROWS)


# ---------------------------------------------------------------------------------------------
# THE CLAIMS - everything this probe checks
# ---------------------------------------------------------------------------------------------

def claims():
    return {
        "sha1": {"SLUS": "67ed165a2c517d4e6106fb0dfa324d66dd9a76f1",
                 "G": "cfe43a7786759f2cb9c57751cf99e84d1074782c"},
        # function: (end, frame bytes).  Instruction count = (end - start) / 4.
        "extent": {
            0x80093E6C: (0x80093ED4, 32), 0x80093ED4: (0x80093F94, 32), 0x80093F94: (0x80093FE4, 24),
            0x80093FE4: (0x80094184, 24), 0x80094184: (0x8009432C, 40), 0x8009432C: (0x800950E8, 88),
            0x800950E8: (0x800951B8, 32), 0x800951B8: (0x800952AC, 32), 0x800952AC: (0x80095410, 40),
            0x80099D48: (0x8009A038, 64), 0x80039F68: (0x8003A37C, 56), 0x80039DFC: (0x80039F68, 48),
            0x80013110: (0x80013204, 0), 0x8008CD88: (0x8008CDF4, 24), 0x8009C308: (0x8009C41C, 72),
            0x8009B474: (0x8009B4C0, 24), 0x8009CFF4: (0x8009D664, 160), 0x8009AD48: (0x8009B474, 112),
            0x8009CF1C: (0x8009CFF4, 40), 0x8009E768: (0x8009E89C, 40), 0x8009F44C: (0x8009F578, 8),
            0x8008DBA8: (0x8008DBCC, 0), 0x8009FE90: (0x8009FF24, 24), 0x8009A298: (0x8009AB60, 72),
            0x8009AB60: (0x8009ACA4, 48), 0x8003A98C: (0x8003A9D8, 24), 0x80012C1C: (0x80012FC8, 56),
            0x8009DAF8: (0x8009DB58, 0), 0x8009F578: (0x8009F6F8, 0),
            0x80039CFC: (0x80039DFC, 24), 0x80037524: (0x8003775C, 0), 0x8003A700: (0x8003A98C, 48),
            0x8003A468: (0x8003A5F4, 40),
        },
        # the jal targets inside each function (a set, from the words)
        "callees": {
            0x80093E6C: {0x80093ED4},
            0x80093ED4: {0x80039F68, 0x80093F94},
            0x80093F94: {0x80093FE4, 0x8009432C},
            0x80093FE4: {0x8002090C, 0x800903F4, 0x8009DAF8, 0x8002847C, 0x8002820C},
            0x8009432C: {0x8001FC90, 0x8003A468, 0x8003B4B0, 0x8003DE28, 0x8002EAD8, 0x80037450, 0x8003DF54,
                         0x8003FB34, 0x8003FA40, 0x8002090C, 0x80068D20, 0x8008BA18, 0x80020018, 0x8001E0B4,
                         0x8007EC30, 0x80012838, 0x80012884, 0x80012858, 0x800C4550, 0x8009DAF8, 0x8003AF9C,
                         0x800BCD10, 0x800BCA68, 0x80028034},
            0x800950E8: {0x800951B8},
            0x800951B8: {0x80099D48, 0x80039F68, 0x800952AC},
            0x800952AC: {0x8003A468, 0x8003DE28, 0x8003DF54, 0x8002EAD8, 0x800C4550, 0x800C3104, 0x800BCD10,
                         0x800BCA68, 0x800903F4},
            0x80099D48: {0x800245DC, 0x800245F4},
            0x80039F68: {0x80039DFC, 0x8003A700, 0x8002EAD8, 0x80013110},
            0x80013110: set(),
            0x8008CD88: {0x8009C308, 0x8009B474},
            0x8009B474: {0x8009E89C, 0x8009CFF4},
            0x8009CFF4: {0x8009F44C, 0x8008CDF4, 0x8008DBA8, 0x8009FF24, 0x8001FC58, 0x80012C1C, 0x8003B4B0,
                         0x8003A98C, 0x8003F580, 0x8009F578, 0x8009AD48, 0x8009FE90},
            0x8009AD48: {0x8001FC58, 0x80039DFC, 0x8003A700, 0x8001E0B4, 0x8002EAD8, 0x8003662C, 0x8003DE28,
                         0x8009E768, 0x8003DF54, 0x8003AF9C, 0x8003B61C, 0x8002FAD4, 0x8008C000, 0x80012FC8,
                         0x8008BA18, 0x80020018, 0x80039F68, 0x80028034},
            0x8009CF1C: {0x8001FC58},
            0x8009E768: {0x8003E1E8, 0x8009CF1C},
            0x8009F44C: set(),
            0x80039DFC: {0x80039A08, 0x80039CFC},
            0x80039CFC: {0x80039C90},
            0x80037524: set(),
            0x8003A700: {0x80039C90, 0x80037524, 0x8003F3B4, 0x8003BE1C, 0x80039B60},
            0x8003A468: {0x80039DFC, 0x80037524},
        },
        # every caller of each function (jal sites, RASHCDG and SLUS)
        "callers": {
            0x80093E6C: [0x8003CFA4, 0x8003DAA0, 0x80078C58],
            0x80093ED4: [0x8008C52C, 0x80093EA4],
            0x80093F94: [0x80011A88, 0x80093F78, 0x8009C778, 0x8009EEEC],
            0x8009432C: [0x80093FCC, 0x800C96AC],
            0x80093FE4: [0x80093FBC],
            0x800950E8: [0x8003CFAC, 0x8003DAA8, 0x80078C60],
            0x800951B8: [0x8008C5D8, 0x8008C644, 0x8009513C, 0x80095188],
            0x80099D48: [0x800951E0],
            0x8008CD88: [0x8008AC40],
            0x8009C308: [0x80011B30, 0x8008CDB0],
            0x8009AD48: [0x8009D5E8, 0x8009D990, 0x800C9BE4],
            0x80039F68: [0x8003DAE0, 0x8003DB40, 0x8003DBA0, 0x8003DC00, 0x8003DC60, 0x80093F50, 0x8009527C,
                         0x8009A318, 0x8009ABB0, 0x8009ACF8, 0x8009B3E4, 0x8009C2B4, 0x800A0924, 0x800A2A10,
                         0x800CB4BC, 0x800CBE80],
            0x8009CF1C: [0x8009E83C],
            0x8009E768: [0x8009A844, 0x8009A850, 0x8009A85C, 0x8009B160, 0x8009B16C, 0x8009B178],
            0x80039CFC: [0x80039E90, 0x80039F04],
            0x80039DFC: [0x80039F94, 0x8003A494, 0x8009ADD0, 0x8009BB90, 0x8009C580, 0x800A0824, 0x800C989C,
                         0x800C9D7C, 0x800CB94C],
            0x80037524: [0x8003A5C8, 0x8003A7D8],
            0x8003A700: [0x80039FC8, 0x8009AEB4, 0x8009BC94, 0x800A083C, 0x800C98B4, 0x800C9D90, 0x800CBAD4],
            0x8003A468: [0x80094638, 0x800952E0],
        },
        # every Rand call site inside the population functions, in address order
        "rand_sites": {
            0x8009CFF4: [0x8009D2D4, 0x8009D494], 0x8009AD48: [0x8009ADB0, 0x8009B0D4],
            0x8009CF1C: [0x8009CF94], 0x80012C1C: [0x80012D9C, 0x80012EB0], 0x8003A98C: [0x8003A9AC],
            0x8009E89C: [0x8009EB10], 0x80094184: [0x800942D4],
            0x8009432C: [], 0x80093FE4: [], 0x80039F68: [], 0x800951B8: [], 0x80099D48: [], 0x8009A298: [],
        },
        # literal words: (address, word, meaning)
        "words": {
            0x800131E0: (0x28620000 | 230, "sticky radius for kinds >= 2 (230)"),
            0x800131E8: (0x28620000 | 200, "entry radius for kinds >= 2 (200)"),
            0x800131F0: (0x28620000 | 350, "sticky radius for bikes and riders (350)"),
            0x800131F8: (0x28620000 | 300, "entry radius for bikes and riders (300)"),
            0x800131AC: (0x24020006, "kind 6 (collision volumes) takes the gp+204 radius"),
            0x8003A188: (0x3C070014, "the sticky call passes extra = 0x140000 (20.0)"),
            0x8008CDA4: (0x28424000, "SpawnerPass: acc >= 16384 runs the cell walker"),
            0x8008CDBC: (0x3402FFFF, "SpawnerPass: acc > 0xFFFF runs the two schedulers"),
            0x8008CD94: (0x8E02B318, "SpawnerPass's accumulator is 0x8005B318"),
            0x8009D2E8: (0x3C040014, "the fast-player threshold 0x00141DDD (hi)"),
            0x8009D2F0: (0x34841DDD, "the fast-player threshold 0x00141DDD (lo)"),
            0x8009D33C: (0x3C020005, "the slow-player threshold 5.0"),
            0x8009D344: (0x24030000 | 90, "race type 44 uses 90 per cent"),
            0x8009D4CC: (0x28A20000 | 76, "race type 44: an ahead spawn is flipped when Rand % 100 < 76"),
            0x8009D5C8: (0x3C020078, "a candidate within 120.0 of the player on his road is dropped"),
            0x8009B090: (0x3C020077, "the spawner's own 120.0 guard (0x77FFFF, hi)"),
            0x8009B0DC: (0x30430003, "|lane| == 4 draws Rand() & 3"),
            0x8009B0F4: (0x2A020005, "... at most 5 draws"),
            0x8009B198: (0x3442CCCC, "the default lateral 0x1CCCC (1.8)"),
            0x8009B35C: (0x3C070002, "a new car's speed 0x26666 (hi)"),
            0x8009ADF4: (0x28620010, "pool 3 holds 16 cars"),
            0x8009AE60: (0x24E20060, "a car's handle is slot + 96"),
            0x80094128: (0x8C42B244, "a retired cop's +0x144 comes from 0x8005B244"),
            0x80095264: (0x00711821, "a downed rider's +0x168 = its bike's + 1"),
            0x80099DB0: (0x3C11FFFB, "a dormant rider chases its bike by -5.0 ..."),
            0x80099DB8: (0x3C110005, "... or +5.0 a frame"),
            0x8009F494: (0x24030024, "the per-road density table is clamped at road 36"),
            0x8009F48C: (0x24A524F0, "the per-road density table is SLUS 0x800524F0 (lo)"),
            0x8009F548: (0x2403000F, "the cap is clamped to 15"),
            0x8009D1C0: (0x00031100, "clock gate: table * 300 (x5, x15, x4) ..."),
            0x80039ED4: (0x24514B10, "the resident piece list is 0x800D4B10 (lo)"),
            0x80039EBC: (0x8C42B31C, "... its last index is *(0x8005B31C) (lo)"),
            0x800375EC: (0x3C0451EB, "SeatAlong divides the along by 50 (0x51EB851F, hi) ..."),
            0x800375F0: (0x3484851F, "... (lo)"),
            0x80037574: (0x8D42000C, "... from the cursor's own slice's running distance +0x28 (lw v0,12(t2))"),
            0x8009D234: (0x000210C0, "... and table * 600 for the early phase"),
        },
        # ported / unported against the bench's row table
        "ported": {0x8001FC58, 0x8001FC90, 0x8002EAD8, 0x80039C90, 0x80039A08, 0x8003B4B0, 0x8003F580,
                   0x8003F408, 0x80020018, 0x8001E0B4, 0x8008BA18, 0x800BCA68, 0x800BCD10, 0x8003B8F4,
                   0x8003662C, 0x8003AF9C, 0x8003B61C, 0x8003DE28, 0x8003DF54, 0x80037450, 0x8003A9D8,
                   # the instant remount (rows_crash.inc)
                   0x800903F4,
                   # the population port (rows_population.inc)
                   0x80013110, 0x80039CFC, 0x80039DFC, 0x80037524, 0x8003A700, 0x8003A468, 0x80039F68,
                   0x8009DAF8, 0x80093FE4, 0x8009432C, 0x80093F94, 0x80093ED4, 0x80093E6C, 0x80099D48,
                   0x800952AC, 0x800951B8, 0x800950E8, 0x8003A3CC, 0x80097518, 0x80095724, 0x8009F44C,
                   0x8008DBA8, 0x8003A98C, 0x8009CF1C, 0x8009E768, 0x8009FE90, 0x8009AD48, 0x8009CFF4,
                   0x8009B474, 0x8008CD88, 0x80012838, 0x80012884, 0x8002820C,
                   # the traffic / police / world rows: RoadWalk, Spacing, ShareFlags, Budget, NodeLanes, CarSetup
                   # (rows_traffic_leaves.inc), the cell walker (rows_world.inc), PoliceSched (rows_police.inc),
                   # PoolRelease, ModelBind (rows_traffic_bind.inc), TrafficPass (rows_traffic_drive.inc). Every
                   # callee of the population functions has a row.
                   0x80012C1C, 0x8009F578, 0x8009FF24, 0x8008CDF4, 0x8003E1E8, 0x80012FC8, 0x8009C308, 0x8009E89C,
                   0x8008C000, 0x8002FAD4, 0x8009A298},
        "unported": set(),
        # model constants (the models read these, so a mutation re-runs the comparison)
        "k": {
            "radius": {"bike_in": 300, "bike_stay": 350, "other_in": 200, "other_stay": 230, "extra": 0x140000},
            "acc": {"walker": 16384, "sched": 0xFFFF},
            "fast": 0x141DDD, "slow": 0x50000, "pct44": 90, "flip44": 76, "dup": 0x780000,
            "rider_step": 0x50000, "far_cop": 201, "density_max": 15, "density_road_max": 36,
            "sched_ahead": 0x800D872C, "sched_behind": 0x800D8730, "piece_list": 0x800D4B10,
        },
        # live facts; filled per run below
        "live": {
            "rr-race": {"cars_spawned": 1, "placements": 0, "retires": 0, "spawn_ahead": 180.0, "rand_per_car_frame": 3,
                        "dormant_writes": (39, 39),
                        "rand_round": ["0x8009d2d4", "0x8009b0d4", "0x8002fc94", "0x80030224",
                                       "0x8009cf94", "0x8009cf94", "0x8009cf94"]},
            "wake": {"woken": [15], "refile": [(1, 0x80094FA0, 0x08000000), (1, 0x80071D04, 0x8005B298),
                                               (1, 0x80071D0C, 0)]},
            "wake-behind": {"woken": [15]},
            "retire": {"retired": [5], "retires": 1},
            "sched": {"cop_released": 1, "cop_ahead": -60.0, "cop_speed": 18.0, "cars_spawned": 0, "cop_stack": [(4, 224)],
                      "rand_frame1": ["0x800942d4", "0x800942d4", "0x8009d2d4", "0x8009cf94", "0x8009cf94", "0x8009cf94"]},
            "sched-empty": {"spawn_ahead": 180.0,
                            "rand_frame1": ["0x8009d2d4", "0x8009b0d4", "0x8009b0d4", "0x8002fc94", "0x80030224"]},
            "car-far": {"car_despawned": 1},
            "rider-far": {"rider_dormant": [6], "chase_calls_min": 8, "chase_steps": [5.0]},
            "quick": {"rand_calls": 0},
            "rider-back": {"rider6": [(1, 0), (2, 1)]},
            # the entity's +0x140 written by its window test in frame 1, at octagonal distance 299/300 (entry of a
            # dormant bike), 349/350 (a live bike staying), 229/230 (a live car staying)
            "edges": {"edge-bike-299": 1, "edge-bike-300": 0, "edge-live-349": 1, "edge-live-350": 0,
                      "edge-car-229": 1, "edge-car-230": 0},
        },
        # the models that must have run at least this many compared calls, with 0 mismatches
        "model_min_calls": {
            0x80013110: 50, 0x80039F68: 50, 0x80093ED4: 50, 0x80093F94: 50, 0x80093E6C: 10, 0x800950E8: 10,
            0x800951B8: 10, 0x8008CD88: 10, 0x8009B474: 2, 0x8009CF1C: 20, 0x8009E768: 20, 0x8009F44C: 2,
            0x80093FE4: 1, 0x800952AC: 1, 0x80099D48: 2, 0x8009CFF4: 2, 0x8009AD48: 2,
            0x80039DFC: 50, 0x80039CFC: 50, 0x8003A700: 50, 0x80037524: 50, 0x8003A468: 20,
        },
    }


# ---------------------------------------------------------------------------------------------
# the models (every address is RASHCDG unless SLUS)
# ---------------------------------------------------------------------------------------------

K = {"c": claims()}


def kk(*path):
    v = K["c"]["k"]
    for p in path:
        v = v[p]
    return v


def gs(m):
    return m.lw(GS_PTR)


def nplayers(m):
    return m.lw(gs(m) + 48)


def m_window_pred(m, kind, pos, p, extra):
    """SLUS 0x80013110 (leaf): is `pos` within the window of player p's view? (5th argument: sticky)"""
    view = u32(VIEWS + 1132 * p)
    dx = iabs(m.lhs(view + 186) - m.lhs(pos + 2))
    dz = iabs(m.lhs(view + 194) - m.lhs(pos + 10))
    big, small = (dz, dx) if dx < dz else (dx, dz)
    s15 = small + (small >> 1)
    d = big - (big >> 5) - (big >> 7) + (s15 >> 2) + (s15 >> 6)
    sticky = m.lw(m.entry_sp + 16)
    r = kk("radius")
    if kind == 6:
        return int(d < (s32(m.lw(0x8005AD58) + extra) >> 16))
    if u32(kind) < 2:
        return int(d < (r["bike_stay"] if sticky else r["bike_in"]))
    return int(d < (r["other_stay"] if sticky else r["other_in"]))


def m_road_window(m, h):
    """SLUS 0x80039F68(h = &e[+0xAC]): the road-window test, re-derived for every entity every frame."""
    piece = m.call(0x80039DFC, h, h + 188)
    if piece == 0:
        return 0
    s4 = m.lhs(h + 148)
    if (m.lhu(h) >> 5) < 2 and s4 == 0:                       # a dormant bike or rider: re-seat it
        if m.call(0x8003A700, piece, h + 188, h + 156) == 0:
            return 0
        sl = m.lw(h + 168)
        m.call(0x8002EAD8, sl + 20, sl + 14, m.lw(h + 176), h + 12)
        m.call(0x8002EAD8, h + 12, sl + 2, m.lw(h + 172), h + 12)
    hd = m.lhu(h)
    if (hd >> 5) < 2 and (hd & 0x1F) < s32(nplayers(m)):      # a player's own bike / rider
        if (hd >> 5) != 0:
            return 1
        b = u32(m.lw(POOL0_BASE_PTR) + hd * BIKE_STRIDE)
        if b:
            if m.lw(b + 1088) == 0:
                b = m.lw(b + 856)
            if b and m.lw(m.lw(b + 852) + 604) < 3:
                s4 = 1
        if s4:
            return s4
    s3 = 0
    if s4 != 0:
        for p in range(max(0, s32(nplayers(m)))):
            s3 |= m.call(0x80013110, m.lhu(h) >> 5, h + 12, p, kk("radius", "extra"))
        s4 = s3
    if m.lhs(h + 148) == 0:
        for p in range(max(0, s32(nplayers(m)))):
            s3 |= m.call(0x80013110, m.lhu(h) >> 5, h + 12, p, 0)
        s4 = s3
    if s4 == 0 or (m.lhu(h) >> 5) != 0:
        return s4
    b = u32(m.lw(POOL0_BASE_PTR) + m.lhu(h) * BIKE_STRIDE)    # a live pool-0 bike: the cop range bit
    npl = nplayers(m)
    if m.lhu(b + 172) < npl or (m.lbu(m.lw(b + 1084) + 1) & 0xF) != 2:
        return s4
    far = []
    for p in range(max(0, s32(npl))):
        d = s32(m.lw(b + 324) - m.lw(m.lw(PLAYERS + 4 * p) + 324)) >> 12
        far.append(-d if d < 0 else d)
    lim = kk("far_cop")
    if s32(npl) == 2:
        if far[0] < lim or far[1] < lim:
            return s4
    elif far[0] < lim:
        return s4
    m.sb(b + 928, m.lbu(b + 928) | 0x20)
    return s4


def m_piece_contains(m, btt, key):
    """SLUS 0x80039CFC(btt, key): does the object of BTT_ record `btt` hold road coordinate `key`
    ({u32 road | kind << 16, s32 dir, s32 along}; only the along's integer half +10 is read)?"""
    if key == 0 or btt == 0:
        return 0
    obj = m.lw(btt + 12)
    if obj == 0:
        return 0
    a0 = m.lhs(btt + 4)
    v1 = u32((a0 << 16) | m.lhu(btt + 16))
    s0 = m.lhs(key + 10)
    if a0 == 0:                                               # a road piece: its id and along range
        if v1 != m.lw(key):
            return 0
        if s0 < m.lhs(btt + 22) or m.lhs(btt + 26) < s0:
            return 0
        return 1
    k = m.lw(key)
    if v1 == k:                                               # the junction's own node key
        return int(m.lw(btt + 28) == 0)
    rec = m.call(0x80039C90, obj, k & 0xFFFF)                 # an arm of the junction on the key's road
    if rec == 0 or m.lhs(rec + 2) != 0:
        return 0
    if s0 < m.lhs(rec + 26):
        return 0
    return int(not (m.lhs(rec + 30) < s0))


def m_road_gate(m, h, key):
    """SLUS 0x80039DFC(h, key): the resident object holding `key`, or 0 (h = &e[+0xAC] or 0)"""
    s3, s4 = 0xFFFFFFFF, 0
    if h != 0 and m.lhs(h + 148) != 0:                        # live: the cursor's own object first
        s0 = m.call(0x80039A08, m.lw(m.lw(h + 156)))
        if s0 == 0:
            return 0
        hd = m.lhu(h)
        if (hd >> 5) < 2 and (hd & 0x1F) < s32(nplayers(m)):
            return m.lw(s0 + 12)
        if m.call(0x80039CFC, s0, key):
            return m.lw(s0 + 12)
        s4 = m.lw(s0 + 12)
        s3 = m.lw(m.lw(h + 156))
    if m.lws(0x8005B31C) < 0:
        return 0
    s2, s1 = 0, kk("piece_list")
    while True:
        a0 = m.lw(s1)
        if a0 != 0xFFFFFFFF and s3 != a0:
            s0 = m.call(0x80039A08, a0)
            if s0 != 0 and m.call(0x80039CFC, s0, key):
                return m.lw(s0 + 12) if s3 == 0xFFFFFFFF else s4
        s2 += 1
        if m.lws(0x8005B31C) < s2:
            return 0
        s1 += 16


def m_seat_along(m, cur, a1):
    """SLUS 0x80037524(cursor, along) (leaf): the slice of the cursor's sub-object at `along` from the
    piece start, found through the object's DIST table, and the along-in-slice; returns the slice"""
    if cur == 0:
        return 0
    sub, obj, piece = m.lw(cur + 8), m.lw(cur), m.lw(cur + 4)
    v1 = m.lhs(sub + 8) + m.lhs(sub + 10)
    t4 = m.lw(obj + 68)
    t3 = u32(m.lw(obj + 52) + 52 * v1 - 52)                   # the sub-object's last slice
    cum = m.lw(m.lw(cur + 12) + 40)
    n1c = m.lhs(obj + 28)
    a1 = u32(cum + a1) if s32(a1) > 0 else u32(cum - a1)
    if m.lhs(piece + 2) != 0 or n1c <= 0 or t4 == 0:
        return 0
    hi = s32(a1) >> 16
    t0 = m.lws(piece + 24)
    if hi < (t0 >> 16) or m.lhs(piece + 30) < hi:
        if (s32(a1) >> 16) < m.lhs(piece + 30):                # before the piece: its first slice
            m.sw(cur + 20, 0)
            a0 = u32(m.lw(obj + 52) + 52 * m.lhs(sub + 8))
            m.sw(cur + 12, a0)
            return a0
        m.sw(cur + 12, t3)                                    # past it: the last slice, at its end
        m.sw(cur + 20, m.lw(t3 + 32))
        return 0
    d = s32(u32(a1 - t0))
    q = ((d >> 16) * 0x51EB851F) >> 32
    v1 = (q >> 4) - (-1 if d < 0 else 0) - 1                  # (d >> 16) / 50 - 1, truncated
    a0 = m.lhs(sub + 22) - 1
    if a0 < v1:
        v1 = a0
    if v1 < 0:
        a0 = u32(m.lw(obj + 52) + 52 * m.lhs(sub + 8))
    else:
        idx = m.lhs(sub + 8) + m.lhs(u32(t4 + 4 * (m.lhs(sub + 20) + v1) + 2))
        a0 = u32(m.lw(obj + 52) + 52 * idx)
    cnt = m.lhs(sub + 10)
    if m.lhs(a0) < cnt:
        while s32(m.lw(a0 + 32)) < s32(u32(a1 - m.lw(a0 + 40))):
            a0 = u32(a0 + 52)
            if not (m.lhs(a0) < cnt):
                break
    if t3 < a0:
        m.sw(cur + 12, t3)
        m.sw(cur + 20, m.lw(t3 + 32))
        return a0
    m.sw(cur + 12, a0)
    m.sw(cur + 20, u32(a1 - m.lw(a0 + 40)))
    return a0


def m_cursor_seat(m, obj, key, cur):
    """SLUS 0x8003A700(object, key, cursor): the 32-byte cursor on `object` for road coordinate `key`"""
    if obj == 0:
        return 0
    k = m.lw(key)
    kind = k >> 16
    if kind == 0:                                             # on a road: the piece, then the along
        s0 = m.lw(obj + 44) if m.lhs(obj + 16) == 0 else m.call(0x80039C90, obj, k & 0xFFFF)
        if s0 == 0:
            return 0
        a0 = m.lhs(s0 + 20)
        v1 = m.lw(obj + 48)
        m.sw(cur, obj)
        m.sw(cur + 4, s0)
        sub = u32(v1 + 28 * a0)
        m.sw(cur + 8, sub)
        first = m.lhs(sub + 8)
        sl = m.lw(obj + 52)
        for o in (16, 20, 24, 28):
            m.sw(cur + o, 0)
        m.sw(cur + 12, u32(sl + 52 * first))
        m.call(0x80037524, cur, u32(m.lw(key + 8) - m.lw(s0 + 24)))
        return 1
    if m.lhs(obj + 16) != kind or m.lw(obj + 8) != (k & 0xFFFF) or m.lw(obj + 12) != 0:
        return 0                                              # a node key: only its own core object
    s0, s3 = m.lw(obj + 44), m.lw(obj + 48)
    leg = m.call(0x8003F3B4, m.lw(obj + 8))
    if leg == 0:
        return 0
    if m.lws(leg + 16) > 0:
        m.sw(cur, obj)
        m.sw(cur + 4, s0)
        s3 = m.call(0x8003BE1C, cur, m.lw(leg + 8), m.lw(leg + 84), m.sp + 16)
    if s3 == 0:
        return 0
    m.sw(cur, obj)
    m.sw(cur + 4, s0)
    m.sw(cur + 8, s3)
    m.sw(cur + 12, u32(m.lw(obj + 52) + 52 * m.lhs(s3 + 8)))
    a2 = m.call(0x80039B60, m.lw(obj))
    if a2:
        s4, a1, cnt = 0, 0, m.lhs(a2 + 8)
        if 0 < cnt:
            t2 = m.lhs(a2 + 6)
            G = m.lw(0x8005B240)
            t1, t0, a3 = m.lw(obj + 48), m.lw(G + 52), m.lw(G + 56)
            while True:
                s4 = u32(t0 + 12 * (t2 + a1))
                g = u32(a3 + 12 * m.lhs(s4 + 2))
                if m.lhs(g + 6) != 0 and u32(t1 + 28 * m.lhs(g + 4)) == s3:
                    break
                a1 += 1
                if not (a1 < cnt):
                    break
        m.sw(cur + 24, a2)
        m.sw(cur + 28, s4)
    else:
        m.sw(cur + 24, 0)
        m.sw(cur + 28, 0)
    m.sw(cur + 20, 0)
    m.sw(cur + 16, 0)
    return 1


def m_cursor_reseat(m, key, cur):
    """SLUS 0x8003A468(key, cursor): re-seat a cursor from a road coordinate alone (no entity)"""
    obj = m.call(0x80039DFC, 0, key)
    if obj == 0:
        return 0
    if m.lhs(obj + 16) == 0:
        s0 = m.lw(obj + 44)
    else:
        s0, n, v1 = 0, m.lhs(obj + 18), 0
        if 0 < n:
            k, a3 = m.lw(key), m.lw(obj + 44)
            while True:
                s0 = u32(a3 + 32 * v1)
                if m.lhs(s0 + 2) == (k >> 16) and m.lw(s0 + 12) == (k & 0xFFFF):
                    break
                v1 += 1
                if not (v1 < n):
                    break
        if m.lhs(s0 + 2) != 0:
            return 0
    sub = u32(m.lw(obj + 48) + 28 * m.lhs(s0 + 20))
    sl = u32(m.lw(obj + 52) + 52 * m.lhs(sub + 8))
    m.sw(cur, obj)
    m.sw(cur + 4, s0)
    m.sw(cur + 8, sub)
    m.sw(cur + 12, sl)
    for o in (20, 16, 24, 28):
        m.sw(cur + o, 0)
    a1 = s32(u32(m.lw(key + 8) - m.lw(s0 + 24)))
    if a1 < 0:
        a1 = 0
    if m.lws(s0 + 28) < a1:
        a1 = m.lws(s0 + 28)
    m.call(0x80037524, cur, u32(a1))
    return 1


def m_activation_pass(m):
    """0x80093E6C: every pool-0 slot, 0x80093ED4(e, 0)"""
    n = s32(m.lw(m.lw(POOLS + 12)))
    e = m.lw(POOLS)
    while n >= 0:
        m.call(0x80093ED4, e, 0)
        e = u32(e + m.lw(POOLS + 4))
        n -= 1


def m_activate(m, e, force):
    """0x80093ED4(e, force)"""
    if m.lhs(e + 320) == 0 and not (m.lhu(e + 172) < nplayers(m)) \
            and (m.lbu(m.lw(e + 1084) + 1) & 0xF) == 2 and not (m.lbu(e + 928) & 0x10):
        return None                                             # a parked cop
    old = m.lhs(e + 320)
    v = 0 if force else m.call(0x80039F68, e + 172)
    m.sh(e + 320, u32(-v) & ((m.lhu(e + 320) & 2) | 1))
    m.call(0x80093F94, e, old)
    return None


def m_transition(m, e, old):
    """0x80093F94(e, old)"""
    if e == 0:
        return None
    a2 = m.lhu(e + 320)
    if (a2 & 1) == (old & 1):
        return None
    if a2 == 0:
        m.call(0x80093FE4, e)
    else:
        m.call(0x8009432C, e)
    return None


def m_retire(m, e):
    """0x80093FE4(e): a bike leaves the window"""
    if e == 0 or m.lhs(e + 320) != 0:
        return None
    g = gs(m)
    if m.lhu(e + 172) < m.lw(g + 48) or not (u32(m.lbu(g + 57) - 1) < 2):
        m.call(0x8002090C, e)
        R = m.lw(e + 852)
        if m.lw(R + 604) < 3 or m.lhs(R + 320) == 0:
            m.sh(R + 320, 0)
            m.call(0x800903F4, e, 1)
        else:
            m.sw(e + 568, m.lw(e + 568) | 0x100)
    m.sw(e + 568, m.lw(e + 568) | 0x08000000)
    a0 = m.lw(e + 540)
    if a0:
        m.sw(a0 + 36, 0)
        m.sw(0x800CE178, u32(m.lw(0x800CE178) - 1))
        m.sw(e + 540, 0)
    if m.lhu(e + 172) < m.lw(gs(m) + 48):
        return None
    if (m.lbu(m.lw(e + 1084) + 1) & 0xF) != 2:
        return None
    v1 = m.lbu(e + 928)
    if v1 & 0x10:
        m.sb(e + 928, v1 & 0xEF)
        m.sw(e + 324, u32(m.lw(0x8005B244) << 12))
        m.call(0x8009DAF8, u32(-1))
        if (m.lw(e + 36) >> 27) & 1:
            m.call(0x8002847C, e)
            m.call(0x8002820C, e)
    m.sb(e + 928, m.lbu(e + 928) & 0xDF)
    return None


def m_downed_pass(m):
    """0x800950E8: every pool-0 bike's rider (and a passenger's) in mount state 3 or 4"""
    n = s32(m.lw(m.lw(POOLS + 12)))
    e = m.lw(POOLS)
    while n >= 0:
        r = m.lw(e + 852)
        if u32(m.lw(r + 604) - 3) < 2:
            m.call(0x800951B8, r, 0)
        if m.lbu(m.lw(e + 852) + 572) & 0x10:
            r = m.lw(m.lw(e + 856) + 852)
            if u32(m.lw(r + 604) - 3) < 2:
                m.call(0x800951B8, r, 0)
        e = u32(e + m.lw(POOLS + 4))
        n -= 1


def m_downed(m, r, off):
    """0x800951B8(r, off): a downed rider's own window test"""
    old = m.lhs(r + 320)
    s1 = off
    if old == 0:
        m.call(0x80099D48, r)
    elif m.lw(r + 552) & 0x04000000:
        hd = m.lhu(r + 172)
        if (hd >> 5) != 1 or not ((hd & 0x1F) < s32(nplayers(m))):
            if not (m.lbu(r + 572) & 0x20):
                s1 = 1
                m.sw(r + 552, m.lw(r + 552) & 0xFBFFFFFF)
                m.sw(r + 360, u32(m.lw(m.lw(r + 596) + 360) + 1))
    if s1:
        m.sh(r + 320, 0)
    else:
        m.sh(r + 320, m.call(0x80039F68, r + 172))
    m.call(0x800952AC, r, old)
    return None


def m_rider_transition(m, r, old):
    """0x800952AC(r, old): a downed rider enters or leaves the window"""
    if r == 0:
        return None
    a0 = m.lhu(r + 320)
    if (a0 & 1) == (old & 1):
        return None
    if a0 != 0:
        if m.call(0x8003A468, r + 360, r + 328) == 0:
            m.sh(r + 320, 0)
            return None
        m.sw(r + 600, 0xFFFF0000)
        m.call(0x8003DE28, r, 1, 0, u32(-1))
        m.call(0x8003DF54, r, 1, u32(-1))
        sl = m.lw(r + 340)
        m.call(0x8002EAD8, sl + 20, sl + 14, m.lw(r + 348), r + 184)
        sl = m.lw(r + 340)
        m.call(0x8002EAD8, r + 184, sl + 2, m.lw(r + 344), r + 184)
        m.sw(r + 508, 0)
        m.sw(r + 552, 0)
        m.sh(r + 544, 224)
        m.sw(r + 604, 0)
        m.call(0x800C4550, 73, r, 17)
        return None
    B = m.lw(r + 596)
    if m.lhs(B + 320) != 0:
        m.call(0x800C3104, r, 0)
        m.call(0x800BCD10, m.lw(r + 596))
        m.call(0x800BCA68, None, 0, m.lw(r + 596))
        m.call(0x800BCA68, None, 0, m.lw(r + 596))
    else:
        m.call(0x800903F4, B, 1)
    return None


def m_rider_chase(m, r):
    """0x80099D48(r): a dormant downed rider's road coordinate walks toward its bike's"""
    v0 = m.lw(r + 360)
    B = m.lw(r + 596)
    s5 = v0 & 0xFFFF
    s3 = m.lhu(B + 360)
    s4 = 0
    step = kk("rider_step")
    s1 = 0
    if s3 == s5:
        if (v0 >> 16) == 0:
            s1 = step if m.lws(r + 368) < m.lws(B + 368) else u32(-step)
    else:
        s0 = 0
        if (v0 >> 16) == 0:
            s4 = m.call(0x800245DC, s5)
        else:
            s0 = m.call(0x800245F4, s5)
        if m.lhu(B + 362) == 0:
            a1 = m.call(0x800245DC, s3)
            s2 = 0
        else:
            s2 = m.call(0x800245F4, s3)
            a1 = 0
        a2 = 1
        if a1 != 0:
            t0 = u32((m.lw(a1 + 4) >> 6) << 16)
            a3 = 0
            if s0 != 0:
                c, end = s0 + 8, s0 + 8 + 8 * m.lw(s0 + 4)
                while c < end:
                    if m.lw(c) == s3:
                        s4 = a1
                        m.sw(r + 360, s3)
                        a2 = 0
                        a3 = int(0 < m.lws(c + 4))
                        break
                    c += 8
            else:
                v, a0 = m.lw(s4 + 8), m.lw(a1 + 8)
                if v == a0 or v == m.lw(a1 + 12):
                    a2, s1 = 0, u32(-step)
                elif m.lw(s4 + 12) == a0 or m.lw(s4 + 12) == m.lw(a1 + 12):
                    a2, s1 = 0, step
            if a2 != 0:
                s4 = a1
                m.sw(r + 360, s3)
                a3 = int((s32(t0) >> 1) < m.lws(B + 368))
            if s1 == 0:
                m.sw(r + 368, 0 if a3 else t0)
        else:
            if s4 != 0:
                c, end = s2 + 8, s2 + 8 + 8 * m.lw(s2 + 4)
                while c < end:
                    if m.lw(c) == s5:
                        s1 = u32(-step) if m.lws(c + 4) >= 0 else step
                        a2 = 0
                        break
                    c += 8
            if a2 != 0:
                a0 = m.lw(s2 + 8)
                m.sw(r + 360, a0)
                s4 = m.call(0x800245DC, a0)
                if m.lws(s2 + 12) < 0:
                    m.sw(r + 368, 0)
                else:
                    m.sw(r + 368, u32((m.lw(s4 + 4) >> 6) << 16))
    v1 = u32(m.lw(r + 368) + s1)
    m.sw(r + 368, v1)
    if s4 == 0:
        return None
    if s32(v1) < 0:
        m.sw(r + 360, m.lhu(s4 + 8) | 0x10000)
    elif s32((m.lw(s4 + 4) >> 6) << 16) < s32(v1):
        m.sw(r + 360, m.lhu(s4 + 12) | 0x10000)
    return None


def m_spawner_pass(m, dt):
    """0x8008CD88(dt): SpawnerPass"""
    acc = u32(m.lw(SPAWN_ACC) + dt)
    m.sw(SPAWN_ACC, acc)
    if not (s32(acc) < kk("acc", "walker")):
        m.call(0x8009C308)
    a1 = m.lw(SPAWN_ACC)
    if kk("acc", "sched") < s32(a1):
        m.call(0x8009B474, 0, a1)
        m.call(0x8009B474, 3, m.lw(SPAWN_ACC))
        m.sw(SPAWN_ACC, 0)
    return None


def m_sched_dispatch(m, kind, dist):
    """0x8009B474(kind, dist)"""
    if kind == 3:
        m.call(0x8009CFF4, dist)
    elif s32(kind) < 4 and kind == 0:
        m.call(0x8009E89C, dist)
    return None


def m_lane_offset(m, spacing, n, sel):
    """0x8009CF1C(spacing, lanes, sel): the lateral centre of a lane; |sel| == 2 draws one"""
    sb = sel & 0xFF
    sb = sb - 256 if sb & 0x80 else sb
    a = -sb if sb < 0 else sb
    k = 0
    if a == 2:
        s1 = s32(n - 1)
        k = (s1 + (u32(s1) >> 31)) >> 1
        if not (s32(n) < 3):
            r = m.call(RAND)
            k = s32(k + (u32(r) % u32(s1 - k)))
    elif a == 3:
        k = s32(n - 1)
    v = u32(s32(spacing) * k + (s32(spacing) >> 1))
    if sb < 0:
        v = u32(-v)
    return v


def m_lane_lateral(m, car):
    """0x8009E768(car): |the road's own offset| + |0x8009CF1C(width, lanes, car's lane)|"""
    s0, s1 = 0x1CCCC, 0
    v1 = 0
    if m.lhu(car + 362) == 0:
        v1 = m.lw(car + 372)
        if v1 == 0:
            return s0 if m.lws(car + 364) >= 0 else 0xFFFE3334
        if m.lws(car + 364) > 0:
            v0, a1, a0, pos = m.lhs(v1 + 142), m.lhs(car + 420), m.lw(car + 424), True
        else:
            v0, a1, a0, pos = m.lhs(v1 + 14), m.lhs(car + 408), u32(-m.lw(car + 424)), False
    else:
        v1 = m.call(0x8003E1E8, m.lw(car + 328), u32(m.lhs(m.lw(car + 356) + 10)), m.sp + 16)
        if v1 == 0:
            return s0 if m.lws(car + 364) >= 0 else 0xFFFE3334
        a0 = m.lw(v1 + 4)
        if m.lws(m.sp + 16) > 0:
            v0, a1, pos = m.lhs(v1 + 142), m.lhs(v1 + 140), True
        else:
            v0, a1, pos = m.lhs(v1 + 14), m.lhs(v1 + 12), False
    if v0 > 0:
        s1 = m.lw(v1 + (208 if pos else 80))
    s0 = m.call(0x8009CF1C, a0, u32(a1), u32(m.lbs(car + 508)))
    return u32(iabs(s1) + iabs(s0))


def m_density(m, p):
    """0x8009F44C(p): the per-road traffic cap and interval of player p's road"""
    b = m.lw(PLAYERS + 4 * p)
    a0 = m.lw(b + 360)
    if a0 >> 16:
        return None
    if m.lw(b + 372) == 0:
        return None
    r = a0 & 0xFFFF
    r = min(r, kk("density_road_max"))
    t = 0x800524F0 + 2 * r
    a2, a3 = m.lbu(t), m.lbu(t + 1)
    g = gs(m)
    bank = m.lws(g + 60)
    if bank > 0:
        if bank == 1:
            a2, a3 = a2 - 2, a3 + 2
        if bank == 2:
            a2, a3 = a2 - 1, a3 + 1
        if bank == 3:
            a3 += 1
    rt = m.lbu(g + 4)
    if rt & 0x10:
        a2, a3 = a2 - 1, a3 + 4
    if rt & 0x8:
        a2, a3 = a2 - 3, a3 + 2
    top = kk("density_max")
    m.sw(TRAF_BLOCK + 4 + 4 * p, u32(max(a2, 0) + min(top - a2, 0)))
    m.sw(TRAF_BLOCK + 12 + 4 * p, u32(max(a3, 0)))
    return None


def m_traffic_sched(m, acc):
    """0x8009CFF4(acc): the traffic scheduler, once per SpawnerPass round"""
    s4 = u32(m.lw(kk("sched_ahead")) << 16)
    s8 = 0
    sp = m.sp
    m.sw(sp + 112, 0)
    if m.lw(TRAF_ON) == 0:
        return None
    p = 0
    while p < nplayers(m):
        s1 = m.lw(PLAYERS + 4 * p)
        m.sw(sp + 104 + 4 * p, 1)
        m.call(0x8009F44C, p)
        ok = m.call(0x8008CDF4, 3)
        if ok:
            ok = m.call(0x8008DBA8, p)
            if ok:
                ok = m.lw(m.lw(s1 + 1084) + 40) == 0
        if not ok:
            m.sw(sp + 104 + 4 * p, 0)
        if m.lbu(m.lw(s1 + 1084)) & 0x40:
            m.sw(sp + 104 + 4 * p, 0)
        p += 1
    g = gs(m)
    if m.lw(g + 48) == 2 and m.lw(sp + 104) == 0 and m.lw(sp + 108) == 0:
        return None
    if m.lw(g + 48) == 1 and m.lw(sp + 104) == 0:
        return None
    m.sw(TRAF_ACC, u32(m.lw(TRAF_ACC) + acc))

    def tbl():
        g = gs(m)
        rt = m.lbu(g + 4)
        k = m.lw(g + 60) + (6 if rt & 4 else (3 if rt & 1 else 0))
        return m.lw(0x80052FAC + 4 * k)
    if not (s32(u32(tbl() * 300)) >> 16 < m.lws(gs(m) + 16)):
        return None
    if not (s32(u32(tbl() * 600)) >> 16 < m.lws(gs(m) + 16)):
        s8 = 1
    m.call(0x8009FF24, sp + 104)
    p = 0
    while p < nplayers(m):
        if m.lw(sp + 104 + 4 * p) == 0:
            p += 1
            continue
        s1 = m.lw(PLAYERS + 4 * p)
        if not (s32(m.lw(TRAF_BLOCK + 12 + 4 * p) << 16) < m.lws(TRAF_ACC)):
            p += 1
            continue
        s5 = s1 + 360
        flag = m.lw(sp + 104 + 4 * p)
        if flag == 1:
            r = m.call(RAND)
            r100 = r % 100
            spd = m.lws(s1 + 480)
            if kk("fast") < spd:
                pct = m.lhs(TRAF_BLOCK + 36)
            elif m.lbu(gs(m) + 4) == 44:
                pct = kk("pct44")
            elif kk("slow") < spd:
                pct = m.lhs(TRAF_BLOCK + 38)
            else:
                pct = m.lhs(TRAF_BLOCK + 40)
            a0 = pct if pct < 101 else 100
            v0 = 100 - a0
            if v0 > 0 and not (v0 < r100):
                s4 = u32(-s4)
        elif flag == 3:
            s4 = u32(-s4)
        if s32(s4) < 0:
            s4 = u32(-(m.lw(kk("sched_behind")) << 16))
        m.call(0x80012C1C, s5, sp + 88, s4)
        road = m.lw(sp + 88)
        if road >> 16:
            p += 1
            continue
        if m.lbu(gs(m) + 4) & 0x10:
            if u32(road - 11) < 2 or road == 20 or road == 26:
                p += 1
                continue
        v1 = m.lw(s1 + 360)
        if (v1 >> 16) == 0:
            if v1 == road:
                m.sw(sp + 92, 1 if m.lws(s1 + 364) > 0 else u32(-1))
            else:
                leg = m.call(0x8003B4B0, m.lw(s1 + 428), road & 0xFFFF)
                m.sw(sp + 92, m.lw(leg + 4) if leg else 1)
            if s32(s4) > 0:
                if m.lbu(gs(m) + 4) == 44:
                    r = m.call(RAND)
                    if (r % 100) < kk("flip44"):
                        m.sw(sp + 92, u32(-m.lw(sp + 92)))
                elif s8:
                    m.sw(sp + 92, u32(-m.lw(sp + 92)))
                else:
                    m.sw(sp + 92, m.call(0x8003A98C, m.lw(sp + 92), s1))
        else:
            if s8:
                p += 1
                continue
            leg = m.call(0x8003B4B0, m.lw(s1 + 428), road & 0xFFFF)
            if leg:
                if m.call(0x8003F580, m.lw(s1 + 428), m.lhu(sp + 88)):
                    m.sw(sp + 92, u32(-m.lw(leg + 4)))
                else:
                    m.sw(sp + 92, m.lw(leg + 4))
            else:
                m.sw(sp + 92, m.lw(s5 + 4))
        m.sh(sp + 18, 0xFFFF)
        m.sh(sp + 80, 4)
        m.sw(sp + 24, u32(m.lhs(sp + 88)))
        m.sw(sp + 52, m.lw(sp + 96))
        m.sh(sp + 76, m.lhu(sp + 92))
        v = m.call(0x8009F578, sp + 16)
        if m.lw(sp + 24) == m.lw(s1 + 360) and not (kk("dup") < iabs(m.lw(s1 + 368) - m.lw(sp + 52))):
            p += 1
            continue
        if v == 1:
            p += 1
            continue
        e = m.call(0x8009AD48, sp + 16, s1)
        if m.call(0x8009FE90, e):
            m.sw(sp + 112, 1)
        p += 1
    if m.lw(sp + 112):
        m.sw(TRAF_ACC, 0)
    return None


def hi16(v):
    v = (u32(v) >> 16) & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def octagon(dx, dz):
    dx, dz = iabs(dx), iabs(dz)
    big, small = (dz, dx) if dx < dz else (dx, dz)
    s15 = small + (small >> 1)
    return big - (big >> 5) - (big >> 7) + (s15 >> 2) + (s15 >> 6)


def m_traffic_spawn(m, rec, pl):
    """0x8009AD48(rec, player): allocate a pool-3 car from a 68-byte placement record"""
    sp = m.sp
    if m.lhs(TRAF_GATE) == 0:
        return 0
    m.sw(sp + 16, m.lw(rec + 8))
    m.sw(sp + 24, m.lw(rec + 36))
    m.sw(sp + 20, u32(m.lhs(rec + 60)))
    if (m.lw(sp + 16) >> 16) == 1:
        return 0
    if m.lhs(rec + 60) == 0:
        r = m.call(RAND) & 1
        m.sw(sp + 20, r)
        if r == 0:
            m.sw(sp + 20, u32(-1))
    piece = m.call(0x80039DFC, 0, sp + 16)
    if piece == 0:
        return 0
    ctrl = POOL3_CTRL
    a3 = m.lws(ctrl + 4)
    car = 0
    if a3 < 16:
        a1 = a3 + 1
        while a1 < 16:
            slot = u32(ctrl + 16 + 512 * a1)
            if m.lhu(slot + 172) == 0 and m.lw(slot + 4) != 0:
                break
            a1 += 1
        car = u32(ctrl + 16 + 512 * a3)
        m.sw(ctrl + 4, a1)
        m.sh(car + 172, a3 + 96)
        m.sw(ctrl, u32(m.lw(ctrl) + 1))
        if m.lws(ctrl + 8) < a3:
            m.sw(ctrl + 8, a3)
    if car == 0:
        return 0

    def release():
        m.call(0x8008C000, car + 172, 3)
        return 0
    m.sw(car + 180, m.lhu(rec + 2))
    m.sh(car + 322, m.lhu(rec + 62))
    if m.call(0x8003A700, piece, sp + 16, sp + 32) == 0:
        return release()
    if m.lhs(m.lw(sp + 36) + 2) == 1:
        return release()
    m.call(0x8001E0B4, car + 328, sp + 32, 32)
    s3 = m.lw(car + 340)
    m.sw(car + 184, m.lw(s3 + 20))
    m.sw(car + 188, m.lw(s3 + 24))
    m.sw(car + 192, m.lw(s3 + 28))
    m.call(0x8002EAD8, s3 + 20, s3 + 14, m.lw(car + 348), car + 184)
    m.sh(car + 454, 0)
    m.sh(car + 452, 0)
    m.sh(car + 450, 0)
    m.call(0x8003662C, car + 450, car + 328, car + 360)
    if m.lw(m.lw(pl + 852) + 604) < 3:
        ref = pl
    else:
        ref = m.lw(pl + 852)
    rx, rz = m.lw(ref + 184), m.lw(ref + 192)
    rroad, ralong = m.lw(ref + 360), m.lw(ref + 368)
    if m.lw(car + 360) == rroad:
        d = s32(iabs(ralong - m.lw(car + 368))) >> 16
    else:
        d = octagon(hi16(rx) - m.lhs(car + 186), hi16(rz) - m.lhs(car + 194))
    g = gs(m)
    if m.lbu(g + 4) != 44 and m.lws(g + 16) > 0:
        if not (0x77FFFF < s32(u32(d << 16))):
            return release()
    m.sw(car + 364, m.lw(sp + 20))
    s4 = m.lhs(rec + 64)
    if iabs(s4) == 4:
        for _ in range(5):
            lane = m.call(RAND) & 3
            m.sb(car + 508, lane)
            if lane != m.lbs(LAST_LANE):
                break
        m.sb(LAST_LANE, lane)
    else:
        m.sb(car + 508, m.lbu(rec + 64))
    if m.lbs(car + 508) == 0:
        m.sb(car + 508, 1)
    if s4 < 0:
        m.sb(car + 508, (-m.lbu(car + 508)) & 0xFF)
    m.call(0x8003DE28, car, 1, 0, u32(-1))
    if m.lw(car + 372):
        a = m.call(0x8009E768, car)
        b = m.call(0x8009E768, car)
        c = m.call(0x8009E768, car)
        m.sw(car + 344, u32((s32(a) >> 31) + b) ^ u32(s32(c) >> 31))
    else:
        m.sw(car + 344, 0x1CCCC)
    if m.lws(sp + 20) < 0:
        m.sw(car + 344, u32(-m.lw(car + 344)))
    if m.lbs(car + 508) < 0:
        m.sw(car + 344, u32(-m.lw(car + 344)))
    m.call(0x8002EAD8, car + 184, s3 + 2, m.lw(car + 344), car + 184)
    m.call(0x8003DF54, car, 1, u32(-1))
    m.sw(car + 492, 0)
    m.sw(car + 496, 0)
    m.sh(car + 438, m.lhu(s3 + 8))
    m.sh(car + 440, m.lhu(s3 + 10))
    m.sh(car + 438, u32(-m.lhu(car + 438)))
    m.sh(car + 442, m.lhu(s3 + 12))
    m.sh(car + 442, u32(-m.lhu(s3 + 12)))
    m.sh(car + 440, u32(-m.lhu(car + 440)))
    for o, so in ((444, 14), (446, 16), (448, 18), (432, 2), (434, 4), (436, 6)):
        m.sh(car + o, m.lhu(s3 + so))
    if m.lws(sp + 20) < 0:
        v432, v434 = m.lhu(car + 432), m.lhu(car + 434)
        m.sh(car + 432, u32(-v432))
        v436 = m.lhu(car + 436)
        m.sh(car + 434, u32(-v434))
        v444 = m.lhu(car + 444)
        m.sh(car + 436, u32(-v436))
        v446 = m.lhu(car + 446)
        m.sh(car + 444, u32(-v444))
        v448 = m.lhu(car + 448)
        m.sh(car + 446, u32(-v446))
        m.sh(car + 448, u32(-v448))
    m.call(0x8003AF9C, car + 172, 0, pl)
    m.sw(car + 324, m.call(0x8003B61C, car + 172))
    cls = m.call(0x8002FAD4, car, 3, m.lhu(rec + 2), 0)
    m.sw(car + 180, cls)
    if cls == 0xFFFF:
        return release()
    m.call(0x80012FC8, car, 0)
    m.call(0x8008BA18, car)
    ang = m.call(0x80020018, u32(m.lhs(car + 444)), u32(m.lhs(car + 448)))
    m.sw(car + 292, ang)
    sin = m.lhs(0x8005624C + ((ang & 0xFFF) << 2 | 2))
    m.sw(car + 296, u32(sin << 4))
    cos = m.lhs(0x8005624C + ((ang & 0xFFF) << 2))
    m.sw(car + 484, 0x26666)
    m.sh(car + 452, m.lhu(car + 446))
    m.sh(car + 454, m.lhu(car + 448))
    m.sb(car + 509, 0)
    m.sb(car + 510, 0)
    m.sh(car + 450, m.lhu(car + 444))
    m.sw(car + 316, 0x05500000)
    m.sw(car + 504, 0xFFFF0000)
    m.sb(car + 511, 255)
    m.sw(car + 300, u32(cos << 4))
    live = m.call(0x80039F68, car + 172)
    m.sh(car + 320, live)
    m.sw(car + 176, 0xFFFFFFFF)
    out = car
    if m.lhs(car + 320) == 0:
        m.call(0x8008C000, car + 172, 3)
        out = 0
    if not (m.lbu(gs(m) + 4) & 1):
        return out
    if m.lw(out + 180) != 0:
        return out
    m.call(0x80028034, out)
    return out


def m_cop_setup(m, e):
    """0x80094184(e): a released cop's rider record is reset from the saved cop record 0x800D86F0"""
    if e == 0:
        return None
    v1 = m.lw(e + 1084)
    a1 = m.lbu(v1 + 1) & 0xF
    if a1 != 2:
        return None
    a0 = 0x800D86F0
    if m.lbu(a0 + 19) == 0:
        return None
    for dst, src in ((12, 12), (13, 13), (15, 15), (14, 14), (2, 16), (61, 17), (36, 18), (37, 18)):
        m.sb(m.lw(e + 1084) + dst, m.lbu(a0 + src))
    m.sh(m.lw(e + 1084) + 44, m.lhu(a0 + 20))
    m.sb(m.lw(e + 1084) + 46, m.lbu(a0 + 22))
    m.sb(m.lw(e + 1084) + 47, 0)
    m.sw(m.lw(e + 1084) + 48, 0)
    m.sw(m.lw(e + 556) + 224, m.lw(a0 + 8))
    bank = m.lws(gs(m) + 60)
    s3 = m.lbu(m.lw(e + 1084) + 38)
    b = max(bank, 0) + min(a1 - bank, 0)
    s2 = (b << 3) + 66
    for _ in range(8):
        r = m.call(RAND) & 7
        m.sb(m.lw(e + 1084) + 38, s2 + r)
        if m.lbu(m.lw(e + 1084) + 38) != s3:
            break
    return None


def neg_h(m, a):
    m.sh(a, u32(-m.lhu(a)))


def m_placement(m, e):
    """0x8009432C(e): a bike enters the window - it is put on the road, faced, its rider seated"""
    s6 = m.call(0x8001FC90, 0x3C0000, 11)
    s2 = s4 = s0 = 0
    if e == 0:
        return None
    g = gs(m)
    if m.lbu(g + 4) == 44 and m.lbu(g + 57) == 0:
        m.sh(e + 320, 0)
    if m.lhs(e + 320) == 0:
        return None
    g = gs(m)

    def player():
        return m.lhu(e + 172) < m.lw(gs(m) + 48)

    def cls():
        return m.lbu(m.lw(e + 1084) + 1) & 0xF
    if (m.lbu(g + 4) & 1) and not player() and m.lbu(m.lw(e + 1084) + 39) == 254:
        s3 = 61
        s6 = m.call(0x8001FC90, 0xA0000, 11)
        m.sw(e + 560, m.lw(e + 560) | 0x20000000)
    elif m.lbu(gs(m) + 57) == 1:
        s3 = 67 if m.lw(m.lw(m.lw(0x8005B38C) + 1084) + 40) != 0 else 123
        s2 = m.lw(e + 344)
        s6 = m.lw(e + 292)
        m.sw(e + 924, 0)
    else:
        s3 = None
        if m.lbu(gs(m) + 57) >= 3 and not player():
            a2 = m.lw(e + 1084)
            if (m.lbu(a2 + 1) & 0xF) == (m.lbu(m.lw(m.lw(0x8005B38C) + 1084) + 1) & 0xF):
                s3 = 85
                m.sb(a2, m.lbu(a2) | 0x10)
                s6 = m.call(0x8001FC90, 0x9B0000, 11)
        if s3 is None:
            R = m.lw(e + 852)
            s3 = 1
            if not (m.lw(R + 604) < 3) and m.lhs(R + 320) != 0:
                s3 = 161
            if not player():
                if m.lw(gs(m) + 48) == 2:
                    p0, p1 = m.lw(PLAYERS), m.lw(PLAYERS + 4)
                    s4 = p1 if m.lws(p1 + 324) < m.lws(p0 + 324) else p0
                else:
                    s4 = m.lw(0x8005B38C)
                if not (m.lws(0x8005B1F8) < m.lbu(m.lw(s4 + 1084) + 39)):
                    rd_ = m.lw(e + 1084)
                    if m.lw(rd_ + 40) != 0:
                        if s32(8 >> (m.lw(gs(m) + 48) - 1)) < m.lbu(rd_ + 39):
                            s3 = 0
                        else:
                            a1 = m.lw(m.lw(0x800D6188) + 4)
                            m.sw(e + 368, a1)
                            pl = m.lbu(m.lw(e + 1084) + 39)
                            lo = s32((pl >> 1) * (1 - ((pl & 1) << 1)))
                            s3 |= 0x3C
                            m.sw(e + 560, m.lw(e + 560) | 0x20000000)
                            m.sw(e + 368, u32(a1 + u32((lo * 3) << 16)))
    if s3 != 0:
        s0 = m.call(0x8003A468, e + 360, e + 328)
    if s0 == 0 or s3 == 0:
        g = gs(m)
        m.sh(e + 320, 0)
        if m.lhu(e + 172) < m.lw(g + 48):
            return None
    else:
        s5 = m.call(0x8003B4B0, m.lw(e + 428), m.lw(m.lw(e + 332) + 12))
        m.call(0x8003DE28, e, 1, 0, u32(-1))
        if s3 & 4:
            g = gs(m)
            if m.lbu(g + 57) >= 3:
                a0 = ((m.lhu(e + 172) >> 1) - 1) & 1
                if a0 == 0:
                    s6 = u32(-s6)
            elif m.lw(g + 48) == 2:
                a0 = int(m.lw(m.lw(0x800D6188)) == 0x20)
            else:
                a0 = 0
            if a0:
                m.sw(e + 344, u32(m.lw(e + 412) - (m.lw(e + 308) << 1)))
            else:
                m.sw(e + 344, u32(m.lw(e + 400) + (m.lw(e + 308) << 1)))
            m.sw(e + 924, 0)
        top = m.lhu(u32(e + 956 + 8 * (m.lbs(e + 946) - 1)))
        if s3 & 2:
            m.sw(e + 344, s2)
        else:
            go_b4 = bool(s3 & 4)
            if not go_b4:
                if (not player() and cls() == 2) or top == 1:
                    go_b4 = True
                else:
                    m.sw(e + 344, 0)
            if go_b4 and not player() and cls() == 2 and top == 1:
                m.sw(e + 344, m.lw(e + 416) if m.lws(e + 364) > 0 else m.lw(e + 404))
        s2 = m.lw(e + 340)
        m.call(0x8002EAD8, s2 + 20, s2 + 14, m.lw(e + 348), e + 184)
        m.call(0x8002EAD8, e + 184, s2 + 2, m.lw(e + 344), e + 184)
        x, y, z = m.lw(e + 184), m.lw(e + 188), m.lw(e + 192)
        for o, v in ((504, x), (508, y), (512, z), (468, x), (472, y), (476, z)):
            m.sw(e + o, v)
        m.call(0x80037450, e)
        m.call(0x8003DF54, e, 1, u32(-1))
        for o, so in ((432, 2), (434, 4), (436, 6), (438, 8), (440, 10)):
            m.sh(e + o, m.lhu(s2 + so))
        v438, v12 = m.lhu(e + 438), m.lhu(s2 + 12)
        m.sh(e + 438, u32(-v438))
        v440 = m.lhu(e + 440)
        m.sh(e + 442, v12)
        m.sh(e + 442, u32(-v12))
        m.sh(e + 440, u32(-v440))
        for o, so in ((444, 14), (446, 16), (448, 18)):
            m.sh(e + o, m.lhu(s2 + so))
        if s5:
            m.sw(e + 364, m.lw(s5 + 4))
            if m.lws(s5 + 4) < 0:
                for o in (444, 446, 448, 432, 434, 436):
                    neg_h(m, e + o)
        else:
            m.sw(e + 364, 1)
        if not player() and cls() == 2:
            if m.lw(gs(m) + 48) != 2:
                s4 = m.lw(0x8005B38C)
            else:
                ds = []
                for p in range(m.lw(gs(m) + 48)):
                    pb = m.lw(PLAYERS + 4 * p)
                    ds.append(octagon(m.lhs(e + 186) - m.lhs(pb + 186), m.lhs(e + 194) - m.lhs(pb + 194)))
                s4 = m.lw(PLAYERS + 4 * int(ds[1] < ds[0]))
            if m.lw(s4 + 360) == m.lw(e + 360) and s32(m.lw(e + 364) ^ m.lw(s4 + 364)) < 0:
                for o in (444, 448, 446, 434, 432):
                    neg_h(m, e + o)
                m.sw(e + 364, u32(-m.lw(e + 364)))
                neg_h(m, e + 436)
        if s3 & 0x10:
            m.call(0x8003FB34, s2 + 8, s6, m.sp + 24)
            m.call(0x8003FA40, e + 432, m.sp + 24, e + 432)
        hv = [m.lhu(e + 432 + 2 * k) for k in range(9)]
        for k in range(9):
            m.sh(e + 516 + 2 * k, hv[k])
        m.call(0x8002090C, e)
        if s3 & 0x88:
            if s3 & 8:
                m.sw(e + 636, 28595)
                m.sw(e + 652, u32(m.lw(e + 668) + 28595))
            else:
                m.sw(e + 652, 0x1921F)
                m.sw(e + 636, u32(0x1921F - m.lw(e + 668)))
            m.call(0x8003FB34, e + 444, u32(s32(u32(m.lw(e + 652) * 652)) >> 16), m.sp + 24)
            m.call(0x8003FA40, e + 432, m.sp + 24, e + 432)
            m.sw(e + 568, m.lw(e + 568) | 0x100)
        if s3 & 0x20:
            m.call(0x80068D20, e, m.lw(e + 852), 0)
        m.call(0x8008BA18, e)
        ang = m.call(0x80020018, u32(m.lhs(e + 444) << 4), u32(m.lhs(e + 448) << 4))
        m.sw(e + 292, ang)
        m.sw(e + 296, u32(m.lhs(0x8005624C + (((ang & 0xFFF) << 2) | 2)) << 4))
        m.sw(e + 300, u32(m.lhs(0x8005624C + ((ang & 0xFFF) << 2)) << 4))
        m.call(0x8002EAD8, e + 184, e + 528, u32(m.lw(e + 308) << 1), e + 880)
        v924 = m.lw(e + 924)
        m.sh(e + 450, m.lhu(e + 444))
        m.sw(e + 480, v924)
        m.sw(e + 576, v924)
        for o, so in ((452, 446), (454, 448), (814, 432), (816, 434), (818, 436)):
            m.sh(e + o, m.lhu(e + so))
        big = 0xA0000 < s32(v924)
        m.sb(e + 849, (m.lbu(m.lw(e + 556) + 444) - 1) & 0xFF if big else 0)
        if (s3 & 0x40) and m.lhu(e + 956) == 1:
            m.sh(e + 960, 1)
            m.sw(e + 564, m.lw(e + 564) & 0xFFFFFDFF)
        if not (s3 & 0x20):
            R = m.lw(e + 852)
            m.call(0x8001E0B4, R + 328, e + 328, 32)
            for o in (184, 188, 192):
                m.sw(R + o, m.lw(e + o))
            for o, so in ((468, 184), (472, 188), (476, 192)):
                m.sw(R + o, m.lw(e + so))
            v320 = m.lhu(e + 320)
            m.sw(R + 552, 0)
            m.sh(R + 320, v320)
            if not (s3 & 8):
                m.call(0x8007EC30, e)
            m.call(0x80012838, e, R, 2, 0)
            a0 = m.lw(e + 924)
            m.sb(e + 72, 0)
            m.sw(e + 480, a0)
            m.sw(e + 576, a0)
            m.sw(e + 580, m.call(0x8001FC90, a0, a0))
            big = 0xA0000 < m.lws(e + 480)
            m.sb(e + 849, (m.lbu(m.lw(e + 556) + 444) - 1) & 0xFF if big else 0)
            for o in (444, 446, 448, 438, 440, 442, 432, 434, 436):
                m.sh(R + o, m.lhu(e + o))
            if not player() and cls() == 2 and m.lw(R + 540) == 0:
                m.sw(R + 540, m.call(0x80012884, 0x800CE170, R))
            m.call(0x80012858, m.lw(R + 540), m.lw(0x800CE190))
            if not player() and cls() == 2 and not (m.lbu(e + 928) & 0x10):
                m.sw(R + 604, 0)
                ev = 4
            else:
                m.sw(R + 604, 1)
                ev = 11
            m.call(0x800C4550, ev, R, 1)
        m.sw(e + 568, m.lw(e + 568) | 0x08000000)
    if player() or cls() != 2:
        return None
    if m.lhs(e + 320) == 0 or not (m.lbu(e + 928) & 0x10):
        m.call(0x8009DAF8, 1)
        m.sb(e + 928, (m.lbu(e + 928) | 0x10) & 0xDF)
        m.call(0x8003AF9C, e + 172, 0, s4)
        m.sw(m.lw(e + 1084) + 40, 0)
        m.call(0x800BCD10, e)
        m.call(0x800BCA68, m.sp + 16, 1, e)
    else:
        m.call(0x800BCD10, e)
        m.call(0x800BCA68, m.sp + 16, 1, e)
    m.call(0x80028034, e)
    return None


# function -> (model, number of register args used for the call record)
MODELS = {
    0x80013110: m_window_pred, 0x80039F68: m_road_window, 0x80093E6C: m_activation_pass,
    0x80093ED4: m_activate, 0x80093F94: m_transition, 0x80093FE4: m_retire, 0x800950E8: m_downed_pass,
    0x800951B8: m_downed, 0x800952AC: m_rider_transition, 0x80099D48: m_rider_chase,
    0x8008CD88: m_spawner_pass, 0x8009B474: m_sched_dispatch, 0x8009CF1C: m_lane_offset,
    0x8009E768: m_lane_lateral, 0x8009F44C: m_density, 0x8009CFF4: m_traffic_sched,
    0x8009AD48: m_traffic_spawn, 0x8009432C: m_placement, 0x80094184: m_cop_setup,
    # the road-coordinate helpers under the window test
    0x80039CFC: m_piece_contains, 0x80039DFC: m_road_gate, 0x80037524: m_seat_along,
    0x8003A700: m_cursor_seat, 0x8003A468: m_cursor_reseat,
}


# ---------------------------------------------------------------------------------------------
# the runs: snapshot copies that differ from the original savestate only in DATA
# ---------------------------------------------------------------------------------------------

BOX = [0x0B8] + [0x0C4 + 12 * k for k in range(8)]      # the box centre and the eight corners (x; z is +8)
BIKE_BOX = [0x1D4, 0x0B8, 0x1F8, 0x310] + [0x0C4 + 12 * k for k in range(8)]


def bike(k):
    return PLAYER + BIKE_STRIDE * k


def rider(k):
    return RIDER0 + RIDER_STRIDE * k


def shift_x(e, fields, dx):
    return [(e + f, dx, None) for f in fields]


def pos_at_window_test(run, e):
    """(x_hi, z_hi) of entity e when frame 1 first asks the window predicate about it, in an unmodified run"""
    src = {n: s_ for n, s_, _ in RUNS}[run]
    fr = {n: f_ for n, _, f_ in RUNS}[run]
    tr = run_trace(run, src, fr, claims())[0]
    for seq, regs in tr.probes.get(0x80013110, []):
        if regs["a1"] == e + 0xB8:
            r = ram_at(tr, seq)
            return rd(r, e + 0xBA, "<h"), rd(r, e + 0xC2, "<h")
    raise RuntimeError(f"no window test of {e:#x} in {run}")


def view_at(x_hi, z_hi, big):
    """DATA edit: player 0's view record placed `big` integer units off (x_hi, z_hi) along x, so that the
    octagonal distance of 0x80013110 is big - (big >> 5) - (big >> 7)"""
    return [(VIEWS + 0xB8, None, ((x_hi + big) & 0xFFFF) << 16), (VIEWS + 0xC0, None, (z_hi & 0xFFFF) << 16)]


def edits_for(name, ram):
    fx = lambda v: int(round(v * 65536))
    pl_along = rd(ram, PLAYER + 0x170)
    return {
        # a dormant AI bike (15) given a road coordinate 60 units ahead of / behind the player, same road
        "wake": lambda: [(bike(15) + 0x170, None, u32(pl_along - fx(60)))],
        "wake-behind": lambda: [(bike(15) + 0x170, None, u32(pl_along + fx(60)))],
        # a live AI bike (5) moved 600 units away in x, every position field with it
        "retire": lambda: shift_x(bike(5), BIKE_BOX, fx(600)),
        # SpawnerPass one tick from its round, and the police accumulator B at the 30.0 threshold
        "sched": lambda: [(SPAWN_ACC, None, 0xFFF0), (0x8005B368, None, 0x1E0000)],
        # the one traffic car moved 600 units away
        "car-far": lambda: shift_x(CAR0, BOX, fx(600)),
        # the traffic scheduler one tick from its round with the one car removed from pool 3 (the pool
        # control block written the way an empty pool reads in the quick snapshot: live 0, next 0, high -1)
        "sched-empty": lambda: [(SPAWN_ACC, None, 0xFFF0),
                                (CAR0 + 0xAC, None, rd(ram, CAR0 + 0xAC, "<I") & 0xFFFF0000),
                                (CAR0 + 0x140, None, rd(ram, CAR0 + 0x140, "<I") & 0xFFFF0000),
                                (POOL3_CTRL, None, 0), (POOL3_CTRL + 4, None, 0), (POOL3_CTRL + 8, None, 0xFFFFFFFF)],
        # the downed rider of bike 6 moved 600 units away, its road coordinate left alone: dormant in frame 1,
        # re-seated from that road coordinate and back in the window in frame 2
        "rider-back": lambda: shift_x(rider(6), BOX, fx(600)),
        # a dormant AI bike (15) given a road coordinate on a road no resident piece holds (road 28, along 100)
        "far-road": lambda: [(bike(15) + 0x168, None, 28), (bike(15) + 0x170, None, fx(100))],
        # EDGE runs: the view record of player 0 (read by the window predicate before the frame's own camera
        # update rewrites it) put at an exact octagonal distance from one entity, one unit either side of a radius
        "edge-bike-299": lambda: view_at(*pos_at_window_test("rr-race", bike(15)), 310),
        "edge-bike-300": lambda: view_at(*pos_at_window_test("rr-race", bike(15)), 311),
        "edge-live-349": lambda: view_at(*pos_at_window_test("quick", bike(5)), 362),
        "edge-live-350": lambda: view_at(*pos_at_window_test("quick", bike(5)), 363),
        "edge-car-229": lambda: view_at(*pos_at_window_test("rr-race", CAR0), 237),
        "edge-car-230": lambda: view_at(*pos_at_window_test("rr-race", CAR0), 238),
        # the downed rider of bike 6 moved 600 units away AND 400 units back along its road
        "rider-far": lambda: shift_x(rider(6), BOX, fx(600))
        + [(rider(6) + 0x170, None, u32(rd(ram, rider(6) + 0x170) - fx(400)))],
    }[name]()


RUNS = [  # name, source savestate, frames
    ("rr-race", "rr-race", 40), ("quick", "quick", 6), ("wake", "rr-race", 6), ("wake-behind", "rr-race", 6),
    ("retire", "quick", 4), ("sched", "rr-race", 8), ("sched-empty", "rr-race", 8), ("car-far", "rr-race", 4),
    ("rider-far", "rr-race", 10), ("rider-back", "rr-race", 4), ("far-road", "rr-race", 2),
    ("edge-bike-299", "rr-race", 2), ("edge-bike-300", "rr-race", 2), ("edge-live-349", "quick", 2),
    ("edge-live-350", "quick", 2), ("edge-car-229", "rr-race", 2), ("edge-car-230", "rr-race", 2),
]

EXTRA_PROBES = [0x8008AB00, RAND]   # RaceTick (the frame boundary) and Rand's entry


def prepare(name, src):
    dst = os.path.join(WORK, "probe", "state-" + name)
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    shutil.copytree(os.path.join(STATE_DIR, src), dst)
    p = os.path.join(dst, "ram.bin")
    ram = bytearray(open(p, "rb").read())
    edits = [] if name == src else edits_for(name, bytes(ram))
    for a, delta, value in edits:
        o = a & 0x1FFFFF
        v = u32(struct.unpack_from("<I", ram, o)[0] + delta) if delta is not None else value
        struct.pack_into("<I", ram, o, v)
    open(p, "wb").write(ram)
    return dst, bytes(ram), edits


def probe_points(c):
    ps = set(EXTRA_PROBES)
    for f in MODELS:
        end, _ = c["extent"][f]
        ps.add(f)
        ps.update(a + 8 for a in jals_in(f, end))
        ps.update(a + 8 for a in all_call_sites(f))
    return sorted(ps)


_TRACES = {}


def run_trace(name, src, frames, c):
    if name in _TRACES:
        return _TRACES[name]
    state, ram, edits = prepare(name, src)
    out = os.path.join(WORK, "probe", "tr-" + name)
    cmd = [RRVERIFY, "trace", "--state", state, "--frames", str(frames), "--no-cop2", "--no-gpu",
           "--pad", "0xFFFF", "--calls", "--out", out]
    for a, n, w in WATCHES:
        cmd += ["--watch", f"{a:#x}:{n}:{w}"]
    for p in probe_points(c):
        cmd += ["--probe", f"{p:#x}:p{p:x}"]
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
    stop = [ln.split(None, 1)[1].strip() for ln in r.stdout.splitlines() if ln.startswith(("stopped", "buffer swaps"))]
    tr = P.Trace(out, ram)
    tr.name, tr.stop, tr.edits = name, stop, edits
    _TRACES[name] = (tr, stop, edits)
    return _TRACES[name]


def all_traces(c):
    return {name: run_trace(name, src, fr, c)[0] for name, src, fr in RUNS}


# ---------------------------------------------------------------------------------------------
# the model-vs-interpreter harness (pairs.py's Machine, with explicit extents for SLUS functions)
# ---------------------------------------------------------------------------------------------

def run_model(tr, f, c):
    """every call of `f` in the trace: [(seq, ok, why)]"""
    end, frame = c["extent"][f]
    model = MODELS[f]
    rets = [a + 8 for a in all_call_sites(f)]
    out = []
    base = bytearray(tr.ram0)
    bi = 0
    W = tr.writes
    for seq, regs in tr.probes.get(f, []):
        ex = None
        for rs in rets:
            p = tr.post(rs, seq, regs["sp"])
            if p and (ex is None or p[0] < ex[0]):
                ex = p
        if ex is None:
            out.append((seq, None, "truncated"))
            continue
        while bi < len(W) and W[bi][0] < seq:
            P.put(base, W[bi][2], W[bi][3], W[bi][4])
            bi += 1
        m = P.Machine(tr, bytearray(base), seq, ex[0], regs, f, end, frame)
        try:
            v0 = model(m, regs["a0"], regs["a1"], regs["a2"], regs["a3"]) if model.__code__.co_argcount == 5 else \
                model(m, *[regs[r] for r in ("a0", "a1", "a2", "a3")][:model.__code__.co_argcount - 1])
            i0, i1 = bisect.bisect_right(tr.wseq, seq), bisect.bisect_left(tr.wseq, ex[0])
            g = [(a, n, v) for sq, pc, a, n, v in W[i0:i1]
                 if f <= pc < end and not any(s0 < sq < s1 and pc != d for s0, s1, d in m.windows)
                 and not (m.sp <= a < regs["sp"])]
            mw = [w for w in m.mwrites if not (m.sp <= w[0] < regs["sp"]) and watched(w[0], w[1])]
            if m.ci != len(m.gcalls):
                cc = m.gcalls[m.ci]
                raise Mismatch(f"model made {m.ci} calls, guest {len(m.gcalls)} (next {cc[2]:#x} from {cc[1]:#x})")
            if mw != g:
                i = next((i for i, (x, y) in enumerate(zip(mw, g)) if x != y), min(len(mw), len(g)))
                raise Mismatch(f"write #{i}: model {[(hex(a), n, hex(v)) for a, n, v in mw[i:i + 2]]} "
                               f"guest {[(hex(a), n, hex(v)) for a, n, v in g[i:i + 2]]} ({len(mw)} vs {len(g)})")
            if v0 is not None and u32(v0) != ex[1]["v0"]:
                raise Mismatch(f"v0 model {u32(v0):#x} guest {ex[1]['v0']:#x}")
            out.append((seq, True, f"{len(g)} writes, {m.ci} calls"))
        except Mismatch as e:
            out.append((seq, False, str(e)))
        except (struct.error, IndexError, TypeError) as e:
            out.append((seq, False, f"model fault: {e!r}"))
    return out



# ---------------------------------------------------------------------------------------------
# facts read from the runs
# ---------------------------------------------------------------------------------------------

def ticks(tr):
    return [sq for sq, _ in tr.probes.get(0x8008AB00, [])]


def frame_no(tr, seq):
    return bisect.bisect_right(ticks(tr), seq)


def calls_to(tr, target):
    return [(frame_no(tr, cc[0]), cc[1], cc[0]) for cc in tr.calls if cc[2] == target]


def field_writes(tr, base, stride, count, off, size=2):
    """[(frame, pc, slot, value)] of every write to entity field `off` of a pool"""
    out = []
    for sq, pc, a, n, v in tr.writes:
        if n == size and base <= a < base + stride * count and (a - base) % stride == off:
            out.append((frame_no(tr, sq), pc, (a - base) // stride, v))
    return out


def changes(ws):
    last, out = {}, []
    for f, pc, slot, v in ws:
        if last.get(slot) != v:
            out.append((f, pc, slot, v))
            last[slot] = v
    return out


def ram_at(tr, seq):
    return P.snapshots(tr, [seq])[seq]


def rand_census(tr):
    """{frame: [call site of every Rand call]}"""
    per = defaultdict(list)
    for f, site, _ in calls_to(tr, RAND):
        per[f].append(site)
    return dict(per)


def facts(traces):
    F = {}
    fx = lambda v: s32(v) / 65536
    # --- rr-race: the natural run
    tr = traces["rr-race"]
    rounds = sorted({f for f, site, _ in calls_to(tr, 0x8009B474)})
    spawns = calls_to(tr, 0x8009AD48)
    cen = rand_census(tr)
    plain = Counter(tuple(v) for f, v in cen.items() if f not in rounds)
    new = []
    for seq, regs in tr.probes.get(0x8009AD48, []):
        ex = tr.post(0x8009D5F0, seq, regs["sp"])
        r = ram_at(tr, ex[0])
        car = ex[1]["v0"]
        new.append({"frame": frame_no(tr, seq), "handle": rd(r, car + 0xAC, "<H"), "road": rd(r, car + 0x168, "<I"),
                    "ahead": fx(rd(r, PLAYER + 0x170) - rd(r, car + 0x170)), "dir": rd(r, car + 0x16C),
                    "pdir": rd(r, PLAYER + 0x16C), "lane": rd(r, car + 0x1FC, "<b"), "live": rd(r, car + 0x140, "<h"),
                    "speed": fx(rd(r, car + 0x1E4))})
    dorm = {}
    for k in list(range(1, 6)) + list(range(7, 16)):
        b_ = bike(k)
        dorm[k] = (sum(1 for sq, pc, a, n, v in tr.writes if a == b_ + 0x170 and pc == 0x8009765C),
                   sum(1 for sq, pc, a, n, v in tr.writes if a == b_ + 0x39C and pc == 0x80095810))
    F["rr-race-dormant"] = dorm
    F["rr-race"] = {"rounds": rounds, "round_order": [site for site in (calls_to(tr, 0x8009E89C) + calls_to(tr, 0x8009CFF4))
                                                      and [c[1] for c in sorted(calls_to(tr, 0x8009E89C) + calls_to(tr, 0x8009CFF4), key=lambda c: c[2])]],
                    "cars_spawned": len(spawns), "new_cars": new,
                    "placements": len(calls_to(tr, 0x8009432C)), "retires": len(calls_to(tr, 0x80093FE4)),
                    "rand_plain_frames": {k: v for k, v in plain.items()},
                    "rand_round": [cen.get(f, []) for f in rounds],
                    "walker_frames": sorted({f for f, _, _ in calls_to(tr, 0x8009C308)})}
    # --- wake / wake-behind: a dormant AI bike given a road coordinate near the player
    for name in ("wake", "wake-behind"):
        tr = traces[name]
        ch = changes(field_writes(tr, PLAYER, BIKE_STRIDE, 18, 0x140))
        rch = changes(field_writes(tr, RIDER0, RIDER_STRIDE, 18, 0x140))
        pl = calls_to(tr, 0x8009432C)
        info = {}
        if pl:
            seq = pl[0][2]
            post = [s_ for s_, _ in tr.probes.get(0x80093FD4, []) if s_ > seq]
            r = ram_at(tr, post[0]) if post else None
            if r:
                b = bike(15)
                edited = [v for a_, d_, v in tr.edits if a_ == b + 0x170][0]
                info = {"along_minus_player": fx(rd(r, b + 0x170) - rd(r, PLAYER + 0x170)), "dir": rd(r, b + 0x16C),
                        "off_edited_along": s32(rd(r, b + 0x170, "<I") - edited),
                        "lateral": fx(rd(r, b + 0x158)), "rider_live": rd(r, rider(15) + 0x140, "<h"),
                        "flagsC_bit27": (rd(r, b + 0x238, "<I") >> 27) & 1}
        b15 = bike(15)
        info["refile"] = [(frame_no(tr, sq), pc, v) for sq, pc, a, n, v in tr.writes
                          if (a == b15 + 0x238 and pc in (0x80094FA0, 0x80071D0C)) or (a == b15 + 0x440 and pc == 0x80071D04)][:3]
        F[name] = {"woken": sorted({slot for f, pc, slot, v in ch if f == 1 and pc == 0x80093F7C and v == 1 and slot != 6}),
                   "placements": len(pl), "placement_from": [hex(c[1]) for c in pl], "retires": len(calls_to(tr, 0x80093FE4)),
                   "rider_woken_by": sorted({hex(pc) for f, pc, slot, v in rch if slot == 15 and v == 1}), **info}
    # --- retire: a live AI bike moved out of the window
    tr = traces["retire"]
    ch = changes(field_writes(tr, PLAYER, BIKE_STRIDE, 18, 0x140))
    rch = changes(field_writes(tr, RIDER0, RIDER_STRIDE, 18, 0x140))
    F["retire"] = {"retired": sorted({slot for f, pc, slot, v in ch if v == 0 and f == 1 and pc == 0x80093F7C}),
                   "retires": len(calls_to(tr, 0x80093FE4)),
                   "rider_retired_by": sorted({hex(pc) for f, pc, slot, v in rch if slot == 5 and v == 0})}
    # --- sched: the police scheduler releases a cop; the traffic scheduler runs in frame 1
    tr = traces["sched"]
    ch = changes(field_writes(tr, PLAYER, BIKE_STRIDE, 18, 0x140))
    pl = calls_to(tr, 0x8009432C)
    cop = {}
    if pl:
        post = [s_ for s_, _ in tr.probes.get(0x8009EEF4, []) if s_ > pl[0][2]]
        if post:
            r = ram_at(tr, post[0])
            e = bike(16)
            cop = {"cop_ahead": fx(rd(r, PLAYER + 0x170) - rd(r, e + 0x170)), "cop_dir": rd(r, e + 0x16C),
                   "cop_speed_39c": fx(rd(r, e + 0x39C)), "cop_3a0": rd(r, e + 0x3A0, "<B"),
                   "cop_cmd_top": rd(r, e + 0x3B4 + 8 * rd(r, e + 0x3B2, "<b"), "<H")}
            r2 = ram_at(tr, ticks(tr)[1])                      # the next RaceTick: after the release's own push
            cop["cop_stack_next_tick"] = [(rd(r2, e + 0x3B4 + 8 * i, "<H"), rd(r2, e + 0x3B6 + 8 * i, "<H"))
                                          for i in range(1, rd(r2, e + 0x3B2, "<b") + 1)]
    F["sched"] = {"cop_released": len([1 for f, pc, slot, v in ch if pc == 0x8009EEE4 and v == 1]),
                  "cop_slot": sorted({slot for f, pc, slot, v in ch if pc == 0x8009EEE4}),
                  "cop_setup_from": [hex(c[1]) for c in calls_to(tr, 0x80094184)],
                  "placement_from": [hex(c[1]) for c in pl],
                  "cars_spawned": len(calls_to(tr, 0x8009AD48)), "rand_frame1": rand_census(tr).get(1, []), **cop}
    tr = traces["sched-empty"]
    new = []
    for seq, regs in tr.probes.get(0x8009AD48, []):
        ex = tr.post(0x8009D5F0, seq, regs["sp"])
        r = ram_at(tr, ex[0])
        car = ex[1]["v0"]
        new.append({"handle": rd(r, car + 0xAC, "<H"), "ahead": fx(rd(r, PLAYER + 0x170) - rd(r, car + 0x170)),
                    "dir": rd(r, car + 0x16C), "lane": rd(r, car + 0x1FC, "<b"), "live": rd(r, car + 0x140, "<h")})
    F["sched-empty"] = {"cars_spawned": len(calls_to(tr, 0x8009AD48)), "new_cars": new,
                        "rand_frame1": rand_census(tr).get(1, []),
                        "rand_later": sorted({len(v) for f, v in rand_census(tr).items() if f > 1} | {0})}
    # --- car-far: the traffic car leaves the window
    tr = traces["car-far"]
    rel = [(f, hex(site)) for f, site, sq in calls_to(tr, 0x8008C000)]
    cw = changes(field_writes(tr, CAR0, 512, 16, 0x140))
    live_after = None
    if rel:
        r = ram_at(tr, [sq for f, site, sq in calls_to(tr, 0x8008C000)][0] + 1000)
        live_after = rd(r, POOL3_CTRL)
    F["car-far"] = {"car_despawned": len(rel), "release_from": rel, "car_live_writes": [(f, hex(pc), v) for f, pc, _, v in cw],
                    "pool3_live_after": live_after}
    # --- rider-far: the downed rider leaves the window, then its road coordinate walks toward its bike
    tr = traces["rider-far"]
    rch = changes(field_writes(tr, RIDER0, RIDER_STRIDE, 18, 0x140))
    al = [(frame_no(tr, sq), s32(v)) for sq, pc, a, n, v in tr.writes if a == rider(6) + 0x170 and pc in range(0x80099D48, 0x8009A038)]
    steps = sorted({b - a for (_, a), (_, b) in zip(al, al[1:])})
    F["rider-far"] = {"rider_dormant": sorted({slot for f, pc, slot, v in rch if v == 0 and f == 1}),
                      "dormant_by": sorted({hex(pc) for f, pc, slot, v in rch if v == 0}),
                      "chase_calls": len(calls_to(tr, 0x80099D48)), "chase_steps": [fx(x) for x in steps],
                      "woke_again": sorted({f for f, pc, slot, v in rch if slot == 6 and v != 0 and f > 1})}
    tr = traces["rider-back"]
    rch = changes(field_writes(tr, RIDER0, RIDER_STRIDE, 18, 0x140))
    F["rider-back"] = {"rider6": [(f, hex(pc), v) for f, pc, slot, v in rch if slot == 6],
                       "reentry_calls": [hex(site) for f, site, _ in calls_to(tr, 0x8003A468)]}
    tr = traces["far-road"]
    res = []
    for seq, regs in tr.probes.get(0x80039F68, []):
        if regs["a0"] == bike(15) + 0xAC and frame_no(tr, seq) == 1:
            ex = [s_ for s_, r_ in tr.probes.get(0x80093F58, []) if s_ > seq]
            dfc = [r_["v0"] for s_, r_ in tr.probes.get(0x80039F9C, []) if s_ > seq][:1]
            res.append((dfc, [r_["v0"] for s_, r_ in tr.probes.get(0x80093F58, []) if s_ > seq][:1]))
    F["far-road"] = {"piece_and_window": res[:1]}
    # --- the edges: frame 1 of each, the entity's +0x140 after its window test
    E_ = {}
    for name, e, off, n in (("edge-bike-299", bike(15), 0x140, "bike15"), ("edge-bike-300", bike(15), 0x140, "bike15"),
                            ("edge-live-349", bike(5), 0x140, "bike5"), ("edge-live-350", bike(5), 0x140, "bike5"),
                            ("edge-car-229", CAR0, 0x140, "car"), ("edge-car-230", CAR0, 0x140, "car")):
        tr = traces[name]
        ws = [v for sq, pc, a, nn, v in tr.writes if a == e + off and frame_no(tr, sq) == 1
              and pc in (0x80093F7C, 0x8009A320)]
        E_[name] = ws[0] if ws else None
    F["edges"] = E_
    # --- quick: a field of 14 riding AI bikes and no traffic yet
    tr = traces["quick"]
    F["quick"] = {"rand_calls": sum(len(v) for v in rand_census(tr).values()),
                  "sched_spawns": len(calls_to(tr, 0x8009AD48)), "rounds": sorted({f for f, _, _ in calls_to(tr, 0x8009B474)})}
    return F


# ---------------------------------------------------------------------------------------------
# checks
# ---------------------------------------------------------------------------------------------

class Bench:
    def __init__(self, verbose):
        self.res = []
        self.verbose = verbose

    def check(self, claim, ok, what):
        self.res.append((claim, bool(ok), what))
        if self.verbose and not ok:
            print(f"  FAIL [{claim}] {what}")
        return ok


def check_skeleton(b, c):
    im = images()
    b.check("sha1", im["SLUS"].sha1 == c["sha1"]["SLUS"] and im["G"].sha1 == c["sha1"]["G"],
            f"image hashes SLUS {im['SLUS'].sha1} RASHCDG {im['G'].sha1}")
    for f, (end, frame) in c["extent"].items():
        # the extent: the word before `end` is the delay slot of the function's last jr ra
        last = word_at(end - 8)
        b.check("extent", last == 0x03E00008, f"{f:#x}: the word at {end - 8:#x} is jr ra ({last:#010x}); "
                                              f"{(end - f) // 4} instructions")
        b.check("extent", frame_of(f, end) == frame, f"{f:#x}: frame {frame_of(f, end)} bytes")
    for f, want in c["callees"].items():
        end = c["extent"][f][0]
        got = callees(f, end)
        b.check("callees", got == want, f"{f:#x} callees: extra {sorted(hex(x) for x in got - want)} "
                                        f"missing {sorted(hex(x) for x in want - got)}")
    for f, want in c["callers"].items():
        got = all_call_sites(f)
        b.check("callers", got == sorted(want), f"{f:#x} callers {[hex(x) for x in got]}")
    for f, want in c["rand_sites"].items():
        end = c["extent"].get(f, (None,))[0]
        if end is None:
            end = {0x8009E89C: 0x8009EF8C}.get(f)
        got = rand_sites(f, end)
        b.check("rand_sites", got == want, f"{f:#x} Rand sites {[hex(x) for x in got]}")
    for a, (w, what) in c["words"].items():
        b.check("words", word_at(a) == w, f"word at {a:#x} = {word_at(a):#010x}: {what}")
    ported = load_ported()
    for f in sorted(c["ported"]):
        b.check("ported", f in ported, f"{f:#x} has a bench row ({ported.get(f)})")
    for f in sorted(c["unported"]):
        b.check("unported", f not in ported, f"{f:#x} has no bench row ({ported.get(f)})")


# which models read which constant of claims()["k"] (a mutated constant re-runs only those)
K_READERS = {"radius": {0x80013110, 0x80039F68}, "acc": {0x8008CD88}, "fast": {0x8009CFF4}, "slow": {0x8009CFF4},
             "pct44": {0x8009CFF4}, "flip44": {0x8009CFF4}, "dup": {0x8009CFF4}, "sched_ahead": {0x8009CFF4},
             "sched_behind": {0x8009CFF4}, "rider_step": {0x80099D48}, "far_cop": {0x80039F68},
             "density_max": {0x8009F44C}, "density_road_max": {0x8009F44C}, "piece_list": {0x80039DFC}}
_BASE_MODELS = {}


def check_models(b, traces, c, verbose):
    base = claims()["k"]
    diff = {k for k in c["k"] if c["k"][k] != base[k]}
    if _BASE_MODELS and not diff:
        res, first = _BASE_MODELS["res"], _BASE_MODELS["first"]
    else:
        only = set().union(*(K_READERS[k] for k in diff)) if diff else None
        res, first = model_results(traces, c, only)
        if not diff:
            _BASE_MODELS.update(res=res, first=first)
    for f in MODELS:
        cnt = res[f]
        want = c["model_min_calls"].get(f, 1)
        ok = cnt["bad"] == 0 and cnt["ok"] >= want
        b.check("model", ok, f"{f:#x} {MODELS[f].__name__}: {cnt['ok']} calls match, {cnt['bad']} differ, "
                             f"{cnt['trunc']} truncated {first.get(f, '')}")
        b.check("model_min_calls", cnt["ok"] >= want, f"{f:#x}: at least {want} compared calls ({cnt['ok']})")
        if verbose:
            print(f"  {f:#010x} {MODELS[f].__name__:20s} {cnt['ok']:5d} calls, {cnt['bad']} differ  {first.get(f, '')}")
    return res


def model_results(traces, c, only=None):
    K["c"] = c
    res, first = {}, {}
    for f in MODELS:
        if only is not None and f not in only and _BASE_MODELS:
            res[f] = _BASE_MODELS["res"][f]
            if f in _BASE_MODELS["first"]:
                first[f] = _BASE_MODELS["first"][f]
            continue
        cnt = Counter()
        for name, tr in traces.items():
            for seq, ok, why in run_model(tr, f, c):
                cnt["trunc" if ok is None else ("ok" if ok else "bad")] += 1
                if ok is False and f not in first:
                    first[f] = f"{name} seq {seq}: {why}"
        res[f] = cnt
    return res, first


def check_facts(b, F, c):
    L = c["live"]
    rr = F["rr-race"]
    b.check("live", rr["cars_spawned"] == L["rr-race"]["cars_spawned"], f"rr-race: {rr['cars_spawned']} car(s) spawned")
    b.check("live", rr["placements"] == L["rr-race"]["placements"] and rr["retires"] == L["rr-race"]["retires"],
            f"rr-race: {rr['placements']} placements, {rr['retires']} retirements in 40 frames")
    b.check("live", len(rr["rounds"]) == 1 and rr["round_order"] == [0x8009B498, 0x8009B4A8],
            f"rr-race: one scheduler round (frame {rr['rounds']}), police before traffic {[hex(x) for x in rr['round_order']]}")
    nc = rr["new_cars"][0] if rr["new_cars"] else {}
    b.check("live", round(nc.get("ahead", 0), 3) == L["rr-race"]["spawn_ahead"] and nc.get("dir") == -nc.get("pdir", 0)
            and nc.get("live") == 1, f"rr-race: the new car {nc}")
    b.check("live", list(rr["rand_plain_frames"]) == [tuple([0x8009CF94] * L["rr-race"]["rand_per_car_frame"])],
            f"rr-race: every other frame draws {[[hex(x) for x in k] for k in rr['rand_plain_frames']]}")
    b.check("rand_order", [hex(x) for x in (rr["rand_round"][0] if rr["rand_round"] else [])] == L["rr-race"]["rand_round"],
            f"rr-race: the round frame draws {[hex(x) for x in (rr['rand_round'][0] if rr['rand_round'] else [])]}")
    dd = F["rr-race-dormant"]
    b.check("live", len(dd) == 14 and set(dd.values()) == {L["rr-race"]["dormant_writes"]},
            f"rr-race: the 14 dormant bikes' +0x170 / +0x39C writes by 0x8009765C / 0x80095810: {set(dd.values())}")
    for name in ("wake", "wake-behind"):
        w = F[name]
        b.check("live", w["woken"] == L[name]["woken"] and w["placements"] == 1 and w["retires"] == 0
                and w["placement_from"] == ["0x80093fcc"] and w.get("rider_live") == 1,
                f"{name}: woken {w['woken']}, placements {w['placement_from']}, rider live {w.get('rider_live')}, "
                f"rider woken by {w['rider_woken_by']}")
        b.check("live", w.get("refile") == L["wake"]["refile"],
                f"{name}: bit 27 set by the placement, then the rider pass re-files bike 15: "
                f"{[(f, hex(pc), hex(v)) for f, pc, v in w.get('refile', [])]}")
        b.check("live", w.get("off_edited_along") is not None and abs(w["off_edited_along"]) < 0x100,
                f"{name}: bike 15 placed {w.get('off_edited_along')}/65536 off its edited road coordinate, at player "
                f"{w.get('along_minus_player'):+}, dir {w.get('dir')}, "
                f"lateral {w.get('lateral')}")
    r = F["retire"]
    b.check("live", r["retired"] == L["retire"]["retired"] and r["retires"] == L["retire"]["retires"]
            and r["rider_retired_by"] == ["0x80094070"], f"retire: {r}")
    sc = F["sched"]
    b.check("live", sc["cop_released"] == L["sched"]["cop_released"] and sc["cop_slot"] == [16]
            and sc["cop_setup_from"] == ["0x8009eecc"] and sc["placement_from"] == ["0x80093fcc"],
            f"sched: cop released {sc['cop_released']} (slot {sc['cop_slot']}), setup from {sc['cop_setup_from']}, "
            f"placement from {sc['placement_from']}")
    b.check("live", round(sc.get("cop_ahead", 0), 1) == L["sched"]["cop_ahead"] and sc.get("cop_speed_39c") == L["sched"]["cop_speed"],
            f"sched: the cop {sc.get('cop_ahead')} ahead of the player, commanded speed {sc.get('cop_speed_39c')}, "
            f"+0x3A0 {sc.get('cop_3a0')}, top command {sc.get('cop_cmd_top')}")
    b.check("live", sc["cars_spawned"] == L["sched"]["cars_spawned"], f"sched: {sc['cars_spawned']} car(s) spawned")
    b.check("live", sc.get("cop_cmd_top") == 1 and sc.get("cop_stack_next_tick") == L["sched"]["cop_stack"],
            f"sched: the cop's top command after placement {sc.get('cop_cmd_top')}, its stack at the next tick "
            f"{sc.get('cop_stack_next_tick')}")
    b.check("rand_order", [hex(x) for x in sc["rand_frame1"]] == L["sched"]["rand_frame1"],
            f"sched: frame 1 draws {[hex(x) for x in sc['rand_frame1']]}")
    se = F["sched-empty"]
    b.check("live", se["cars_spawned"] == 1 and se["new_cars"] and se["new_cars"][0]["handle"] == 0x60
            and round(se["new_cars"][0]["ahead"], 3) == L["sched-empty"]["spawn_ahead"],
            f"sched-empty: {se['new_cars']}")
    b.check("rand_order", [hex(x) for x in se["rand_frame1"]] == L["sched-empty"]["rand_frame1"] and se["rand_later"] == [0],
            f"sched-empty: frame 1 draws {[hex(x) for x in se['rand_frame1']]}, later frames {se['rand_later']}")
    cf = F["car-far"]
    b.check("live", cf["car_despawned"] == L["car-far"]["car_despawned"] and cf["release_from"] == [(1, "0x8009aa98")]
            and cf["pool3_live_after"] == 0 and (1, "0x8009a320", 0) in cf["car_live_writes"], f"car-far: {cf}")
    rf = F["rider-far"]
    b.check("live", rf["rider_dormant"] == L["rider-far"]["rider_dormant"] and rf["dormant_by"] == ["0x80095284"]
            and rf["chase_calls"] >= L["rider-far"]["chase_calls_min"] and rf["woke_again"] == [],
            f"rider-far: {rf}")
    b.check("live", rf["chase_steps"] == L["rider-far"]["chase_steps"], f"rider-far: road steps {rf['chase_steps']}")
    rb = F["rider-back"]
    b.check("live", [(f, v) for f, pc, v in rb["rider6"]] == L["rider-back"]["rider6"]
            and rb["reentry_calls"] == ["0x800952e0"],
            f"rider-back: rider 6 +0x140 {rb['rider6']}, re-entry through {rb['reentry_calls']}")
    fr_ = F["far-road"]["piece_and_window"]
    b.check("live", fr_ == [([0], [0])], f"far-road: resident piece / window result for bike 15 in frame 1: {fr_}")
    ed, want = F["edges"], c["live"]["edges"]
    b.check("live", ed == want, f"edges (frame-1 +0x140 of the probed entity at an exact distance): {ed}")
    q = F["quick"]
    b.check("live", q["rand_calls"] == L["quick"]["rand_calls"] and q["sched_spawns"] == 0,
            f"quick: {q['rand_calls']} Rand calls in 4 frames, {q['sched_spawns']} spawns, rounds {q['rounds']}")


# ---------------------------------------------------------------------------------------------
# coverage of the models (which model lines no run executed)
# ---------------------------------------------------------------------------------------------

_COVER = set()


def _tracer(frame, event, arg):
    if frame.f_code.co_filename == __file__:
        if event == "line":
            _COVER.add(frame.f_lineno)
        return _tracer
    return None


def uncovered(fn):
    import inspect
    src, first = inspect.getsourcelines(fn)
    out = []
    for i, ln in enumerate(src):
        t = ln.strip()
        if not t or t.startswith(("#", "def ", '"""', "else:", "return None")) or t.endswith('"""'):
            continue
        if first + i not in _COVER:
            out.append(first + i)
    return out


# ---------------------------------------------------------------------------------------------
# one mutation per claim
# ---------------------------------------------------------------------------------------------

def _set(path, value):
    def f(c):
        d = c
        for k in path[:-1]:
            d = d[k]
        d[path[-1]] = value
    return f


MUTATIONS = [
    ("sha1", "a different RASHCDG image", _set(("sha1", "G"), "0" * 40)),
    ("extent", "the placement ends one word early", _set(("extent", 0x8009432C), (0x800950E4, 88))),
    ("extent", "0x80093ED4 keeps a 40-byte frame", _set(("extent", 0x80093ED4), (0x80093F94, 40))),
    ("callees", "0x80093F94 also calls 0x80093ED4",
     lambda c: c["callees"][0x80093F94].add(0x80093ED4)),
    ("callees", "the road-window test does not call the per-player predicate",
     lambda c: c["callees"][0x80039F68].discard(0x80013110)),
    ("callers", "the road-window test has one caller fewer",
     lambda c: c["callers"][0x80039F68].remove(0x800CBE80)),
    ("callers", "only the step calls the activation pass", _set(("callers", 0x80093E6C), [0x80078C58])),
    ("rand_sites", "the car spawner draws Rand a third time",
     lambda c: c["rand_sites"][0x8009AD48].append(0x8009B160)),
    ("rand_sites", "the placement draws Rand (its first call is FixMul)", _set(("rand_sites", 0x8009432C), [0x8009435C])),
    ("words", "the sticky radius for kinds >= 2 is 240",
     lambda c: c["words"].update({0x800131E0: (0x28620000 | 240, "mutated")})),
    ("words", "the traffic spacing guard is 128.0", lambda c: c["words"].update({0x8009D5C8: (0x3C020080, "mutated")})),
    ("ported", "the camera-target leaf SLUS 0x800235B0 has a bench row", lambda c: c["ported"].add(0x800235B0)),
    ("callees", "the cursor seat does not call TurnSubObject", lambda c: c["callees"][0x8003A700].discard(0x8003BE1C)),
    ("callers", "the cursor re-seat has a third caller", lambda c: c["callers"][0x8003A468].append(0x8009AEB4)),
    ("words", "SeatAlong divides by 64", lambda c: c["words"].update({0x800375EC: (0x3C040400, "mutated")})),
    ("model", "the resident piece list starts one entry later", _set(("k", "piece_list"), 0x800D4B20)),
    ("unported", "route_find_leg is not ported", lambda c: c["unported"].add(0x8003B4B0)),
    ("model", "a bike enters the window at 301", _set(("k", "radius", "bike_in"), 301)),
    ("model", "a live bike stays in the window up to 349", _set(("k", "radius", "bike_stay"), 349)),
    ("model", "a car enters the window below 190", _set(("k", "radius", "other_in"), 190)),
    ("model", "a live car stays in the window up to 229", _set(("k", "radius", "other_stay"), 229)),
    ("model", "the sticky test adds 0 instead of 20.0", _set(("k", "radius", "extra"), 0)),
    ("model", "the cell walker runs from 8192", _set(("k", "acc", "walker"), 0x2000)),
    ("model", "the schedulers run from 0xE000", _set(("k", "acc", "sched"), 0xE000)),
    ("model", "the slow-player threshold is 3.0", _set(("k", "slow"), 0x30000)),
    ("model", "the spacing guard is 181.0", _set(("k", "dup"), 0xB50000)),
    ("model", "cars are spawned 145.0 ahead", _set(("k", "sched_ahead"), 0x800D8730)),
    ("model", "a dormant rider chases its bike by 4.0", _set(("k", "rider_step"), 0x40000)),
    ("model", "the per-road table is clamped at road 8", _set(("k", "density_road_max"), 8)),
    ("model", "the cap is clamped to 10", _set(("k", "density_max"), 10)),
    ("model", "a cop is far from 1 (not 201)", _set(("k", "far_cop"), 1)),
    ("model_min_calls", "the car spawner ran three times", lambda c: c["model_min_calls"].update({0x8009AD48: 3})),
    ("live", "the natural run spawns two cars", _set(("live", "rr-race", "cars_spawned"), 2)),
    ("live", "the dormant driver skips a frame", _set(("live", "rr-race", "dormant_writes"), (38, 39))),
    ("live", "cars are spawned 181.0 ahead", _set(("live", "rr-race", "spawn_ahead"), 181.0)),
    ("live", "a car with lane 2 draws Rand twice a frame", _set(("live", "rr-race", "rand_per_car_frame"), 2)),
    ("live", "bike 14 wakes, not 15", _set(("live", "wake", "woken"), [14])),
    ("live", "the woken bike is re-filed onto the dormant list",
     _set(("live", "wake", "refile"), [(1, 0x80094FA0, 0x08000000), (1, 0x80071D04, 0x8005B270), (1, 0x80071D0C, 0)])),
    ("live", "the woken bike wakes somewhere other than its road coordinate (bike 14 instead)",
     _set(("live", "wake-behind", "woken"), [14])),
    ("live", "nothing retires", _set(("live", "retire", "retired"), [])),
    ("live", "the cop lands 180.0 ahead", _set(("live", "sched", "cop_ahead"), 180.0)),
    ("live", "the forced round spawns a car beside the parked one", _set(("live", "sched", "cars_spawned"), 1)),
    ("live", "the released cop ends on the placement's {1, 224}", _set(("live", "sched", "cop_stack"), [(1, 224)])),
    ("live", "the far car survives", _set(("live", "car-far", "car_despawned"), 0)),
    ("live", "the dormant rider steps 4.0", _set(("live", "rider-far", "chase_steps"), [4.0])),
    ("live", "quick draws Rand", _set(("live", "quick", "rand_calls"), 3)),
    ("live", "a dormant bike at 300 wakes", _set(("live", "edges", "edge-bike-300"), 1)),
    ("live", "a live car at 229 leaves", _set(("live", "edges", "edge-car-229"), 0)),
    ("live", "the downed rider stays dormant once its box is moved", _set(("live", "rider-back", "rider6"), [(1, 0)])),
    ("rand_order", "the car's lane is drawn before the scheduler's roll",
     _set(("live", "rr-race", "rand_round"), ["0x8009b0d4", "0x8009d2d4", "0x8002fc94", "0x80030224",
                                                "0x8009cf94", "0x8009cf94", "0x8009cf94"])),
    ("rand_order", "the traffic roll comes before the cop draws",
     _set(("live", "sched", "rand_frame1"), ["0x8009d2d4", "0x800942d4", "0x800942d4",
                                               "0x8009cf94", "0x8009cf94", "0x8009cf94"])),
]


# ---------------------------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------------------------

def run_all(c, verbose, live=True):
    b = Bench(verbose)
    check_skeleton(b, c)
    if live:
        if not os.path.exists(RRVERIFY):
            b.check("model", False, f"{RRVERIFY} missing - the live comparison cannot run")
            return b.res
        traces = all_traces(c)
        check_models(b, traces, c, verbose)
        check_facts(b, facts(traces), c)
    return b.res


def header(c):
    im = images()
    print(f"SLUS_010.53 sha1 {im['SLUS'].sha1}   RASHCDG.BIN sha1 {im['G'].sha1}")


def cmd_live(a):
    c = claims()
    header(c)
    traces = {}
    for name, src, fr in RUNS:
        tr, stop, edits = run_trace(name, src, fr, c)
        traces[name] = tr
        print(f"  run {name}: {len(edits)} data edits of {src}; {'; '.join(stop)}")
    if a.cover:
        sys.settrace(_tracer)
    b = Bench(True)
    check_models(b, traces, c, True)
    sys.settrace(None)
    if a.cover:
        for f, fn in MODELS.items():
            u = uncovered(fn)
            print(f"  cover {fn.__name__:20s} {len(u)} model lines never executed: {u[:40]}")
    F = facts(traces)
    for name, d in F.items():
        print(f"  facts {name}:")
        for k, v in d.items():
            if isinstance(v, list) and v and isinstance(v[0], int):
                v = [hex(x) for x in v]
            print(f"      {k}: {v}")
    check_facts(b, F, c)
    fails = sum(1 for _, ok, _ in b.res if not ok)
    print(f"spawn live: {len(b.res)} checks, {fails} failures")


def cmd_rand(_a):
    c = claims()
    header(c)
    for name, src, fr in RUNS:
        tr, _, _ = run_trace(name, src, fr, c)
        cen = rand_census(tr)
        print(f"  {name} ({len(ticks(tr))} RaceTicks):")
        agg = defaultdict(list)
        for f in sorted(cen):
            agg[tuple(cen[f])].append(f)
        for k, fs in agg.items():
            print(f"      frames {fs[:6]}{' ...' if len(fs) > 6 else ''} ({len(fs)}): {' '.join(hex(x) for x in k)}")
        if not cen:
            print("      no Rand call")


def cmd_info(_a):
    c = claims()
    header(c)
    res = run_all(c, True, live=False)
    ported = load_ported()
    for f, (end, frame) in sorted(c["extent"].items()):
        cs = sorted(callees(f, end))
        print(f"  {f:#010x} {(end - f) // 4:4d} insns frame {frame:3d}  callers {len(all_call_sites(f)):2d}  "
              f"callees " + " ".join(f"{x:#x}{'*' if x in ported else ''}" for x in cs))
    fails = sum(1 for _, ok, _ in res if not ok)
    print(f"spawn info: {len(res)} checks, {fails} failures   (* = has a bench row)")


def cmd_verify(a):
    base_c = claims()
    header(base_c)
    res = run_all(base_c, True)
    fails = sum(1 for _, ok, _ in res if not ok)
    if not a.mutate:
        print(f"spawn: {len(res)} checks, {fails} failures")
        return 1 if fails else 0
    if fails:
        print(f"spawn: the unmutated claims already fail ({fails}); --mutate is meaningless")
        return 1
    detected = ran = 0
    for claim, what, fn in MUTATIONS:
        c = copy.deepcopy(claims())
        fn(c)
        r = run_all(c, False)
        guarded = [x for x in r if x[0] == claim]
        if not guarded:
            print(f"  n/a       {claim:15s} {what}")
            continue
        ran += 1
        caught = [m for cl, ok, m in r if not ok and cl == claim]
        if caught:
            detected += 1
            print(f"  DETECTED  {claim:15s} {what}  <- {caught[0][:150]}")
        else:
            print(f"  MISSED    {claim:15s} {what}")
    K["c"] = claims()
    print(f"spawn: {ran} mutations, {detected} detected")
    return 0 if detected == ran and ran > 0 else 1


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["info", "live", "rand", "verify"])
    ap.add_argument("--mutate", action="store_true")
    ap.add_argument("--cover", action="store_true", help="live: report the model lines no run executed")
    a = ap.parse_args(argv)
    return {"info": cmd_info, "live": cmd_live, "rand": cmd_rand, "verify": cmd_verify}[a.cmd](a) or 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
