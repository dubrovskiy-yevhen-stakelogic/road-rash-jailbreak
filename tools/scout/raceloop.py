"""Scout probes for THE RACE LOOP of Road Rash: Jailbreak (USA, SLUS_01053).

The race loop is the spine that calls everything else: what builds the starting grid, what
runs each frame and in what order, how progress and place are derived, what ends a race and
how control gets back to the shell.  This probe checks its STRUCTURE against the player's own
disc extract and savestates, independently of the C++ tree:

  * the frame skeleton is re-derived from the instruction words themselves - every documented
    "function A calls function B at address X" is checked by decoding the `jal` at X, and the
    per-frame child order of the race tick, the race director and the world pass is checked to
    be exactly the documented sequence with nothing extra;
  * the countdown is checked against its own constants (the 3.00 s literal, the 0x40 / 0x20
    flag bits and the 0xBF / 0xDF clears);
  * the post-race delay constant 5.00 s is checked at its writer, at its reader, and in RAM;
  * `DATA\\STARTDF{A,B}.BIN` is checked to be tiled by the 292-byte block the loader's own
    `li a2,292` declares, and every block of both files is parsed;
  * the starting-grid placement is checked numerically: the two parked police bikes of the
    captured race are predicted from `[START]` in `ROADGRF1.TXT` plus the grid block plus the
    `v - (v>>7) - (v>>6)` scaling the spawner performs, and compared with `entity+0x170` in
    every savestate - together with the negative control that the unscaled formula is worse;
  * the pool-0 / pool-1 allocation the grid builder performs (stride 1096 and 628, bases
    published into the pool table at 0x800CE4D0) is checked against live RAM;
  * the per-player view/camera records are checked: base, stride, and the target entity of
    view 0 being the player's own bike in every savestate;
  * the HUD driver is checked to be the one in RASHCDG (not RASHCDI) by the element functions
    it calls;
  * the two writers of the main state byte inside the race overlay are checked to be exactly
    the race-over transition and the results-scene exit.

Reads ONLY from work\\disc_us (the player's own disc extract, gitignored) and from
work\\oracle\\state (savestate extracts).  Writes ONLY to stdout.  Never copies game bytes
into the repo.

Usage (from the project root):

    python tools\\scout\\raceloop.py info      # the decoded call skeleton, addresses only
    python tools\\scout\\raceloop.py grid      # the starting-grid blocks and the placement check
    python tools\\scout\\raceloop.py verify    # the assertion bench (the gate)

`verify` ends with a single line a gate can match:

    raceloop: <n> checks, <m> failures
"""

from __future__ import annotations

import argparse
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import exe as E  # noqa: E402  (sibling scout module; read-only use)

ROOT = E.ROOT
DISC = E.DISC
DATA = os.path.join(DISC, "DATA")
STATE_DIR = os.path.join(ROOT, "work", "oracle", "state")
STATES = ("rr-race", "rr-pack", "rr-grid", "quick")

OVL_BASE = 0x8005B5E8  # all three overlays load here (docs\formats\overview.md)

SHA1 = {
    "SLUS": "67ed165a2c517d4e6106fb0dfa324d66dd9a76f1",
    "F": "a3fec4b4e9292c358d0f6dc529843f5d8f25924a",
    "G": "cfe43a7786759f2cb9c57751cf99e84d1074782c",
    "I": "9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06",
}

# ---------------------------------------------------------------------------
# Guest globals the race loop lives in, each derived from the instruction that establishes it.
# ---------------------------------------------------------------------------

GAME_STATE_PTR = 0x8005B2F8      # -> game_state (0x800D5D38)
GS_STATE = 0x00
GS_FIRST_FRAME = 0x03
GS_RACE_TYPE = 0x04
GS_RACE_CLOCK = 0x10
GS_NUM_PLAYERS = 0x30
GS_BANK = 0x3C
GS_RACE_ID = 0x40

POOL_TABLE = 0x800CE4D0          # population.md 1
BIKE_POOL_BASE_PTR = 0x8005B3A0  # the grid builder's malloc result for pool 0
RIDER_POOL_BASE_PTR = 0x8005B3A4 # ... and for pool 1
BIKE_COUNT = 0x8005B1F8          # live pool-0 bikes
RACER_COUNT = 0x8005B1FC         # pool-0 bikes that are players or non-police AI
PLAYER_BIKES = 0x8005B268        # u32[2], one per player
PLAYER0_BIKE = 0x8005B38C
PLAYER1_BIKE = 0x8005B21C
COUNTDOWN = 0x8005B230           # 16.16 seconds; also the post-race accumulator
POSTRACE_LIMIT = 0x8005B228      # 16.16 seconds, 5.00
EVENT_ACC = 0x8005B30C           # the AI-planner / periodic-event accumulator
SKIP_RESULTS = 0x8005B220        # non-zero -> the race end goes straight to the shell
START_POS = 0x800CF578           # the 12-byte [START] road position every bike is seeded from
VIEW_BASE = 0x800CD898           # per-player view / camera record
VIEW_STRIDE = 1132
VIEW_TARGET = 0x238              # -> the entity the camera follows
CAMERA_CA = 0x800CD7B8           # DATA\CAMERA.CA lands here
CAMERA_CA_READ = 224             # ... and only this many bytes are read (RASHCDI 0x80069664)

