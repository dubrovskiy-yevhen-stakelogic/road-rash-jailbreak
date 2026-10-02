"""Scout probes for COLLISION and COMBAT in Road Rash: Jailbreak (USA, SLUS_01053).

The subject is the shape of the collision pass `RASHCDG 0x800A4774` and of the combat
machine that hangs off `FIGHT.BIN`.  This probe re-derives that structure from the player's
own disc extract and savestates, independently of the C++ tree:

  * the BROAD PHASE is re-derived from the instruction words: the 24 x 48 byte grid at
    0x800CD0B0 filled with 0x80, the per-player origin `playerBike[+0xB8] + 15.0`, the
    cell arithmetic `((coord - origin) >> 21) + 11`, the 128-entry chain-node array at
    0x800CCFA8, the pool walk 6 -> 0, the four-corner insertion from `entity+0xC4` and the
    pool-6 arm that inserts the centre at `+0x0C` for class 1 and the four footprint
    corners at `+0x18` for class 0;
  * and then CHECKED AGAINST LIVE RAM: in every savestate the two grid origins are exactly
    `playerBike[+0xB8] + 0x000F0000` and `playerBike[+0xC0] + 0x000F0000`, and every chain
    node left in the grid names a live entity that really does have an inserted point
    landing in that very cell - which is what pins the cell formula numerically;
  * the ORIENTED BOX layout is settled arithmetically rather than by reading: over every
    live entity of pools 0, 2 and 4 in all four captures, `entity+0xB8` is EXACTLY the
    integer mean of the FIRST FOUR corners at `+0xC4`, and never the mean of all eight.
    That is the built-in negative control for the `+0xB8` / `+0xC4` question;
  * the pool-1 skip is checked from both sides: the collision pass inserts a rider only
    while `rider+0x25C >= 2`, and a rider's box is maintained only in exactly those cases;
  * the three jump tables (0x8005B8F8 kind 0..4, 0x8005B910 partner pool 0..6,
    0x8005B930 partner pool of a rider/pedestrian) are read out of the overlay and
    compared with the named resolvers;
  * `rider+0x25C` (the mount state) is re-derived from the resident rider-state table
    `SLUS 0x800541D4` through the mapping the state setter `RASHCDG 0x800C2FF4` performs,
    and compared with what every live rider in every capture actually holds;
  * `DATA\\FIGHT.BIN` is re-parsed with the 12-byte record / node / edge layout and every
    block offset, node count and rider-state id is range-checked;
  * the player's combat-move decoder `RASHCDG 0x800C2348` is checked to have exactly one
    caller in all four images and to emit exactly the documented command alphabet.

Reads ONLY from work\\disc_us (the player's own disc extract, gitignored) and from
work\\oracle\\state (savestate extracts).  Writes ONLY to stdout.  Never copies game bytes
into the repository.

Usage (from the project root):

    python tools\\scout\\collide.py info        # the decoded skeleton, addresses only
    python tools\\scout\\collide.py grid        # the live broad-phase grid of each capture
    python tools\\scout\\collide.py fight       # DATA\\FIGHT.BIN structure and the move census
    python tools\\scout\\collide.py verify      # the assertion bench (the gate)
    python tools\\scout\\collide.py verify --mutate   # the deliberate-failure demonstration

`verify` ends with a single line a gate can match:

    collide: <n> checks, <m> failures
"""

from __future__ import annotations

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import exe as E  # noqa: E402  (sibling scout module; read-only use)

ROOT = E.ROOT
DISC = E.DISC
DATA = os.path.join(DISC, "DATA")
STATE_DIR = os.path.join(ROOT, "work", "oracle", "state")
STATES = ("rr-race", "rr-pack", "rr-grid", "quick")
DUMP_DIR = os.path.join(ROOT, "work", "oracle", "vr_capture", "ramdumps")

OVL_BASE = 0x8005B5E8

SHA1 = {
    "SLUS": "67ed165a2c517d4e6106fb0dfa324d66dd9a76f1",
    "F": "a3fec4b4e9292c358d0f6dc529843f5d8f25924a",
    "G": "cfe43a7786759f2cb9c57751cf99e84d1074782c",
    "I": "9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06",
}
FIGHT_SHA1 = "15e41265f8b0379d4ca080924ffe5f73a76e97dc"

# ---------------------------------------------------------------------------
# Guest globals the collision pass lives in, each derived from the instruction that establishes it.
# ---------------------------------------------------------------------------

COLLIDE = 0x800A4774             # the pass itself
COLLIDE_TAIL = 0x800A77B0        # the deferred contact handler

GRID = 0x800CD0B0                # u8[24 * 48], 0x80 = empty
GRID_W = 24
GRID_ROWS = 48
GRID_EMPTY = 0x80
NODES = 0x800CCFA8               # {u8 prev; u8 handle} x 128, prev 128 = end of chain
NODE_CAP = 128
ORIGIN_X = 0x800CCF98            # s32[2], playerBike[+0xB8] + 15.0
ORIGIN_Z = 0x800CCFA0            # s32[2], playerBike[+0xC0] + 15.0
ORIGIN_BIAS = 0x000F0000         # 15.0 in 16.16
CELL_SHIFT = 21                  # 1 << 21 in 16.16 = 32.0 world units per cell
CELL_BIAS = 11                   # the player always lands in cell (10, 10)

CONTACTS = 0x800CCE48            # 8 x 36 bytes; +0x00 handleA, +0x02 handleB
CONTACT_STRIDE = 36
CONTACT_COUNT = 0x800CCF68
KIND_RADIUS = 0x800CCA8C         # s32[5], the per-kind broad-phase radius
KIND_RADIUS_EXPECT = (0x00060000, 0x00048000, 0x00048000, 0x00070000, 0x00030000)

POOL_TABLE = 0x800CE4D0
POOL6_BASE = 0x800CD6C4          # pool 6 keeps its base outside the pool table
POOL6_STRIDE = 280

PLAYER0_BIKE = 0x8005B38C
PLAYER1_BIKE = 0x8005B21C
GAME_STATE_PTR = 0x8005B2F8
GS_NUM_PLAYERS = 0x30