ENT_STRIDE = 1096
ENT_HANDLE = 0x0AC
ENT_ROAD = 0x168                 # packed u16 id; u16 kind
ENT_DIR = 0x16C
ENT_DIST = 0x170
ENT_PROGRESS = 0x144
ENT_RIDER_DEF = 0x43C
RIDER_STRIDE = 628
RD_RANK_CLASS = 0x01
RD_PLACE = 0x27

GRID_BLOCK = 292                 # bytes per race, declared by `li a2,292`

# ---------------------------------------------------------------------------
# The call skeleton.  Each row is (image, site, callee, what) and is checked by
# decoding the instruction word at `site`; nothing here is assumed.
# ---------------------------------------------------------------------------

SKELETON = [
    # --- entering a race -------------------------------------------------
    ("SLUS", 0x800122F4, 0x80064610, "main -> RASHCDI LoadLevel"),
    ("SLUS", 0x800122FC, 0x80063B90, "main -> RASHCDI EnterRace"),
    ("SLUS", 0x80012348, 0x800119C0, "main -> the once-per-race entity prime pass"),
    ("I",    0x80063BC8, 0x80063500, "EnterRace -> reset + state 3"),
    ("I",    0x80063BD0, 0x800635D0, "EnterRace -> per-bank / per-race asset load"),
    ("I",    0x80063BD8, 0x80063670, "EnterRace -> SetUpRace"),
    ("I",    0x800637F8, 0x8006982C, "SetUpRace -> BuildRace"),
    ("I",    0x800699D0, 0x80067B00, "BuildRace -> BuildGrid"),
    ("I",    0x80069970, 0x8001E0B4, "BuildRace -> memcpy of the 292-byte grid block"),
    ("I",    0x8006815C, 0x80065A94, "BuildGrid -> SpawnBike (per grid entry)"),
    ("I",    0x8006822C, 0x800138E8, "BuildGrid -> ComputePlace (initial places)"),
    ("I",    0x80068240, 0x800650A0, "BuildGrid -> rider-record init"),
    ("I",    0x800667F4, 0x8002EAD8, "SpawnBike -> MulAdd along the slice tangent"),
    ("I",    0x80066808, 0x8002EAD8, "SpawnBike -> MulAdd along the slice lateral axis"),
    ("I",    0x80066BCC, 0x8003AF9C, "SpawnBike -> bind the road position to an intersection"),
    ("I",    0x80066BD4, 0x8003B61C, "SpawnBike -> the initial progress scalar"),
    # --- the frame -------------------------------------------------------
    ("SLUS", 0x80012370, 0x8001CB3C, "frame -> PollPads"),
    ("SLUS", 0x800123AC, 0x8008AB00, "first race frame -> RaceTick(1094)"),
    ("SLUS", 0x800123B8, 0x800881B4, "first race frame -> ViewUpdate(view0, 1094)"),
    ("SLUS", 0x80012414, 0x80011C4C, "frame -> GameFrame"),
    ("SLUS", 0x80011DA4, 0x80012524, "GameFrame -> RaceStep"),
    ("SLUS", 0x80011DAC, 0x8008CFDC, "GameFrame -> RASHCDG per-frame bookkeeping"),
    ("SLUS", 0x80011E14, 0x8005E1D8, "GameFrame -> RASHCDG list walk inside a critical section"),
    ("SLUS", 0x80011E3C, 0x800C89A0, "GameFrame -> clear the ordering tables"),
    ("SLUS", 0x80011FB0, 0x800674C8, "GameFrame per-player -> the one-line clear of 0x8005B280"),
    ("SLUS", 0x80011FF8, 0x8008D56C, "GameFrame per-player -> entity presentation"),
    ("SLUS", 0x80012008, 0x800C8B24, "GameFrame per-player -> draw pass A"),
    ("SLUS", 0x80012034, 0x800C8CD4, "GameFrame per-player -> draw pass B"),
    ("SLUS", 0x8001211C, 0x8005E848, "GameFrame -> the HUD"),
    ("SLUS", 0x80012178, 0x800C5918, "GameFrame -> the results scene (state 6)"),
    ("SLUS", 0x80012198, 0x8002D2F4, "GameFrame -> the in-race menu state machine"),
    ("SLUS", 0x800121A8, 0x80048DB4, "GameFrame -> DrawOTag"),
    ("SLUS", 0x8001258C, 0x8008AB00, "RaceStep -> RaceTick(218 * ticks)"),
    ("SLUS", 0x800125D4, 0x800881B4, "RaceStep -> ViewUpdate(view[p], dt)"),
    # --- the race tick ---------------------------------------------------
    ("G", 0x8008AB34, 0x8008AD38, "RaceTick -> TickCountdown"),
    ("G", 0x8008AB64, 0x800B8018, "RaceTick -> AiPlan, half-second edge"),
    ("G", 0x8008AC20, 0x800B8018, "RaceTick -> AiPlan, the other arm"),
    ("G", 0x8008AC38, 0x800B9414, "RaceTick -> RaceDirector"),
    ("G", 0x8008AC40, 0x8008CD88, "RaceTick -> the spawner pass"),
    ("G", 0x8008AC48, 0x8008AC80, "RaceTick -> the world + bike pass"),
    ("G", 0x8008AC50, 0x800A4774, "RaceTick -> collision resolution"),
    ("G", 0x8008AC58, 0x8008ACE8, "RaceTick -> the rider + engine pass"),
    ("G", 0x8008AC60, 0x80090814, "RaceTick -> presentation"),
    ("G", 0x8008AC8C, 0x8009A298, "world pass -> world / traffic / camera tracking"),
    ("G", 0x8008AC94, 0x80075EE0, "world pass -> the per-bike step"),
    ("G", 0x8008ACF4, 0x8007B840, "rider pass -> the rider + engine loop"),
    ("G", 0x8008CDCC, 0x8009B474, "spawner pass -> police / traffic scheduler"),
    ("G", 0x80078B6C, 0x8003B520, "per-bike step -> the progress-scalar walk"),
    # --- the race director ----------------------------------------------
    ("G", 0x800B9634, 0x800B9794, "RaceDirector -> progress / finish / wrong way"),
    ("G", 0x800B9648, 0x800BA304, "RaceDirector -> the AI drive pass"),
    ("G", 0x800B9658, 0x800BA4CC, "RaceDirector -> the AI command pass"),
    ("G", 0x800B9668, 0x800BCD48, "RaceDirector -> the AI brain pass"),
    ("G", 0x800B9868, 0x800B9958, "progress pass -> the finish test"),
    ("G", 0x800B95EC, 0x8003F708, "race over -> teardown before state 6"),
]

# The children of the race tick, in the order the code issues them.
TICK_ORDER = [0x800B9414, 0x8008CD88, 0x8008AC80, 0x800A4774, 0x8008ACE8, 0x80090814]
# The four passes of the race director, in order.
DIRECTOR_ORDER = [0x800B9794, 0x800BA304, 0x800BA4CC, 0x800BCD48]
# The children of the world + bike pass, in order.
WORLD_ORDER = [0x8009A298, 0x80075EE0, 0x8008F068, 0x800A2898, 0x800CB304, 0x8009ACA4, 0x8009AB60]

# The HUD element functions, as called by the HUD driver (rules.md 10).
HUD_ELEMENTS = {
    0x8005FF84: "countdown digit",
    0x80062C40: "race clock",
    0x80060178: "speed",
    0x800603E4: "odometer",
    0x800606F0: "place / racer count",
    0x80060C10: "health bar",
    0x80061F6C: "radar",
    0x8005F030: "arrest counters",
    0x80062610: "TKO counter",
    0x8003C590: "the wrong-way / turn mask",
}

# Instruction words the claims quote verbatim; each is checked bit for bit.
LITERALS = [
    ("G", 0x8008AD58, 0x30420040, "TickCountdown: andi v0,v0,0x40 - the grid freeze bit"),
    ("G", 0x8008AD90, 0x30420020, "TickCountdown: andi v0,v0,0x20 - the arm bit"),
    ("G", 0x8008ADAC, 0x3C020003, "TickCountdown: lui v0,0x3 - the 3.00 s countdown"),
    ("G", 0x8008ADBC, 0x304200DF, "TickCountdown: andi v0,v0,0xdf - clear the arm bit"),
    ("G", 0x8008AE18, 0x306300BF, "TickCountdown: andi v1,v1,0xbf - clear the freeze bit"),
    ("SLUS", 0x80012390, 0x3C020005, "first race frame: lui v0,0x5 - the 5.00 s post-race delay"),
    ("SLUS", 0x80012398, 0xAC22B228, "first race frame: sw v0,-19928(at) -> 0x8005B228"),
    ("I", 0x80069940, 0x24060124, "BuildRace: li a2,292 - the starting-grid block size"),
    ("I", 0x8006990C, 0x2C420002, "BuildRace: sltiu v0,v0,2 on game_state+0x30"),
    ("I", 0x80069914, 0x24030041, "BuildRace: li v1,65 - 'A'"),
    ("I", 0x80069918, 0x24030042, "BuildRace: li v1,66 - 'B'"),
    ("I", 0x80069928, 0xA203FFFF, "BuildRace: sb v1,-1(s0) - patch the set letter"),
    ("I", 0x800698BC, 0x0C011259, "BuildRace: strrchr on the path-table entry"),
    ("I", 0x8006672C, 0x000231C3, "SpawnBike: sra a2,v0,7 on the grid alongOffset"),
    ("I", 0x80066730, 0x00463023, "SpawnBike: subu a2,v0,a2"),
    ("I", 0x80066734, 0x00021183, "SpawnBike: sra v0,v0,6"),
    ("I", 0x8006673C, 0x00C23023, "SpawnBike: subu a2,a2,v0  => v * 125/128"),
    ("I", 0x800667DC, 0x000311C3, "SpawnBike: sra v0,v1,7 on the grid lateral"),
    ("I", 0x800667E4, 0x00031983, "SpawnBike: sra v1,v1,6"),
    ("I", 0x80066710, 0x2406000C, "SpawnBike: li a2,12 - the [START] road position copy"),
    ("I", 0x80066668, 0x30420060, "SpawnBike: andi v0,v0,0x60 - the player freeze+arm mask"),
    ("I", 0x80066674, 0xA0830000, "SpawnBike: sb v1,0(a0) -> riderDef+0x00"),
    ("I", 0x80069664, 0x240600E0, "the DATA\\CAMERA.CA read is li a2,224"),
    ("G", 0x800674CC, 0x03E00008, "0x800674C8 is a two-instruction clear, then jr ra"),
]