ENT_HANDLE = 0x0AC               # u16, (pool << 5) | slot
ENT_CENTRE = 0x0B8               # 3 x 16.16, the mean of corners 0..3
ENT_CORNERS = 0x0C4              # 8 x 3 x 16.16
ENT_STATE = 0x140                # u16, bit 0 = live
ENT_MOUNT = 0x25C                # pool 1: the mount state; pool 0: the rev counter
VOL_HANDLE = 0x00                # a pool-6 entry keeps its handle at +0x00
VOL_CLS = 0x08
VOL_CENTRE = 0x0C
VOL_CORNERS = 0x18

RIDER_STATE_TABLE = 0x800541D4   # SLUS, 8 bytes per record, 0..224
RIDER_STATE_COUNT = 225
COMBAT_INPUT_MAP = 0x800CCB78    # u16[2] per combat action n

JT_KIND = 0x8005B8F8             # 5 entries, the moving entity's kind 0..4
JT_PARTNER_BIKE = 0x8005B910     # 7 entries, the partner's pool when the mover is a bike
JT_PARTNER_RIDER = 0x8005B930    # 6 entries, ... when the mover is a rider or pedestrian

# ---------------------------------------------------------------------------
# The call skeleton.  Each row is (image, site, callee, what) and is checked by
# decoding the instruction word at `site`; nothing here is assumed.
# ---------------------------------------------------------------------------

SKELETON = [
    ("G", 0x8008AC50, 0x800A4774, "RaceTick -> the collision pass"),
    # the broad phase
    ("G", 0x800A4B74, 0x8001E100, "collision -> memset the 288-byte contact list"),
    ("G", 0x800A4BC4, 0x8001E100, "collision -> memset the 32 bytes below the node array"),
    # bike vs bike
    ("G", 0x800A533C, 0x800B09C4, "bike x bike -> the point resolver (the other is special)"),
    ("G", 0x800A5384, 0x800B09C4, "bike x bike -> the point resolver (I am special)"),
    ("G", 0x800A5394, 0x800AB7A0, "bike x bike -> the box-pair resolver"),
    # the kind-0 (bike) arm, dispatched on the partner's pool
    ("G", 0x800A5C48, 0x800AD04C, "bike x rider / pedestrian"),
    ("G", 0x800A5C5C, 0x800AC5BC, "bike x traffic vehicle"),
    ("G", 0x800A5CDC, 0x800B0D8C, "bike x prop, large footprint"),
    ("G", 0x800A5CEC, 0x800B09C4, "bike x prop, small footprint"),
    ("G", 0x800A5D3C, 0x800B0D8C, "bike x static volume class 0, large footprint"),
    ("G", 0x800A5D4C, 0x800B09C4, "bike x static volume class 0, small footprint"),
    ("G", 0x800A5D5C, 0x800AE794, "bike x static volume class 1 (the pole resolver)"),
    ("G", 0x800A5E68, 0x800B3AD0, "bike -> the contact integrator"),
    ("G", 0x800A5EC0, 0x800B3AD0, "bike -> the contact integrator, second pass"),
    ("G", 0x800A5EF0, 0x800B1978, "bike -> the wipeout arm"),
    ("G", 0x800A5F60, 0x80017BA0, "bike -> PlaySound3D on a fresh impact"),
    ("G", 0x800A5FC0, 0x800B658C, "bike -> the pad rumble"),
    # the kind-1 / kind-2 (rider, pedestrian) arm
    ("G", 0x800A6648, 0x800B2844, "rider / pedestrian x rider / pedestrian"),
    ("G", 0x800A6670, 0x800B2B00, "rider / pedestrian x traffic or prop"),
    ("G", 0x800A677C, 0x800B3AD0, "rider / pedestrian -> the contact integrator"),
    ("G", 0x800A67AC, 0x800B208C, "rider / pedestrian -> the ground / road arm"),
    # the kind-3 (traffic) arm
    ("G", 0x800A6E3C, 0x800B2D44, "traffic x traffic"),
    ("G", 0x800A6E50, 0x800B2D88, "traffic x prop"),
    # the kind-4 (prop) arm
    ("G", 0x800A76CC, 0x800B3AD0, "prop -> the contact integrator"),
    ("G", 0x800A7724, 0x800B3344, "prop -> topple"),
    ("G", 0x800A7760, 0x800A77B0, "collision -> the deferred contact handler"),
    # the shared response solvers
    ("G", 0x800AB9F8, 0x800ABE78, "the box-pair resolver -> the narrow-phase box test"),
    ("G", 0x800ABB30, 0x800AA474, "the box-pair resolver -> response A"),
    ("G", 0x800ABB70, 0x800AAD30, "the box-pair resolver -> response B"),
    ("G", 0x800AC678, 0x800AAD30, "bike x traffic -> the same response B"),
    ("G", 0x800AD1BC, 0x800AAD30, "bike x rider -> the same response B"),
    ("G", 0x800B0AFC, 0x800AAD30, "the point resolver -> the same response B"),
    # combat
    ("SLUS", 0x8001D368, 0x800C2348, "PollPads -> the combat-move decoder"),
    ("G", 0x800BA780, 0x800C035C, "AI command 16 -> FightUpdate"),
    ("G", 0x800B96F0, 0x800C035C, "RaceDirector's two-rider arm -> FightUpdate"),
    ("G", 0x800C0890, 0x800C159C, "FightUpdate -> the reach test"),
    ("G", 0x800C0978, 0x800C17B0, "FightUpdate -> ApplyHit"),
    ("G", 0x800C1B78, 0x800BF674, "ApplyHit -> the knock-off / reaction applier"),
    ("G", 0x800BF848, 0x80096F30, "the reaction applier -> Arrest(attacker, victim, 9)"),
    ("G", 0x800C3160, 0x800C4550, "RiderDismount -> the idle-stance trigger"),
    # falling off and getting back on
    ("G", 0x80076C64, 0x80074E6C, "the per-bike step -> BikeCrashTimer"),
    ("G", 0x8007C988, 0x80074E6C, "the rider + engine pass -> BikeCrashTimer"),
    ("G", 0x80074F0C, 0x80027028, "BikeCrashTimer -> the crash-emitter test"),
    ("G", 0x80074F20, 0x80027540, "BikeCrashTimer -> the crash emitter"),
    ("G", 0x800B9538, 0x800903F4, "RaceDirector's remount arm -> SetMountState"),
]