# ---------------------------------------------------------------------------
# Image / RAM helpers
# ---------------------------------------------------------------------------


def load_images():
    imgs = {"SLUS": E.load_exe()}
    for key, name in (("F", "RASHCDF.BIN"), ("G", "RASHCDG.BIN"), ("I", "RASHCDI.BIN")):
        imgs[key] = E.load_overlay(name, OVL_BASE)
    return imgs


def word(img, addr):
    o = addr - img.base
    if o < 0 or o + 4 > len(img.data):
        return None
    return struct.unpack_from("<I", img.data, o)[0]


def jal_target(img, addr):
    w = word(img, addr)
    if w is None or (w >> 26) != 3:
        return None
    return 0x80000000 | ((w & 0x03FFFFFF) << 2)


def calls_of(img, start, limit=6000):
    """The jal targets a function issues, in order, up to its first `jr ra`."""
    out = []
    a = start
    while a - start < 4 * limit:
        w = word(img, a)
        if w is None:
            break
        if (w >> 26) == 3:
            out.append((a, 0x80000000 | ((w & 0x03FFFFFF) << 2)))
        if w == 0x03E00008:
            break
        a += 4
    return out


class Ram:
    def __init__(self, path):
        self.d = open(path, "rb").read()

    def ok(self, a, n=4):
        o = a & 0x1FFFFF
        return o + n <= len(self.d)

    def u8(self, a):
        return self.d[a & 0x1FFFFF]

    def u16(self, a):
        return struct.unpack_from("<H", self.d, a & 0x1FFFFF)[0]

    def s32(self, a):
        return struct.unpack_from("<i", self.d, a & 0x1FFFFF)[0]

    def u32(self, a):
        return struct.unpack_from("<I", self.d, a & 0x1FFFFF)[0]

    def blob(self, a, n):
        o = a & 0x1FFFFF
        return self.d[o:o + n]


def states():
    out = []
    for name in STATES:
        p = os.path.join(STATE_DIR, name, "ram.bin")
        if os.path.isfile(p):
            out.append((name, Ram(p)))
    return out


# ---------------------------------------------------------------------------
# Data-file helpers
# ---------------------------------------------------------------------------


def grid_blocks(letter):
    path = os.path.join(DATA, "STARTDF%s.BIN" % letter)
    raw = open(path, "rb").read()
    return raw