# ---------------------------------------------------------------------------
# Instruction words quoted verbatim.  (image, address, word, what)
# ---------------------------------------------------------------------------

LITERALS = [
    # --- the broad-phase grid --------------------------------------------
    ("G", 0x800A4780, 0x3C088080, "lui t0,0x8080   - the empty-cell fill pattern"),
    ("G", 0x800A4784, 0x35088080, "ori t0,t0,0x8080"),
    ("G", 0x800A4790, 0x3C02800D, "lui v0,0x800d   - the grid base"),
    ("G", 0x800A4794, 0x2442D0B0, "addiu v0,v0,-12112  = 0x800CD0B0"),
    ("G", 0x800A47F4, 0x254A0018, "addiu t2,t2,24  - 24 bytes (cells) per row"),
    ("G", 0x800A480C, 0x2AA20030, "slti v0,s5,48   - 48 rows"),
    # --- the per-player origin -------------------------------------------
    ("G", 0x800A4828, 0x3C04000F, "lui a0,0xf      - the 15.0 bias on the origin"),
    ("G", 0x800A4830, 0xACC2CF98, "sw v0,-12392(a2)  -> 0x800CCF98, the X origin"),
    ("G", 0x800A4840, 0xACA2CFA0, "sw v0,-12384(a1)  -> 0x800CCFA0, the Z origin"),
    # --- the cell arithmetic ---------------------------------------------
    ("G", 0x800A4A14, 0x00021543, "sra v0,v0,0x15  - 32.0 world units per cell"),
    ("G", 0x800A4A18, 0x2444000B, "addiu a0,v0,11  - the player's own cell is 10"),
    ("G", 0x800A4A38, 0x2C820018, "sltiu v0,a0,24  - 24 columns"),
    ("G", 0x800A4A70, 0x004D1004, "sllv v0,t5,v0   - the second player's 24-row band"),
    ("G", 0x800A4AAC, 0x000210C0, "sll v0,v0,0x3   - row * 24"),
    ("G", 0x800A4AB0, 0x00821021, "addu v0,a0,v0   - cell = column + 24 * row"),
    ("G", 0x800A4AC0, 0x108C0007, "beq a0,t4       - 128 means the cell is empty"),
    ("G", 0x800A4B20, 0x104C000C, "beq v0,t4       - the node array holds 128 entries"),
    # --- what is inserted -------------------------------------------------
    ("G", 0x800A48E4, 0x245100C4, "addiu s1,v0,196 - pools 0..5 insert from entity+0xC4"),
    ("G", 0x800A4B50, 0x24090004, "li t1,4         - ... four corners each"),
    ("G", 0x800A4950, 0x26110018, "addiu s1,s0,24  - pool 6 class 0: the four +0x18 corners"),
    ("G", 0x800A4964, 0x24090004, "li t1,4"),
    ("G", 0x800A4954, 0x2611000C, "addiu s1,s0,12  - pool 6 class 1: the +0x0C centre"),
    ("G", 0x800A495C, 0x24090001, "li t1,1"),
    ("G", 0x800A4968, 0x86020140, "lh v0,320(s0)   - the +0x140 live test"),
    ("G", 0x800A4908, 0x8E02025C, "lw v0,604(s0)   - pool 1: the mount state"),
    ("G", 0x800A4910, 0x2C420002, "sltiu v0,v0,2   - ... < 2 skips the rider"),
    ("G", 0x800A4934, 0x86020094, "lh v0,148(s0)   - pool 6: the +0x94 gate"),
    ("G", 0x800A49C8, 0xAE000228, "sw zero,552(s0) - the bike's +0x228 is cleared per frame"),
    # --- the partner lookup ----------------------------------------------
    ("G", 0x800A5184, 0x00071942, "srl v1,a3,0x5   - the partner's pool from its handle"),
    ("G", 0x800A51AC, 0x30E3001F, "andi v1,a3,0x1f - ... and its slot"),
    ("G", 0x800A51C4, 0x244400B8, "addiu a0,v0,184 - pools 0..5: the point is +0xB8"),
    ("G", 0x800A51EC, 0x2444000C, "addiu a0,v0,12  - pool 6: the point is +0x0C"),
    ("G", 0x800A51E0, 0x8C83D6C4, "lw v1,-10556(a0)  = 0x800CD6C4, the pool-6 base"),
    ("G", 0x800A5250, 0x0058102A, "slt v0,v0,t8    - max + min/2 against the kind radius"),
    ("G", 0x800A4C38, 0x8C42CA8C, "lw v0,-13684(v0)  = 0x800CCA8C, the kind radius table"),
    # --- the two outer loops ---------------------------------------------
    ("G", 0x800A55B4, 0x2E820005, "sltiu v0,s4,5   - the kind loop runs 0..4"),
    ("G", 0x800A7770, 0x2A820005, "slti v0,s4,5"),
    ("G", 0x800A55C8, 0x2442B8F8, "addiu v0,v0,-18184 = 0x8005B8F8, the kind jump table"),
    ("G", 0x800A5C0C, 0x2442B910, "addiu v0,v0,-18160 = 0x8005B910, the partner table"),
    ("G", 0x800A5C00, 0x2C620007, "sltiu v0,v1,7   - seven pools in that table"),
    # --- the contact list -------------------------------------------------
    ("G", 0x800A4B6C, 0x24060120, "li a2,288       - 8 contact records of 36 bytes"),
    ("G", 0x800A4B88, 0x24060008, "li a2,8"),
    # --- falling off ------------------------------------------------------
    ("G", 0x80074E84, 0x30420200, "andi v0,v0,0x200 - +0x234 bit 9 runs the wipeout timer"),
    ("G", 0x80074EC4, 0x30620002, "andi v0,v1,0x2   - +0x230 bit 1, crash armed"),
    ("G", 0x80074ECC, 0x30620004, "andi v0,v1,0x4   - +0x230 bit 2, the crash edge"),
    ("G", 0x80074F10, 0xA2030350, "sb v1,848(s0)    - one crash spent from +0x350"),
    # --- the mount state --------------------------------------------------
    ("G", 0x800C3000, 0x14430036, "bne v0,v1        - SetRiderState only maps pool 1"),
    ("G", 0x800C3028, 0x2C620005, "sltiu v0,v1,5    - category < 5  -> mount 1"),
    ("G", 0x800C3034, 0x2C620006, "sltiu v0,v1,6    - category 5     -> mount 2"),
    ("G", 0x800C3040, 0x2C620007, "sltiu v0,v1,7    - category 6     -> mount 3"),
    ("G", 0x800C3048, 0x24070004, "li a3,4          - category >= 7  -> mount 4"),
    ("G", 0x800C3068, 0xACA7025C, "sw a3,604(a1)    - the mount state is written"),
    ("G", 0x800C31B4, 0xAE02025C, "sw v0,604(s0)    - RiderDismount's own mount 4"),
    ("G", 0x800C31B0, 0x24020004, "li v0,4"),
    # --- the combat command alphabet --------------------------------------
    ("G", 0x800C237C, 0x2402004D, "li v0,77   - combat action 8"),
    ("G", 0x800C2390, 0x2402004B, "li v0,75   - combat action 4"),
    ("G", 0x800C23A4, 0x24020047, "li v0,71   - combat action 3"),
    ("G", 0x800C24D8, 0x24020020, "li v0,32   - action 1, bare fists"),
    ("G", 0x800C24F0, 0x2402008E, "li v0,142  - action 1, armed"),
    ("G", 0x800C251C, 0x24020024, "li v0,36   - action 2, bare fists"),
    ("G", 0x800C2530, 0x24020092, "li v0,146  - action 2, armed"),
    ("G", 0x800C2534, 0xA202003C, "sb v0,60(s0) - into riderDef+0x3C"),
]

# The three jump tables and the resolvers they name.
JUMP_TABLES = [
    ("the moving entity's kind", JT_KIND, [
        0x800A55E0, 0x800A5FD0, 0x800A5FE4, 0x800A67BC, 0x800A6F58]),
    ("the partner's pool, mover = bike", JT_PARTNER_BIKE, [
        0x800A5D64, 0x800A5C28, 0x800A5C28, 0x800A5C58,
        0x800A5C6C, 0x800A5CAC, 0x800A5CFC]),
    ("the partner's pool, mover = rider or pedestrian", JT_PARTNER_RIDER, [
        0x800A6630, 0x800A6630, 0x800A6644, 0x800A6658, 0x800A6658, 0x800A6668]),
]

# The command bytes RASHCDG 0x800C2348 can write into riderDef+0x3C.
COMBAT_COMMANDS = (32, 36, 38, 71, 75, 77, 142, 146, 148)

# The pools the broad phase walks, in the order it walks them.
INSERT_POOLS = (6, 5, 4, 3, 2, 1, 0)


# ---------------------------------------------------------------------------
# Helpers
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


def count_jal(imgs, target):
    """How many `jal target` sites exist across all four images."""
    n = 0
    for img in imgs.values():
        for w in struct.unpack_from("<%dI" % (len(img.data) // 4), img.data, 0):
            if (w >> 26) == 3 and (0x80000000 | ((w & 0x03FFFFFF) << 2)) == target:
                n += 1
    return n


class Ram:
    def __init__(self, path):
        self.d = open(path, "rb").read()

    def u8(self, a):
        return self.d[a & 0x1FFFFF]

    def u16(self, a):
        return struct.unpack_from("<H", self.d, a & 0x1FFFFF)[0]

    def s32(self, a):
        return struct.unpack_from("<i", self.d, a & 0x1FFFFF)[0]

    def u32(self, a):
        return struct.unpack_from("<I", self.d, a & 0x1FFFFF)[0]


def states():
    out = []
    for name in STATES:
        p = os.path.join(STATE_DIR, name, "ram.bin")
        if os.path.isfile(p):
            out.append((name, Ram(p)))
    return out


def pools(ram):
    """{pool: (base, stride, live, high)} for pools 0..6 (pool 6 from its own block)."""
    out = {}
    for p in range(7):
        rec = POOL_TABLE + 16 * p
        base = ram.u32(rec)
        stride = ram.u32(rec + 4)
        live = ram.s32(ram.u32(rec + 8)) if ram.u32(rec + 8) else 0
        high = ram.s32(ram.u32(rec + 0xC)) if ram.u32(rec + 0xC) else -1
        out[p] = (base, stride, live, high)
    return out


def entity_points(ram, pl, handle, cfg):
    """The (x, z) points this handle contributes to the broad phase."""
    pool, slot = handle >> 5, handle & 0x1F
    if pool == 6:
        base = ram.u32(POOL6_BASE) + POOL6_STRIDE * slot
        if ram.u32(base + VOL_CLS) == 1:
            return [(ram.s32(base + VOL_CENTRE), ram.s32(base + VOL_CENTRE + 8))]
        return [(ram.s32(base + VOL_CORNERS + 12 * j),
                 ram.s32(base + VOL_CORNERS + 12 * j + 8)) for j in range(4)]
    b, stride = pl[pool][0], pl[pool][1]
    e = b + stride * slot
    off = cfg["corners"]
    return [(ram.s32(e + off + 12 * j), ram.s32(e + off + 12 * j + 8)) for j in range(4)]


def cell_of(coord, origin, cfg):
    return ((coord - origin) >> cfg["shift"]) + CELL_BIAS


def chains(ram, cfg):
    """[(column, row, [handle, ...]), ...] reconstructed from the live grid."""
    out = []
    for cell in range(GRID_W * GRID_ROWS):
        head = ram.u8(GRID + cell)
        if head == GRID_EMPTY:
            continue
        n, seen = head, []
        while n != GRID_EMPTY and len(seen) <= NODE_CAP:
            seen.append(ram.u8(NODES + 2 * n + 1))
            n = ram.u8(NODES + 2 * n + 0)
        out.append((cell % GRID_W, cell // GRID_W, seen))
    return out


def dumps():
    if not os.path.isdir(DUMP_DIR):
        return []
    out = []
    for name in sorted(os.listdir(DUMP_DIR)):
        if name.startswith("ram_") and name.endswith(".bin"):
            out.append((name, Ram(os.path.join(DUMP_DIR, name))))
    return out


def rider_state_table():
    """[(anim, category, w2, w3)] x 225 out of SLUS_010.53."""
    img = E.load_exe()
    o = RIDER_STATE_TABLE - img.base
    return [struct.unpack_from("<HHHH", img.data, o + 8 * i)
            for i in range(RIDER_STATE_COUNT)]


def mount_of_category(cat, cfg):
    """The mapping RASHCDG 0x800C2FF4 performs, category -> rider+0x25C."""
    if cat == 0:
        return 0
    if cat < 5:
        return 1
    if cat < 6:
        return 2
    if cat < 7:
        return 3
    return cfg["mount_high"]


def fight_file():
    return open(os.path.join(DATA, "FIGHT.BIN"), "rb").read()


def fight_records(raw):
    out = []
    for i in range(40):
        idle, ca, cb, blka, blkb = struct.unpack_from("<HBBII", raw, 12 * i)
        out.append(dict(index=i, idle=idle, countA=ca, countB=cb, blockA=blka, blockB=blkb))
    return out


def fight_nodes(raw, rec):
    return [struct.unpack_from("<HHHHHBB", raw, rec["blockB"] + 12 * k)
            for k in range(rec["countB"])]


def fight_edges(raw, rec):
    return [struct.unpack_from("<iiI", raw, rec["blockA"] + 12 * k)
            for k in range(rec["countA"])]


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


def config(mutate: bool) -> dict:
    """The claims the numeric checks depend on.  --mutate perturbs them so the probe
    can be seen failing on demand; every perturbation is one of the load-bearing
    statements."""
    cfg = dict(centre=ENT_CENTRE, corners=ENT_CORNERS, shift=CELL_SHIFT,
               bias=ORIGIN_BIAS, mount_high=4, skeleton_callee=0x800AB7A0)
    if mutate:
        cfg["centre"] = ENT_CENTRE + 4      # "the centre is the second word of the box"
        cfg["shift"] = CELL_SHIFT - 1       # "cells are 16.0 world units"
        cfg["mount_high"] = 3               # "categories 7 and 8 share mount 3"
        cfg["skeleton_callee"] = 0x800AD04C  # "bike x bike is the rider resolver"
    return cfg


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------


def cmd_info(_args):
    imgs = load_images()
    for k in ("SLUS", "F", "G", "I"):
        print("%-5s %-14s sha1 %s" % (k, os.path.basename(imgs[k].path), imgs[k].sha1))
    print()
    print("the collision pass and its children, decoded from the instruction words:")
    for key, site, callee, what in SKELETON:
        got = jal_target(imgs[key], site)
        print("  %-4s %08X  jal %08X  %-58s %s"
              % (key, site, callee, what, "ok" if got == callee else "MISMATCH %08X" % (got or 0)))
    print()
    for what, addr, expect in JUMP_TABLES:
        got = [word(imgs["G"], addr + 4 * i) for i in range(len(expect))]
        print("  jump table %08X (%s):" % (addr, what))
        for i, (g, e) in enumerate(zip(got, expect)):
            print("    [%d] %08X %s" % (i, g or 0, "ok" if g == e else "MISMATCH, expected %08X" % e))
    print()
    print("combat-move decoder RASHCDG 0x800C2348: %d caller(s) in the four images"
          % count_jal(imgs, 0x800C2348))
    print("FightUpdate  RASHCDG 0x800C035C: %d caller(s)" % count_jal(imgs, 0x800C035C))
    print("the response solver 0x800AAD30: %d caller(s)" % count_jal(imgs, 0x800AAD30))


def cmd_grid(_args):
    cfg = config(False)
    for name, ram in states():
        pl = pools(ram)
        ox, oz = ram.s32(ORIGIN_X), ram.s32(ORIGIN_Z)
        print("== %s   origin = (%.4f, %.4f)" % (name, ox / 65536.0, oz / 65536.0))
        for col, row, seen in chains(ram, cfg):
            marks = []
            for h in seen:
                hit = any(cell_of(x, ox, cfg) == col and cell_of(z, oz, cfg) == row
                          for x, z in entity_points(ram, pl, h, cfg))
                marks.append("%d%s" % (h, "" if hit else "?"))
            print("   cell(%2d,%2d)  %s" % (col, row, " <- ".join(marks)))
        print("   contacts logged: %d" % ram.u32(CONTACT_COUNT))


def cmd_fight(_args):
    raw = fight_file()
    recs = fight_records(raw)
    print("DATA\\FIGHT.BIN  %d bytes, %d records of 12 bytes (the loader relocates exactly 40)"
          % (len(raw), len(recs)))
    tbl = rider_state_table()
    dmg, reach = [], []
    for r in recs:
        nodes = fight_nodes(raw, r)
        for (a_start, a_end, a_hit, a_miss, d, rc, hf) in nodes:
            if d:
                dmg.append(d)
            reach.append(rc)
        print("  rec %2d idle %3d  A=%d @%04X  B=%d @%04X  nodes: %s"
              % (r["index"], r["idle"], r["countA"], r["blockA"], r["countB"], r["blockB"],
                 " ".join("%d/%d" % (n[0], n[4]) for n in nodes)))
    print()
    print("  %d striking nodes, damage %d..%d, reach byte %d..%d"
          % (len(dmg), min(dmg), max(dmg), min(reach), max(reach)))
    cats = {}
    for i, rec in enumerate(tbl):
        cats.setdefault(rec[1], []).append(i)
    print("  rider-state categories (SLUS 0x800541D4): %s"
          % ", ".join("%d:%d" % (c, len(v)) for c, v in sorted(cats.items())))


def cmd_verify(args):
    cfg = config(args.mutate)
    imgs = load_images()
    b = Bench()

    print("== images")
    for k in ("SLUS", "F", "G", "I"):
        b.check(imgs[k].sha1 == SHA1[k], "%s sha1 matches the documented image" % k,
                imgs[k].sha1)
    b.check(E.sha1_of(os.path.join(DATA, "FIGHT.BIN")) == FIGHT_SHA1,
            "DATA\\FIGHT.BIN sha1 matches the documented file")

    print("== the collision / combat call skeleton, decoded from the instruction words")
    for key, site, callee, what in SKELETON:
        want = cfg["skeleton_callee"] if site == 0x800A5394 else callee
        b.check(jal_target(imgs[key], site) == want,
                "%s %08X: jal %08X (%s)" % (key, site, want, what),
                "%08X" % (jal_target(imgs[key], site) or 0))

    print("== quoted instruction words")
    for key, addr, w, what in LITERALS:
        b.check(word(imgs[key], addr) == w, "%s %08X == %08X (%s)" % (key, addr, w, what),
                "%08X" % (word(imgs[key], addr) or 0))

    print("== the three jump tables, read out of the overlay")
    for what, addr, expect in JUMP_TABLES:
        got = [word(imgs["G"], addr + 4 * i) for i in range(len(expect))]
        b.check(got == expect, "jump table %08X is %s" % (addr, what),
                " ".join("%08X" % (g or 0) for g in got))
    b.check(word(imgs["G"], JT_KIND + 4 * 5) == 0,
            "the kind jump table has exactly five entries (the sixth word is 0)")

    print("== the caller counts, scanned over all four images")
    b.check(count_jal(imgs, COLLIDE) == 2,
            "the collision pass 0x800A4774 has exactly two callers: the race tick and the "
            "one-shot prime pass")
    b.check(jal_target(imgs["SLUS"], 0x80011B20) == COLLIDE,
            "... the second one is SLUS 0x80011B20, inside the prime pass 0x800119C0")
    b.check(count_jal(imgs, COLLIDE_TAIL) == 1,
            "the deferred contact handler 0x800A77B0 has exactly one caller")
    b.check(count_jal(imgs, 0x800C2348) == 1,
            "the combat-move decoder 0x800C2348 has exactly one caller")
    b.check(count_jal(imgs, 0x800C035C) == 2,
            "FightUpdate 0x800C035C has exactly two callers")
    b.check(count_jal(imgs, 0x800AB7A0) == 2,
            "the bike-vs-bike resolver 0x800AB7A0 has two callers: the pass and the "
            "chain-reaction helper 0x800A7AB4")
    b.check(jal_target(imgs["G"], 0x800ABE20) == 0x800A7AB4
            and jal_target(imgs["G"], 0x800A7B98) == 0x800AB7A0,
            "... and 0x800AB7A0 and 0x800A7AB4 really are mutually recursive")
    b.check(count_jal(imgs, 0x800B3AD0) == 5,
            "the contact integrator 0x800B3AD0 has five call sites")

    print("== two negatives, scanned over the pass's own instruction words")
    riderdef = corners47 = 0
    for a in range(COLLIDE, COLLIDE + 12348, 4):
        w = word(imgs["G"], a)
        if w is None:
            continue
        op = w >> 26
        if op in (0x20, 0x21, 0x23, 0x24, 0x25, 0x28, 0x29, 0x2B):   # lb/lh/lw/lbu/lhu/sb/sh/sw
            rs = (w >> 21) & 0x1F
            imm = w & 0xFFFF
            if imm >= 0x8000:
                imm -= 0x10000
            if rs == 29:                                              # sp: a local
                continue
            if imm == 0x43C:
                riderdef += 1
            if 0xF4 <= imm <= 0x123:
                corners47 += 1
    b.check(riderdef == 0,
            "the collision pass never dereferences entity+0x43C - nothing in it is "
            "class-aware, so there is no police arm inside collision", "%d" % riderdef)
    b.check(corners47 == 0,
            "the collision pass never touches entity+0xF4..+0x123 - only the FIRST FOUR "
            "box corners are inserted and tested", "%d" % corners47)

    print("== the combat command alphabet the decoder can emit")
    got = set()
    for a in range(0x800C2348, 0x800C2588, 4):
        w = word(imgs["G"], a)
        if w is not None and (w >> 16) == 0x2402:       # li v0,imm
            got.add(w & 0xFFFF)
    for c in COMBAT_COMMANDS:
        b.check(c in got, "the decoder builds command %d" % c)
    b.check(143 not in got,
            "the decoder never builds 143 - that is the AI's own armed command")

    print("== the rider mount state is the rider-state category, remapped")
    tbl = rider_state_table()
    cats = {}
    for i, rec in enumerate(tbl):
        cats.setdefault(rec[1], []).append(i)
    b.check(sorted(cats) == [0, 1, 2, 3, 4, 5, 6, 7, 8],
            "the resident rider-state table uses categories 0..8", str(sorted(cats)))
    b.check(tbl[224] == (0, 0, 0, 0) and tbl[223] == (0, 0, 0, 0),
            "states 223 and 224 are the zero records, which is why 224 means no animation")
    b.check(mount_of_category(3, cfg) == 1,
            "category 3 (the 131 combat states) leaves the rider on the bike")
    b.check(mount_of_category(8, cfg) == 4,
            "category 8 is a rider down on the road")
    b.check(mount_of_category(tbl[73][1], cfg) == 4,
            "state 73 - the one RiderDismount picks - maps to mount 4")

    print("== live RAM: the broad-phase origin, the box layout and the grid chains")
    st = states()
    b.check(len(st) == len(STATES), "all four savestates are present", "%d" % len(st))
    for name, ram in st:
        pl = pools(ram)
        bike = ram.u32(PLAYER0_BIKE)
        b.check(bike != 0, "%s: the player's bike pointer is set" % name)
        if not bike:
            continue
        b.check(ram.s32(ORIGIN_X) == ram.s32(bike + cfg["centre"]) + cfg["bias"],
                "%s: the X grid origin is playerBike[+0x%02X] + 15.0" % (name, cfg["centre"]),
                "%08X vs %08X" % (ram.s32(ORIGIN_X) & 0xFFFFFFFF,
                                  (ram.s32(bike + cfg["centre"]) + cfg["bias"]) & 0xFFFFFFFF))
        b.check(ram.s32(ORIGIN_Z) == ram.s32(bike + cfg["centre"] + 8) + cfg["bias"],
                "%s: the Z grid origin is playerBike[+0x%02X] + 15.0" % (name, cfg["centre"] + 8))
        b.check(ram.u8(NODES + 2 * NODE_CAP) == GRID_EMPTY,
                "%s: node[128].prev is 128, so an absent handle ends the walk" % name)
        for i, want in enumerate(KIND_RADIUS_EXPECT):
            b.check(ram.s32(KIND_RADIUS + 4 * i) == want,
                    "%s: the kind-%d broad-phase radius is %.2f" % (name, i, want / 65536.0),
                    "%08X" % (ram.s32(KIND_RADIUS + 4 * i) & 0xFFFFFFFF))

        # the oriented box: +0xB8 is the mean of the FIRST FOUR corners, exactly
        exact4 = exact8 = tested = 0
        for pool in (0, 2, 4):
            base, stride, _, high = pl[pool]
            for i in range(max(high + 1, 0)):
                e = base + stride * i
                if ram.u16(e + ENT_STATE) == 0:
                    continue
                tested += 1
                c = [ram.s32(e + cfg["centre"] + 4 * k) for k in range(3)]
                co = [[ram.s32(e + cfg["corners"] + 12 * j + 4 * k) for k in range(3)]
                      for j in range(8)]
                if all(c[k] == sum(cn[k] for cn in co[:4]) // 4 for k in range(3)):
                    exact4 += 1
                if all(c[k] == sum(cn[k] for cn in co) // 8 for k in range(3)):
                    exact8 += 1
        b.check(tested > 0 and exact4 == tested,
                "%s: +0x%02X is exactly the mean of the four corners at +0x%02X, on all "
                "%d live entities of pools 0/2/4" % (name, cfg["centre"], cfg["corners"], tested),
                "%d of %d" % (exact4, tested))
        b.check(exact8 == 0,
                "%s: ... and never the mean of all EIGHT corners (the negative control)" % name,
                "%d of %d" % (exact8, tested))

        # the pool-1 skip, from both sides
        base, stride, _, high = pl[1]
        agree = riders = 0
        for i in range(max(high + 1, 0)):
            e = base + stride * i
            if ram.u16(e + ENT_STATE) == 0:
                continue
            riders += 1
            mount = ram.s32(e + ENT_MOUNT)
            c = [ram.s32(e + cfg["centre"] + 4 * k) for k in range(3)]
            co = [[ram.s32(e + cfg["corners"] + 12 * j + 4 * k) for k in range(3)]
                  for j in range(4)]
            built = all(c[k] == sum(cn[k] for cn in co) // 4 for k in range(3))
            if built == (mount >= 2):
                agree += 1
            # and the mount state must be what the rider-state category says
            stt = ram.u16(e + 0x220)
            if stt < RIDER_STATE_COUNT:
                b.check(mount == mount_of_category(tbl[stt][1], cfg),
                        "%s: rider %d is in state %d, so its +0x25C is %d"
                        % (name, i, stt, mount_of_category(tbl[stt][1], cfg)), "%d" % mount)
        b.check(riders > 0 and agree == riders,
                "%s: a rider has a maintained box exactly when +0x25C >= 2 - the same "
                "predicate the broad phase skips on" % name, "%d of %d" % (agree, riders))

        # the grid chains: every node names an entity with a point in that very cell
        ox, oz = ram.s32(ORIGIN_X), ram.s32(ORIGIN_Z)
        nodes = hits = 0
        for col, row, seen in chains(ram, cfg):
            for h in seen:
                nodes += 1
                pts = entity_points(ram, pl, h, cfg)
                if any(cell_of(x, ox, cfg) == col and cell_of(z, oz, cfg) == row
                       for x, z in pts):
                    hits += 1
        b.check(nodes > 0, "%s: the broad-phase grid is not empty" % name, "%d" % nodes)
        b.check(nodes and hits == nodes,
                "%s: every one of the %d live chain nodes names an entity with an inserted "
                "point in that cell" % (name, nodes), "%d of %d" % (hits, nodes))

        # handle == (pool << 5) | slot for every node, and the pool set is the one inserted
        ok = pools_seen = 0
        seen_pools = set()
        for _, _, seen in chains(ram, cfg):
            for h in seen:
                pool, slot = h >> 5, h & 0x1F
                seen_pools.add(pool)
                if pool == 6:
                    base6 = ram.u32(POOL6_BASE) + POOL6_STRIDE * slot
                    ok += (ram.u16(base6 + VOL_HANDLE) == h)
                else:
                    pb, ps = pl[pool][0], pl[pool][1]
                    ok += (ram.u16(pb + ps * slot + ENT_HANDLE) == h)
                pools_seen += 1
        b.check(ok == pools_seen,
                "%s: every chain node's handle reads back as (pool << 5) | slot" % name,
                "%d of %d" % (ok, pools_seen))
        b.check(seen_pools <= set(INSERT_POOLS),
                "%s: only the pools the broad phase walks appear in the grid" % name,
                str(sorted(seen_pools)))

        # pool 6: class is 0 or 1, and class 1 contributes its centre only
        base6 = ram.u32(POOL6_BASE)
        clsok = vols = 0
        for slot in range(24):
            v = base6 + POOL6_STRIDE * slot
            h = ram.u16(v + VOL_HANDLE)
            if h == 0 or (h >> 5) != 6:
                continue
            vols += 1
            clsok += ram.u32(v + VOL_CLS) in (0, 1)
        b.check(vols == 0 or clsok == vols,
                "%s: every live static volume has class 0 or 1" % name,
                "%d of %d" % (clsok, vols))

    print("== the 14 frame dumps: the captured fall-off")
    dmp = dumps()
    b.check(len(dmp) == 14, "all 14 frame dumps are present", "%d" % len(dmp))
    timeline, samples, agree, implied, chain_ok, chain_n = [], 0, 0, 0, 0, 0
    for name, ram in dmp:
        pl = pools(ram)
        grid_handles = set()
        ox, oz = ram.s32(ORIGIN_X), ram.s32(ORIGIN_Z)
        for col, row, seen in chains(ram, cfg):
            for h in seen:
                grid_handles.add(h)
                chain_n += 1
                # The frame dumps are taken at a different point in the frame from the
                # savestates - after the presentation pass has rebuilt the boxes - so an
                # entity can have moved up to one cell since the grid was filled.  The
                # savestates are checked for exact equality above; here the predicate is
                # "within one cell", which still pins the origin, the shift and the bias.
                if any(abs(cell_of(x, ox, cfg) - col) <= 1 and abs(cell_of(z, oz, cfg) - row) <= 1
                       for x, z in entity_points(ram, pl, h, cfg)):
                    chain_ok += 1
        base, stride, _, high = pl[1]
        for i in range(max(high + 1, 0)):
            e = base + stride * i
            if ram.u16(e + ENT_STATE) == 0:
                continue
            stt, mount = ram.u16(e + 0x220), ram.s32(e + ENT_MOUNT)
            if stt >= RIDER_STATE_COUNT:
                continue
            samples += 1
            agree += (mount == mount_of_category(tbl[stt][1], cfg))
            implied += (not ((32 + i) in grid_handles) or mount >= 2)
        bike = ram.u32(PLAYER0_BIKE)
        rider = ram.u32(bike + 0x354)
        rd = ram.u32(bike + 0x43C)
        timeline.append((ram.u16(rider + 0x220), ram.s32(rider + ENT_MOUNT),
                         ram.u8(rd + 0x0F), ram.u8(rd + 0x44)))
    b.check(samples and agree == samples,
            "in every frame dump, every live rider's +0x25C is its state's category remapped",
            "%d of %d" % (agree, samples))
    b.check(samples and implied == samples,
            "a rider appears in the broad-phase grid only when its +0x25C >= 2",
            "%d of %d" % (implied, samples))
    b.check(chain_n and chain_ok == chain_n,
            "every chain node of every frame dump names an entity with a point within one "
            "cell of that cell (the dumps are taken later in the frame than the savestates)",
            "%d of %d" % (chain_ok, chain_n))
    if len(timeline) == 14:
        mounts = [t[1] for t in timeline]
        b.check(mounts == sorted(mounts),
                "the player's mount state never goes backwards across the capture",
                str(mounts))
        b.check(mounts[0] == 1 and 3 in mounts and mounts[-1] == 4,
                "... and it runs 1 (riding) -> 3 (tumbling) -> 4 (down)", str(mounts))
        first = next(i for i, m in enumerate(mounts) if m != 1)
        b.check(timeline[first - 1][2] > 0 and timeline[first][2] == 0,
                "the rider's health riderDef+0x0F reaches 0 on exactly the dump the mount "
                "state leaves 1", "%d -> %d" % (timeline[first - 1][2], timeline[first][2]))
        b.check(all((t[3] & 0x40) == 0 for t in timeline[:first])
                and all((t[3] & 0x40) != 0 for t in timeline[first:]),
                "... and riderDef+0x44 bit 0x40 latches on the same dump and stays set")

    print("== DATA\\FIGHT.BIN")
    raw = fight_file()
    recs = fight_records(raw)
    b.check(len(raw) == 4200, "the file is 4200 bytes", "%d" % len(raw))
    b.check(all(0x1E0 <= r["blockA"] < len(raw) and 0x1E0 <= r["blockB"] < len(raw)
                for r in recs),
            "every block offset of all 40 records points past the 0x1E0-byte record table")
    b.check(all(r["blockA"] + 12 * r["countA"] <= len(raw)
                and r["blockB"] + 12 * r["countB"] <= len(raw) for r in recs),
            "every node and edge block lies inside the file")
    b.check(all(r["idle"] < RIDER_STATE_COUNT for r in recs),
            "every idle stance is a valid rider-state id")
    bad = []
    for r in recs:
        for n in fight_nodes(raw, r):
            for sid in n[:4]:
                if sid >= RIDER_STATE_COUNT:
                    bad.append((r["index"], sid))
    b.check(not bad, "every animStart / animEnd / animHit / animMiss is a valid state id",
            str(bad[:4]))
    tbl2 = rider_state_table()
    onbike = [r["index"] for r in recs
              if all(mount_of_category(tbl2[n[0]][1], cfg) == 1 for n in fight_nodes(raw, r))]
    b.check(len(onbike) == len(recs),
            "every striking node of every record plays a state that keeps the rider mounted",
            "%d of %d" % (len(onbike), len(recs)))
    edges = [e for r in recs for e in fight_edges(raw, r)]
    b.check(all(0 <= t0 <= t1 for t0, t1, _ in edges),
            "every input window is a non-negative, non-inverted interval")
    bad_edge = []
    for r in recs:
        for t0, t1, p in fight_edges(raw, r):
            node, alt = (p >> 20) & 0x3F, (p >> 26) & 0x3F
            if node != 63 and node >= r["countB"]:
                bad_edge.append((r["index"], node, r["countB"]))
            if node == 63 and alt >= r["countB"]:
                bad_edge.append((r["index"], "alt", alt, r["countB"]))
    b.check(not bad_edge,
            "every edge names a node inside its own record's block, or 63 with a real "
            "alternative (3 of the 148 edges do that)", str(bad_edge[:4]))

    fails = len(b.bad)
    print()
    print("collide: %d checks, %d failures" % (b.n, fails))
    if fails:
        for w in b.bad:
            print("  - %s" % w)
    sys.exit(1 if fails else 0)


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        prog="collide.py", description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("info", help="the decoded skeleton (addresses only)")
    s.set_defaults(func=cmd_info)

    s = sub.add_parser("grid", help="the live broad-phase grid of each capture")
    s.set_defaults(func=cmd_grid)

    s = sub.add_parser("fight", help="DATA\\FIGHT.BIN structure and the move census")
    s.set_defaults(func=cmd_fight)

    s = sub.add_parser("verify", help="the assertion bench (the gate)")
    s.add_argument("--mutate", action="store_true",
                   help="perturb four documented claims so the probe can be seen failing")
    s.set_defaults(func=cmd_verify)
    return ap


def main() -> None:
    args = build_parser().parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