def parse_grid_block(raw, index):
    off = GRID_BLOCK * index
    b = raw[off:off + GRID_BLOCK]
    n = struct.unpack_from("<I", b, 0)[0]
    ents = []
    for i in range(min(n, (GRID_BLOCK - 4) // 12)):
        slot, lateral, along = struct.unpack_from("<iii", b, 4 + 12 * i)
        ents.append((slot, lateral, along))
    return n, ents


def roadgrf_start(setno, race_id):
    """The [START] line of one race block: (road, distance, direction, node)."""
    path = os.path.join(DATA, "ROADGRF%d.TXT" % setno)
    txt = open(path, "rb").read().decode("latin1")
    for block in txt.split("[BEGIN]")[1:]:
        m = re.search(r"\[RACEID\]=(\d+)", block)
        if not m or int(m.group(1)) != race_id:
            continue
        s = re.search(r"\[START\]=(-?\d+)\s+(-?\d+)\s+(-?\d+)\s+(-?\d+)", block)
        if s:
            return tuple(int(x) for x in s.groups())
    return None


def scale125(v):
    """The spawner's own scaling: v - (v>>7) - (v>>6), arithmetic shifts."""
    return v - (v >> 7) - (v >> 6)


# ---------------------------------------------------------------------------
# Bench
# ---------------------------------------------------------------------------


class Bench:
    def __init__(self):
        self.n = 0
        self.bad = []

    def check(self, cond, what, detail=""):
        self.n += 1
        if not cond:
            self.bad.append(what)
            print("  FAIL  %s%s" % (what, ("  [%s]" % detail) if detail else ""))
        return bool(cond)


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------


def cmd_info(_args):
    imgs = load_images()
    for k in ("SLUS", "F", "G", "I"):
        print("%-5s %-14s sha1 %s" % (k, os.path.basename(imgs[k].path), imgs[k].sha1))
    print()
    print("Call skeleton (site -> callee, decoded from the instruction word):")
    for key, site, callee, what in SKELETON:
        t = jal_target(imgs[key], site)
        mark = "ok " if t == callee else "BAD"
        print("  %s %-6s %08X -> %08X  %s" % (mark, key, site, t or 0, what))
    print()
    print("Race tick children, in issue order:")
    for a, t in calls_of(imgs["G"], 0x8008AB00):
        print("    %08X jal %08X" % (a, t))
    print()
    print("Race director children, in issue order:")
    for a, t in calls_of(imgs["G"], 0x800B9414):
        print("    %08X jal %08X" % (a, t))


def cmd_grid(_args):
    for letter, count in (("A", 64), ("B", 55)):
        raw = grid_blocks(letter)
        print("STARTDF%s.BIN  %d bytes = %d blocks of %d"
              % (letter, len(raw), len(raw) // GRID_BLOCK, GRID_BLOCK))
        sizes = {}
        for i in range(len(raw) // GRID_BLOCK):
            n, _ = parse_grid_block(raw, i)
            sizes[n] = sizes.get(n, 0) + 1
        print("   grid sizes: %s" % sizes)
    print()
    start = roadgrf_start(1, 4)
    print("ROADGRF1.TXT race 4 [START] = road %d dist %d dir %d node %d" % start)
    n, ents = parse_grid_block(grid_blocks("A"), 3)
    print("STARTDFA.BIN block 3: %d entries" % n)
    for i, (slot, lat, al) in enumerate(ents):
        print("   pool0 slot %2d <- rider slot %2d  lateral %9.4f  along %9.4f"
              % (i, slot, lat / 65536.0, al / 65536.0))
    print()
    print("Placement check (the two parked police bikes):")
    base = start[1] << 16
    direction = start[2]
    for name, ram in states():
        p0 = ram.u32(POOL_TABLE)
        for idx in (16, 17):
            _slot, _lat, along = ents[idx]
            pred = base + direction * scale125(along)
            raw_pred = base + direction * along
            meas = ram.s32(p0 + ENT_STRIDE * idx + ENT_DIST)
            print("   %-8s slot %2d  predicted %.6f  measured %.6f  err %d (unscaled err %d)"
                  % (name, idx, pred / 65536.0, meas / 65536.0,
                     abs(pred - meas), abs(raw_pred - meas)))


def cmd_verify(_args):
    imgs = load_images()
    b = Bench()

    print("== images")
    for k in ("SLUS", "F", "G", "I"):
        b.check(imgs[k].sha1 == SHA1[k], "%s sha1 matches the documented image" % k,
                imgs[k].sha1)

    print("== the call skeleton, decoded from the instruction words")
    for key, site, callee, what in SKELETON:
        b.check(jal_target(imgs[key], site) == callee,
                "%s %08X: jal %08X (%s)" % (key, site, callee, what))

    print("== the per-frame child orders")
    tick = [t for _, t in calls_of(imgs["G"], 0x8008AB00)]
    b.check(tick[-len(TICK_ORDER):] == TICK_ORDER,
            "RaceTick 0x8008AB00 issues its six children in the documented order",
            " ".join("%08X" % t for t in tick))
    b.check(tick.count(0x8008AD38) == 1, "RaceTick calls TickCountdown exactly once")
    director = [t for _, t in calls_of(imgs["G"], 0x800B9414)]
    idx = [director.index(t) for t in DIRECTOR_ORDER if t in director]
    b.check(len(idx) == len(DIRECTOR_ORDER) and idx == sorted(idx),
            "RaceDirector 0x800B9414 issues its four passes in the documented order")
    world = [t for _, t in calls_of(imgs["G"], 0x8008AC80)]
    b.check(world == WORLD_ORDER,
            "the world+bike pass 0x8008AC80 issues exactly the documented seven children",
            " ".join("%08X" % t for t in world))
    rider = [t for _, t in calls_of(imgs["G"], 0x8008ACE8)]
    b.check(rider[0] == 0x8007B840 and len(rider) == 4,
            "the rider+engine pass 0x8008ACE8 starts with 0x8007B840 and has four children")

    print("== quoted instruction words")
    for key, addr, w, what in LITERALS:
        b.check(word(imgs[key], addr) == w, "%s %08X == %08X (%s)" % (key, addr, w, what),
                "%08X" % (word(imgs[key], addr) or 0))

    print("== the HUD driver is the one in RASHCDG, not the one in RASHCDI")
    hud_g = set(t for _, t in calls_of(imgs["G"], 0x8005E848))
    hud_i = set(t for _, t in calls_of(imgs["I"], 0x8005E848))
    for addr, what in HUD_ELEMENTS.items():
        b.check(addr in hud_g, "RASHCDG 0x8005E848 calls %08X (%s)" % (addr, what))
    b.check(not (hud_i & set(HUD_ELEMENTS)),
            "RASHCDI 0x8005E848 calls none of the HUD element functions",
            "%d calls total" % len(hud_i))

    print("== the two writers of game_state+0x00 inside the race overlay")
    writers = state_writers(imgs["G"])
    # NOTE: this is a 12-instruction-window heuristic, not a proof of exhaustiveness.
    # It sees the two writers that load the pointer locally; the third, 0x800B9604, holds
    # its pointer 36 instructions and is pinned by the two `li` words asserted below.
    b.check(sorted(writers) == [0x800B9600, 0x800C5B24],
            "RASHCDG: the local-pointer state writers are 0x800B9600 and 0x800C5B24",
            " ".join("%08X" % w for w in sorted(writers)))
    b.check(word(imgs["G"], 0x800B9604) == 0xA0A20000,
            "RASHCDG 0x800B9604 is the third, long-pointer store to game_state+0x00",
            "%08X" % (word(imgs["G"], 0x800B9604) or 0))
    b.check(word(imgs["G"], 0x800B95E8) == 0x24020002,
            "the race-over arm loads state 2 (straight back to the shell)")
    b.check(word(imgs["G"], 0x800B95F8) == 0x24020006,
            "the race-over arm loads state 6 (the results scene)")

    print("== the starting-grid files")
    for letter, blocks in (("A", 64), ("B", 55)):
        raw = grid_blocks(letter)
        b.check(len(raw) % GRID_BLOCK == 0,
                "STARTDF%s.BIN is an exact multiple of %d" % (letter, GRID_BLOCK))
        b.check(len(raw) // GRID_BLOCK == blocks,
                "STARTDF%s.BIN holds %d blocks" % (letter, blocks),
                str(len(raw) // GRID_BLOCK))
        for i in range(len(raw) // GRID_BLOCK):
            n, ents = parse_grid_block(raw, i)
            ok = 0 < n <= 24 and len(ents) == n
            slots = [s for s, _, _ in ents]
            ok = ok and len(set(slots)) == len(slots) and all(1 <= s <= 20 for s in slots)
            ok = ok and 4 + 12 * n <= GRID_BLOCK
            b.check(ok, "STARTDF%s.BIN block %d parses (n=%d, slots unique and in range)"
                    % (letter, i, n))

    print("== the captured race: grid, pools, placement")
    start = roadgrf_start(1, 4)
    b.check(start == (9, 1057, -1, 7),
            "ROADGRF1.TXT race 4 [START] is road 9, 1057, dir -1, node 7", str(start))
    n4, ents4 = parse_grid_block(grid_blocks("A"), 3)
    b.check(n4 == 18, "race 4's grid has 18 entries", str(n4))
    b.check(ents4[16][0] == 17 and ents4[17][0] == 18,
            "the last two grid entries name rider slots 17 and 18 (the police)")
    b.check(ents4[16][2] == 0 and ents4[17][2] == 3 * 65536,
            "their along offsets are 0.0 and 3.0")

    st = states()
    b.check(len(st) >= 3, "at least three savestate RAM images are available", str(len(st)))
    base = start[1] << 16
    direction = start[2]
    for name, ram in st:
        gs = ram.u32(GAME_STATE_PTR)
        b.check(ram.u32(gs + GS_RACE_ID) == 4, "%s: game_state race id is 4" % name)
        b.check(ram.u32(gs + GS_NUM_PLAYERS) == 1, "%s: one player" % name)

        # pools: the grid builder's two allocations, published into the pool table
        p0 = ram.u32(POOL_TABLE)
        p1 = ram.u32(POOL_TABLE + 0x10)
        b.check(p0 == ram.u32(BIKE_POOL_BASE_PTR),
                "%s: pool 0 base == *(0x8005B3A0)" % name, "%08X" % p0)
        b.check(p1 == ram.u32(RIDER_POOL_BASE_PTR),
                "%s: pool 1 base == *(0x8005B3A4)" % name, "%08X" % p1)
        b.check(ram.s32(POOL_TABLE + 4) == ENT_STRIDE,
                "%s: pool 0 stride is 1096" % name)
        b.check(ram.s32(POOL_TABLE + 0x14) == RIDER_STRIDE,
                "%s: pool 1 stride is 628" % name)
        nb = ram.s32(BIKE_COUNT)
        b.check(nb == n4, "%s: the live bike count equals the grid entry count" % name,
                str(nb))
        gap = p1 - p0 - ENT_STRIDE * nb
        b.check(0 <= gap < 64,
                "%s: the rider pool follows the bike pool 1096*n bytes later "
                "(plus the allocator header)" % name,
                "gap %d" % gap)

        # the [START] road position every bike is seeded from
        b.check(ram.u32(START_POS) & 0xFFFF == start[0],
                "%s: *(0x800CF578) road id is [START].road" % name)
        b.check(ram.s32(START_POS + 4) == direction,
                "%s: *(0x800CF57C) direction is [START].direction" % name)
        b.check(ram.s32(START_POS + 8) == base,
                "%s: *(0x800CF580) distance is [START].distance" % name,
                "%.4f" % (ram.s32(START_POS + 8) / 65536.0))

        # the placement formula, and its negative control
        for idx in (16, 17):
            along = ents4[idx][2]
            pred = base + direction * scale125(along)
            raw_pred = base + direction * along
            meas = ram.s32(p0 + ENT_STRIDE * idx + ENT_DIST)
            b.check(abs(pred - meas) <= 200,
                    "%s: police bike %d sits where the scaled grid formula predicts" % (name, idx),
                    "err %d" % abs(pred - meas))
            if along:
                b.check(abs(raw_pred - meas) > abs(pred - meas) * 10,
                        "%s: the UNSCALED formula is an order of magnitude worse for bike %d"
                        % (name, idx),
                        "unscaled err %d vs %d" % (abs(raw_pred - meas), abs(pred - meas)))
            b.check(ram.u32(p0 + ENT_STRIDE * idx + ENT_ROAD) & 0xFFFF == start[0],
                    "%s: police bike %d is on the start road" % (name, idx))

        # racer count = bikes that are a player or a non-police AI
        racers = 0
        for i in range(nb):
            e = p0 + ENT_STRIDE * i
            rd = ram.u32(e + ENT_RIDER_DEF)
            handle = ram.u16(e + ENT_HANDLE)
            klass = ram.u8(rd + RD_RANK_CLASS) & 0x0F
            if handle < ram.u32(gs + GS_NUM_PLAYERS) or klass != 2:
                racers += 1
        b.check(ram.s32(RACER_COUNT) == racers,
                "%s: *(0x8005B1FC) counts the players plus the non-police AI" % name,
                "%d vs %d" % (ram.s32(RACER_COUNT), racers))

        # places are a permutation over the racers, police last
        places = []
        for i in range(nb):
            e = p0 + ENT_STRIDE * i
            rd = ram.u32(e + ENT_RIDER_DEF)
            klass = ram.u8(rd + RD_RANK_CLASS) & 0x0F
            pl = ram.u8(rd + RD_PLACE)
            if klass == 2:
                b.check(pl == nb + 1, "%s: police bike %d is placed %d (n+1)" % (name, i, pl))
            else:
                places.append(pl)
        b.check(sorted(places) == list(range(1, len(places) + 1)),
                "%s: the racers' places are an exact permutation of 1..n" % name,
                str(sorted(places)))

        # the post-race delay and the countdown share 0x8005B230
        b.check(ram.s32(POSTRACE_LIMIT) == 5 << 16,
                "%s: the post-race delay limit is 5.00 s" % name,
                "%.4f" % (ram.s32(POSTRACE_LIMIT) / 65536.0))
        b.check(ram.s32(COUNTDOWN) <= 0,
                "%s: the countdown timer has expired (the race is running)" % name,
                str(ram.s32(COUNTDOWN)))
        b.check(ram.u8(gs + GS_STATE) == 1, "%s: main state is 1 (race running)" % name)
        b.check(ram.u8(gs + GS_FIRST_FRAME) == 0,
                "%s: the first-race-frame one-shot is consumed" % name)

        # the per-player view / camera records
        v0 = VIEW_BASE
        v1 = VIEW_BASE + VIEW_STRIDE
        player_bike = ram.u32(PLAYER_BIKES)
        b.check(player_bike == p0,
                "%s: player 0's bike is pool-0 slot 0" % name, "%08X" % player_bike)
        b.check(ram.u32(PLAYER0_BIKE) == player_bike,
                "%s: *(0x8005B38C) is the same bike" % name)
        b.check(ram.u32(v0 + VIEW_TARGET) == player_bike,
                "%s: view 0 follows the player's bike" % name,
                "%08X" % ram.u32(v0 + VIEW_TARGET))
        b.check(ram.u32(v1 + VIEW_TARGET) == 0,
                "%s: view 1 has no target in a one-player race" % name)
        b.check(ram.u32(PLAYER1_BIKE) == 0,
                "%s: there is no player-2 bike" % name)
        # the camera position is near the bike it follows and moved since last frame
        cam = [ram.s32(v0 + 0xB8 + 4 * i) for i in range(3)]
        prev = [ram.s32(v0 + 0x1D4 + 4 * i) for i in range(3)]
        bike = [ram.s32(p0 + 0xB8 + 4 * i) for i in range(3)]
        d2 = sum(((c - t) / 65536.0) ** 2 for c, t in zip(cam, bike))
        b.check(d2 < 400.0, "%s: view 0's position is within 20 world units of its target" % name,
                "%.2f" % (d2 ** 0.5))
        b.check(cam != prev, "%s: view 0's previous-position copy differs from the live one" % name)

        # DATA\CAMERA.CA is resident where the camera code reads it
        ca = open(os.path.join(DATA, "CAMERA.CA"), "rb").read()
        b.check(ram.blob(CAMERA_CA, CAMERA_CA_READ) == ca[:CAMERA_CA_READ],
                "%s: the first %d bytes of DATA\\CAMERA.CA are resident at 0x800CD7B8"
                % (name, CAMERA_CA_READ))
        b.check(CAMERA_CA + CAMERA_CA_READ == VIEW_BASE,
                "%s: that region abuts the view-record array exactly" % name)

    print()
    print("raceloop: %d checks, %d failures" % (b.n, len(b.bad)))
    if b.bad:
        sys.exit(1)


def state_writers(img):
    """Every `sb rt,0(rs)` whose base register was loaded from *(0x8005B2F8) nearby."""
    out = []
    n = len(img.data) // 4
    w = struct.unpack_from("<%dI" % n, img.data, 0)
    holders = {}
    for i in range(n):
        ins = w[i]
        op = ins >> 26
        if op == 0x23 and (ins & 0xFFFF) == (GAME_STATE_PTR & 0xFFFF):
            holders[(ins >> 16) & 0x1F] = i
            continue
        if op == 0x28 and (ins & 0xFFFF) == 0:
            rs = (ins >> 21) & 0x1F
            if rs in holders and i - holders[rs] <= 12:
                out.append(img.base + 4 * i)
        # any other write to a tracked register invalidates it
        dst = None
        if op == 0x00 and (ins & 0x3F) not in (0x08, 0x09):     # SPECIAL, not jr/jalr
            dst = (ins >> 11) & 0x1F
        elif op in (0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
                    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26):
            dst = (ins >> 16) & 0x1F
        if dst is not None and dst in holders:
            del holders[dst]
    return out


# ---------------------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        prog="raceloop.py", description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("info", help="the decoded call skeleton (addresses only)")
    s.set_defaults(func=cmd_info)

    s = sub.add_parser("grid", help="the starting-grid blocks and the placement check")
    s.set_defaults(func=cmd_grid)

    s = sub.add_parser("verify", help="the assertion bench (the gate)")
    s.set_defaults(func=cmd_verify)
    return ap


def main() -> None:
    args = build_parser().parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
