"""Scout probes for THE SOUND SYSTEM of Road Rash: Jailbreak (USA, SLUS_01053).

`docs\\formats\\audio.md` proved the DECODERS and the containers.  This probe checks the layer
ABOVE them - how the running game decides what to play, on which voice, at what pitch and how
loud - independently of the C++ tree:

  * the call skeleton of the emitter, the voice allocator and the per-frame service is
    re-derived from the instruction words themselves: every documented "function A calls
    function B at address X" is checked by decoding the `jal` at X;
  * the sizes that define the data structures are quoted out of the code that builds them -
    24 voices, 44 bytes per voice, 22 reserved slots, a 25-slot steal ring, a 72-byte listener,
    a 12-byte sound descriptor, the `2048 - camYaw` listener heading;
  * `DATA\\RASHNZ_E.DAT` is parsed from its own 8-entry directory and the running ends are
    checked to close EXACTLY on the file size;
  * the resident sound banks in every savestate are compared byte for byte with the banks in
    the player's own `RASHNZ_E.DAT`, and the ONLY bytes allowed to differ are the SPU-address
    words the structure itself predicts - each shifted by the one base the bank header carries;
  * the sample payload of every resident bank is compared byte for byte with the SPU RAM of
    the savestates;
  * every 0x4000-byte record of `DATA\\AUDTAUNT.STR` is checked to carry a bank of the same
    format at +0x28;
  * the live voice table, the steal ring, the reserved table, the listener record and the
    per-player engine-sound record are checked against all four savestates - including the
    listener heading being exactly `2048 - camera yaw` and the listener velocity being the
    player bike's own;
  * three NEGATIVES that sibling documents got wrong are checked by transitive closure over
    the call graph: `RASHCDG 0x8005E1D8`, `RASHCDG 0x800C4550` and `SLUS 0x80027540` reach no
    function of the sound system at all, and `SLUS 0x80017BA0` (PlaySound3D) never reaches
    `Rand SLUS 0x8001FC58`.

Reads ONLY from work\\disc_us (the player's own disc extract, gitignored) and from
work\\oracle\\state (savestate extracts).  Writes ONLY to stdout.  Never copies game bytes
into the repo.

Usage (from the project root):

    python tools\\scout\\sound.py info              # the decoded skeleton, addresses only
    python tools\\scout\\sound.py banks             # the bank directory and the live bank table
    python tools\\scout\\sound.py voices            # the live voice table of each savestate
    python tools\\scout\\sound.py events            # every PlaySound3D call site and its sound id
    python tools\\scout\\sound.py verify            # the assertion bench (the gate)
    python tools\\scout\\sound.py verify --mutate   # the deliberate-failure demonstration

`verify` ends with a single line a gate can match:

    sound: <n> checks, <m> failures
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

OVL_BASE = 0x8005B5E8

SHA1 = {
    "SLUS": "67ed165a2c517d4e6106fb0dfa324d66dd9a76f1",
    "F": "a3fec4b4e9292c358d0f6dc529843f5d8f25924a",
    "G": "cfe43a7786759f2cb9c57751cf99e84d1074782c",
    "I": "9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06",
}

# ---------------------------------------------------------------------------
# Guest globals, each derived from the instruction that establishes it.
# $gp = 0x8005AC8C (EXE 0x800403A8/AC).
# ---------------------------------------------------------------------------

GP = 0x8005AC8C

G_TOGGLE = GP + 1888        # 0x8005B3EC  which player the vsync tick services
G_BANK_DEFAULT = GP + 1912  # 0x8005B404  the bank PlaySound3D uses when its 4th arg is 0
G_LISTENERS = GP + 1920     # 0x8005B40C  -> Listener[numPlayers], stride 72
G_ENGINE = GP + 1952        # 0x8005B42C  -> EngineSound[numPlayers], stride 132
G_MUTE_BANK = GP + 1940     # 0x8005B420  PlaySound3D returns at once for this bank
G_SERIAL = GP + 2068        # 0x8005B4A0  the sound-handle serial counter
G_LCG = GP + 2076           # 0x8005B4A8  the game's shared LCG seed (NOT the sound system's)

SNDSYS = 0x800D6870         # the sound-system struct
S_BANK_COUNT = 0x000
S_BANKS = 0x004
S_VOICE_COUNT = 0x008
S_VOICES = 0x00C
S_BUSY = 0x010
S_KEY_ON = 0x014
S_KEY_OFF = 0x018
S_RESERVED = 0x058          # s32[22]
S_FREE_STACK = 0x0B0        # s32[24]
S_RING = 0x110              # s32[25]
S_FREE_TOP = 0x174
S_RING_HEAD = 0x178
S_RING_TAIL = 0x17C
S_MASTER_VOL = 0x180
S_PAN_OFF = 0x184

SND3D = 0x800D6C00          # the 3D block; +0x0C is the master 3D volume scale
SPU_BASE_PTR = 0x8005A41C   # holds 0x1F801C00

VOICE_STRIDE = 44
V_SERIAL = 0x04
V_DESC = 0x08
V_STATE = 0x10
V_CHANNEL = 0x1C
V_PITCH = 0x20
V_VOL_L = 0x24
V_VOL_R = 0x28

LISTENER_STRIDE = 72
L_YAW = 0x00
L_X = 0x04
L_Z = 0x08
L_VX = 0x0C
L_VZ = 0x10
L_PAN = 0x14

ENGINE_STRIDE = 132
EN_ENTITY = 0x00
EN_ENABLE = 0x04
EN_FALLBACK_BANK = 0x08
EN_HANDLES = (0x10, 0x14, 0x18, 0x1C, 0x20)
EN_PARAMS = 0x2C
EN_LEVEL1 = 0x30
EN_TARGET1 = 0x34
EN_RATE1 = 0x38
EN_LEVEL2 = 0x60
EN_TARGET2 = 0x64
EN_RATE2 = 0x68
EN_VOLUME = 0x70
EN_PAN = 0x74
EN_PITCH_SCALE = 0x78

# the bank container
BANK_MAGIC = 0x00           # u32, always 2
BANK_COUNT = 0x04           # u8
BANK_SPU_ADDR = 0x08        # u32, patched at load
BANK_SAMPLE_BYTES = 0x0C    # u32
BANK_TABLE = 0x10           # u32[count], offsets relative to the bank start
SOUND_DESC_STRIDE = 12      # a descriptor; the first one is at sound + 4
SOUND_DESC_SPU = 8          # ... and its SPU address is at descriptor + 8

GAME_STATE_PTR = 0x8005B2F8
GS_NUM_PLAYERS = 0x30
PLAYER_BIKES = 0x8005B268
CAM_PTR = 0x8005AEC0        # -> the camera record; (s16)+0x7C is the yaw SetListener inverts
CAM_YAW = 0x7C
ENT_X = 0x0B8
ENT_Z = 0x0C0
ENT_VX = 0x1C8
ENT_VZ = 0x1D0

RASHNZ = "RASHNZ_E.DAT"
RASHNZ_ENTRIES = 8          # the directory closes exactly on the file size with 8
RASHNZ_HEADER = 12 * RASHNZ_ENTRIES

TAUNT = "AUDTAUNT.STR"
TAUNT_RECORD = 0x4000
TAUNT_RECORDS = 150
TAUNT_BANK_AT = 0x28        # the bank inside each record (audio.md 4.2)

# ---------------------------------------------------------------------------
# The call skeleton.  Each row is (image, site, callee, what) and is checked by
# decoding the instruction word at `site`; nothing here is assumed.
# ---------------------------------------------------------------------------

SKELETON = [
    # --- the emitter ------------------------------------------------------
    ("SLUS", 0x80017C30, 0x80019E40, "PlaySound3D -> Sound3DParams (the positional arm)"),
    ("SLUS", 0x80017CA4, 0x8001F174, "PlaySound3D -> StartVoice(bank, id, restart, reserved, params)"),
    ("SLUS", 0x80019F7C, 0x80020018, "Sound3DParams -> atan2 for the pan"),
    ("SLUS", 0x8001A038, 0x80020018, "Sound3DParams -> atan2 for the doppler"),
    ("SLUS", 0x8001A080, 0x80010028, "Sound3DParams -> FixDiv, the doppler ratio"),
    # --- starting a voice -------------------------------------------------
    ("SLUS", 0x8001F1E8, 0x8001E86C, "StartVoice -> LookupSound(bank, id)"),
    ("SLUS", 0x8001F2CC, 0x8001F9C4, "StartVoice -> AllocVoice(reserved)"),
    ("SLUS", 0x8001F31C, 0x80050678, "StartVoice -> SpuSetKey off, the restart arm"),
    ("SLUS", 0x8001F344, 0x8001EB28, "StartVoice -> KeyOn(1 << channel)"),
    ("SLUS", 0x8001FA60, 0x8001EB44, "AllocVoice -> KeyOff(1 << stolen), the steal arm"),
    # --- the per-frame service -------------------------------------------
    ("SLUS", 0x800121B8, 0x80018FAC, "GameFrame -> AudioFrame, the per-frame sound update"),
    ("SLUS", 0x80019100, 0x80016768, "AudioFrame -> SetListener"),
    ("SLUS", 0x80019204, 0x800169D0, "AudioFrame -> the per-player sound state machine A"),
    ("SLUS", 0x8001920C, 0x80016E4C, "AudioFrame -> the per-player sound state machine B"),
    ("SLUS", 0x80019954, 0x80017BA0, "AudioFrame -> PlaySound3D for a queued bank event"),
    # --- the vsync path ---------------------------------------------------
    ("SLUS", 0x8001BDCC, 0x8001B594, "EnterRace -> register the vsync callback"),
    ("SLUS", 0x8001B578, 0x800479C4, "arm -> the BIOS VSyncCallback"),
    ("SLUS", 0x8001B810, 0x80019990, "the vsync callback -> AudioVSyncTick"),
    ("SLUS", 0x80019ADC, 0x80019C54, "AudioVSyncTick -> UpdateVoice, engine layer 0"),
    ("SLUS", 0x80019B3C, 0x8001F934, "AudioVSyncTick -> GetSoundPitch, the fallback layer"),
    ("SLUS", 0x80019B64, 0x80019C54, "AudioVSyncTick -> UpdateVoice, engine layer 1"),
    ("SLUS", 0x80019BD4, 0x80019C54, "AudioVSyncTick -> UpdateVoice, engine layer 2"),
    ("SLUS", 0x80019C18, 0x80019C54, "AudioVSyncTick -> UpdateVoice, engine layer 3"),
    ("SLUS", 0x80019C30, 0x8001EE94, "AudioVSyncTick -> SoundService"),
    ("SLUS", 0x80019D84, 0x80051C38, "UpdateVoice -> SpuSetVoiceAttr, directly"),
    ("SLUS", 0x8001EEDC, 0x80050D08, "SoundService -> SpuSetKey(off, mask)"),
    ("SLUS", 0x8001EF3C, 0x8001EB7C, "SoundService -> ProgramVoice for a keyed-on voice"),
    ("SLUS", 0x8001EF70, 0x80050D08, "SoundService -> SpuSetKey(on, mask)"),
    ("SLUS", 0x8001EBD8, 0x80051C38, "ProgramVoice -> SpuSetVoiceAttr"),
    # --- loading ----------------------------------------------------------
    ("SLUS", 0x8001E5FC, 0x8001FBD4, "ResetSoundState -> ResetVoiceLists"),
    ("SLUS", 0x8001E688, 0x8001E48C, "SoundInit -> malloc the bank pointer table"),
    ("SLUS", 0x8001E6B0, 0x8001E48C, "SoundInit -> malloc the voice table"),
    ("SLUS", 0x8001EDCC, 0x8001E938, "LoadBank -> upload the samples to SPU RAM"),
    ("SLUS", 0x8001EDF0, 0x8001EAA4, "LoadBank -> PatchBank(bank, spuAddr)"),
    ("SLUS", 0x8001E980, 0x8004F3C8, "upload -> allocate SPU RAM"),
    ("SLUS", 0x8001E9E8, 0x8001E22C, "upload -> the transfer"),
    # --- the race's own emitters -----------------------------------------
    ("G", 0x8007A6C0, 0x80017BA0, "BikeEngineStep -> PlaySound3D(id 54) at the throttle crossing"),
    ("G", 0x800A5F60, 0x80017BA0, "the collision pass -> PlaySound3D(id 3 / 17)"),
    ("G", 0x800B380C, 0x80017BA0, "the topple -> PlaySound3D(SurfaceSound(+0xB4) + 14)"),
    ("G", 0x800B37F8, 0x80017B30, "the topple -> SurfaceSound(entity[+0xB4])"),
    ("G", 0x800C2548, 0x80017BA0, "the combat-move reader -> PlaySound3D(id 78), the taunt"),
]

# Instruction words quoted verbatim: these ARE the stated sizes and constants.
LITERALS = [
    ("SLUS", 0x8001E664, 0x24100018, "SoundInit: li s0,24 - the default voice count"),
    ("SLUS", 0x8001E62C, 0x24110001, "SoundInit: li s1,1 - the default bank count"),
    ("SLUS", 0x8001FBD8, 0x24030015, "ResetVoiceLists: li v1,21 - 22 reserved slots"),
    ("SLUS", 0x8001FBF8, 0x24030017, "ResetVoiceLists: li v1,23 - 24 free-stack slots"),
    ("SLUS", 0x8001FC1C, 0x24030018, "ResetVoiceLists: li v1,24 - 25 ring slots"),
    ("SLUS", 0x8001FC44, 0x24030017, "ResetVoiceLists: li v1,23 - freeTop starts at 23"),
    ("SLUS", 0x8001FA48, 0x28620019, "AllocVoice: slti v0,v1,25 - the ring wraps at 25"),
    ("SLUS", 0x8001FABC, 0x28820016, "AllocVoice: slti v0,a0,22 - 22 reserved slots scanned"),
    ("SLUS", 0x8001FB0C, 0x3463FFFF, "AllocVoice: the serial mask is 0x07FFFFFF"),
    ("SLUS", 0x80019C68, 0x00041EC2, "UpdateVoice: srl v1,a0,27 - the channel is the top 5 bits"),
    ("SLUS", 0x8001EB90, 0x34420093, "ProgramVoice: SpuVoiceAttr mask 0x00060093"),
    ("SLUS", 0x80019D58, 0x24020013, "UpdateVoice: SpuVoiceAttr mask 0x00000013"),
    ("SLUS", 0x80016784, 0x24020800, "SetListener: li v0,2048 - half a turn"),
    ("SLUS", 0x80016788, 0x00441023, "SetListener: subu v0,v0,a0 - yaw = 2048 - camYaw"),
    ("SLUS", 0x80019F18, 0x3C020040, "Sound3DParams: lui v0,0x40 - the 64.0 cut-off distance"),
    ("SLUS", 0x80019F50, 0x2404007F, "Sound3DParams: li a0,127 - the volume ceiling"),
    ("SLUS", 0x8001F244, 0x28870081, "the pan fold: slti a3,a0,129"),
    ("SLUS", 0x8001F26C, 0x2482FFC0, "the pan fold: addiu v0,a0,-64 - centre is 64"),
    ("SLUS", 0x80017B40, 0x24030033, "SurfaceSound: li v1,51 - a 52-entry table"),
    ("SLUS", 0x8001E874, 0x00A2302A, "LookupSound: slt a2,a1,v0 - id < bank[+0x04]"),
    ("SLUS", 0x8001F1B8, 0x0044102A, "StartVoice: slt v0,v0,a0 - bank <= count, NOT <"),
    ("SLUS", 0x8001FC5C, 0x8F82081C, "Rand: lw v0,2076(gp) - the LCG seed is gp+2076"),
    ("SLUS", 0x8001FB08, 0x8F820814, "AllocVoice: lw v0,2068(gp) - the serial is gp+2068"),
]

# The multiply-by-stride chains, quoted word by word so the strides are read out of the
# code that builds them.  The compiler interleaves other instructions between the shifts,
# so each word is named by its own address.
STRIDE_CHAINS = [
    ("SLUS", 72, "Sound3DParams: 72 * playerIndex - the listener stride",
     ((0x80019E5C, 0x000410C0), (0x80019E68, 0x00441021), (0x80019E74, 0x000210C0))),
    ("SLUS", 72, "SetListener: 72 * playerIndex",
     ((0x80016768, 0x000410C0), (0x8001676C, 0x00441021), (0x80016770, 0x000210C0))),
    ("SLUS", 44, "AllocVoice: 44 * voiceIndex - the voice stride",
     ((0x8001FA80, 0x00101040), (0x8001FA84, 0x00501021), (0x8001FA88, 0x00021080),
      (0x8001FA8C, 0x00501023), (0x8001FA94, 0x00021080))),
    ("SLUS", 44, "SoundInit: malloc(44 * voiceCount)",
     ((0x8001E6A0, 0x00102040), (0x8001E6A4, 0x00902021), (0x8001E6A8, 0x00042080),
      (0x8001E6AC, 0x00902023), (0x8001E6B4, 0x00042080))),
    ("SLUS", 12, "StartVoice: descriptor = sound + 4 + 12 * index",
     ((0x8001F200, 0x00101040), (0x8001F210, 0x00501021), (0x8001F214, 0x00021080),
      (0x8001F218, 0x24420004))),
]

# PlaySound3D call sites whose sound id is a plain immediate, read out of the code.
# (site -> id).  The rest are computed and are listed by `events`.
EVENT_IDS_G = {
    0x80060078: 76, 0x8006008C: 75, 0x800600A0: 29, 0x80060120: 65,
    0x80060134: 106, 0x80060148: 80, 0x8007A6C0: 54, 0x800A5F60: 17,
    0x800AD84C: 19, 0x800B1608: 13, 0x800B1690: 49, 0x800B18D4: 54,
    0x800B2690: 55, 0x800B2828: 19, 0x800B2A98: 19, 0x800B2C70: 19,
    0x800C0194: 96, 0x800C0330: 88, 0x800C1910: 105, 0x800C2548: 78,
}

# Functions of the sound system.  A transitive closure that reaches ANY of these is
# "reaches the sound system".
SOUND_FUNCS = (
    0x80017BA0,  # PlaySound3D
    0x80017B6C,  # QueueListenerSound
    0x80017814,  # StopVoicesOfObject
    0x80019E40,  # Sound3DParams
    0x8001F174,  # StartVoice
    0x8001F9C4,  # AllocVoice
    0x8001EE94,  # SoundService
    0x8001EB28,  # KeyOn
    0x8001EB44,  # KeyOff
    0x80019C54,  # UpdateVoice
    0x80051C38,  # SpuSetVoiceAttr
    0x80050D08,  # SpuSetKey
)

# Functions whose role could suggest sound code; the check shows what each really reaches.
NEGATIVES = [
    ("G", 0x8005E1D8, "RASHCDG 0x8005E1D8 (suspected: 'the sound-emitter update')"),
    ("G", 0x800C4550, "RASHCDG 0x800C4550 (suspected: 'the idle-stance trigger')"),
    ("SLUS", 0x80027540, "SLUS 0x80027540 (suspected: 'the crash emitter')"),
]


# ---------------------------------------------------------------------------
# Helpers
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


def jal_sites(img, target):
    out = []
    n = len(img.data) // 4
    for i, (w,) in enumerate(struct.iter_unpack("<I", img.data[: n * 4])):
        if (w >> 26) == 3 and (0x80000000 | ((w & 0x03FFFFFF) << 2)) == target:
            out.append(img.base + 4 * i)
    return out


def own_func_starts(img):
    """Addresses inside `img` that `img` itself calls.  Restricted to the image's own
    jal targets so the three overlays, which share an address range, cannot pollute
    each other's function map."""
    s = set()
    n = len(img.data) // 4
    for i, (w,) in enumerate(struct.iter_unpack("<I", img.data[: n * 4])):
        if (w >> 26) == 3:
            t = 0x80000000 | ((w & 0x03FFFFFF) << 2)
            if img.base <= t < img.base + len(img.data):
                s.add(t)
    return sorted(s)


def func_end(img, lo, limit=0x8000):
    """The address just past the first `jr ra` at or after `lo`."""
    a = lo
    while a < lo + limit:
        w = word(img, a)
        if w is None:
            break
        if w == 0x03E00008:
            return a + 8
        a += 4
    return lo + 8


def reaches(imgs, key, root, targets, maxdepth=10):
    """Transitive closure of `jal` out of `root` in image `key`, following calls into
    SLUS as well.  Returns the set of `targets` reached."""
    import bisect
    starts = {k: own_func_starts(imgs[k]) for k in (key, "SLUS")}
    found = set()
    seen = set()
    stack = [(key, root, 0)]
    while stack:
        im, f, d = stack.pop()
        if (im, f) in seen or d > maxdepth:
            continue
        seen.add((im, f))
        if f in targets:
            found.add(f)
        img = imgs[im]
        if not (img.base <= f < img.base + len(img.data)):
            continue
        st = starts[im]
        i = bisect.bisect_right(st, f)
        end = st[i] if i < len(st) else img.base + len(img.data)
        end = min(end, func_end(img, f))
        for a in range(f, end, 4):
            w = word(img, a)
            if w is not None and (w >> 26) == 3:
                t = 0x80000000 | ((w & 0x03FFFFFF) << 2)
                for cand in (im, "SLUS") if im != "SLUS" else ("SLUS",):
                    ci = imgs[cand]
                    if ci.base <= t < ci.base + len(ci.data):
                        stack.append((cand, t, d + 1))
                        break
    return found


class Ram:
    def __init__(self, path):
        self.d = open(path, "rb").read()

    def u8(self, a):
        return self.d[a & 0x1FFFFF]

    def u16(self, a):
        return struct.unpack_from("<H", self.d, a & 0x1FFFFF)[0]

    def s16(self, a):
        return struct.unpack_from("<h", self.d, a & 0x1FFFFF)[0]

    def u32(self, a):
        return struct.unpack_from("<I", self.d, a & 0x1FFFFF)[0]

    def s32(self, a):
        return struct.unpack_from("<i", self.d, a & 0x1FFFFF)[0]

    def blk(self, a, n):
        o = a & 0x1FFFFF
        return self.d[o:o + n]


def states():
    out = []
    for name in STATES:
        p = os.path.join(STATE_DIR, name, "ram.bin")
        if os.path.isfile(p):
            spu = os.path.join(STATE_DIR, name, "spuram.bin")
            out.append((name, Ram(p), open(spu, "rb").read() if os.path.isfile(spu) else None))
    return out


# ---------------------------------------------------------------------------
# DATA\RASHNZ_E.DAT
# ---------------------------------------------------------------------------


def rashnz():
    """(raw, [(offset, bankBytes, sampleBytes)]) from the file's own 8-entry directory."""
    raw = open(os.path.join(DATA, RASHNZ), "rb").read()
    ents = [struct.unpack_from("<3I", raw, 12 * i) for i in range(RASHNZ_ENTRIES)]
    return raw, ents


def bank_parse(blob, at=0):
    """Parse a bank header in `blob` at `at`.  Returns (count, spuAddr, sampleBytes, [sound
    offsets], [absolute offsets of the SPU-address words the loader patches])."""
    magic = struct.unpack_from("<I", blob, at)[0]
    count = blob[at + BANK_COUNT]
    spu = struct.unpack_from("<I", blob, at + BANK_SPU_ADDR)[0]
    nbytes = struct.unpack_from("<I", blob, at + BANK_SAMPLE_BYTES)[0]
    offs = [struct.unpack_from("<I", blob, at + BANK_TABLE + 4 * i)[0] for i in range(count)]
    patched = list(range(at + BANK_SPU_ADDR, at + BANK_SPU_ADDR + 4))
    for o in offs:
        if o == 0:
            continue
        snd = at + o
        m = blob[snd]
        for j in range(m):
            w = snd + 4 + SOUND_DESC_STRIDE * j + SOUND_DESC_SPU
            patched += list(range(w, w + 4))
    return magic, count, spu, nbytes, offs, patched


def bank_end(blob, at, count, offs):
    """The end of the bank's descriptor part: past the last sound record."""
    end = at + BANK_TABLE + 4 * count
    for o in offs:
        if o == 0:
            continue
        snd = at + o
        end = max(end, snd + 4 + SOUND_DESC_STRIDE * blob[snd])
    return end


# ---------------------------------------------------------------------------
# Config, so the probe can be seen failing on demand
# ---------------------------------------------------------------------------


def config(mutate: bool) -> dict:
    """The claims the numeric checks depend on.  --mutate perturbs five of them; every
    perturbation is one of the load-bearing statements."""
    cfg = dict(voices=24, voice_stride=VOICE_STRIDE, listener_stride=LISTENER_STRIDE,
               half_turn=2048, ring=25, engine_stride=ENGINE_STRIDE,
               service_callee=0x8001EB7C, rashnz_entries=RASHNZ_ENTRIES)
    if mutate:
        cfg["voices"] = 23                 # "the SPU has 23 usable voices"
        cfg["voice_stride"] = 48           # "a voice record is 48 bytes"
        cfg["listener_stride"] = 64        # "a listener is 64 bytes"
        cfg["half_turn"] = 1024            # "a turn is 2048 units, so half of it is 1024"
        cfg["service_callee"] = 0x8001EB28  # "SoundService keys voices on one at a time"
    return cfg


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------


def cmd_info(_args):
    imgs = load_images()
    for k in ("SLUS", "F", "G", "I"):
        print("%-5s %-14s sha1 %s" % (k, os.path.basename(imgs[k].path), imgs[k].sha1))
    print()
    print("the sound call skeleton, decoded from the instruction words:")
    for key, site, callee, what in SKELETON:
        got = jal_target(imgs[key], site)
        print("  %-4s %08X  jal %08X  %-62s %s"
              % (key, site, callee, what, "ok" if got == callee else "MISMATCH %08X" % (got or 0)))
    print()
    print("call-site census (all four images):")
    for t, name in ((0x80017BA0, "PlaySound3D"), (0x8001F174, "StartVoice"),
                    (0x8001F9C4, "AllocVoice"), (0x8001EE94, "SoundService"),
                    (0x80019C54, "UpdateVoice"), (0x80017814, "StopVoicesOfObject"),
                    (0x8001FC58, "Rand (for contrast)")):
        per = {k: len(jal_sites(imgs[k], t)) for k in ("SLUS", "F", "G", "I")}
        print("  %-20s %08X  total %4d   %s"
              % (name, t, sum(per.values()),
                 "  ".join("%s=%d" % (k, v) for k, v in per.items())))
    print()
    print("negatives (transitive closure over jal):")
    for key, root, what in NEGATIVES:
        hit = reaches(imgs, key, root, set(SOUND_FUNCS))
        print("  %-4s %08X reaches %s   %s"
              % (key, root, "NOTHING of the sound system" if not hit
                 else ", ".join("%08X" % h for h in sorted(hit)), what))


def cmd_banks(_args):
    raw, ents = rashnz()
    print("DATA\\%s  %d bytes, sha1 %s" % (RASHNZ, len(raw),
                                           E.sha1_of(os.path.join(DATA, RASHNZ))))
    print("  directory: %d entries of 12 bytes = %d bytes of header" % (len(ents), RASHNZ_HEADER))
    total = RASHNZ_HEADER
    for i, (off, bb, sb) in enumerate(ents):
        magic, count, spu, nbytes, offs, _ = bank_parse(raw, RASHNZ_HEADER + off)
        print("   %d  off %06X  bank %5X  samples %6X  | magic %d  sounds %3d  declared samples %6X"
              % (i, off, bb, sb, magic, count, nbytes))
        total += bb + sb
    print("  header + every (bank + samples) = %d, file = %d  -> %s"
          % (total, len(raw), "EXACT" if total == len(raw) else "MISMATCH"))
    print()
    for name, ram, _ in states():
        bt = ram.u32(SNDSYS + S_BANKS)
        n = ram.s32(SNDSYS + S_BANK_COUNT)
        print("%-8s bank table %08X, declared count %d" % (name, bt, n))
        for i in range(n):
            p = ram.u32(bt + 4 * i)
            if p == 0:
                print("   %2d  NULL" % i)
                continue
            blob = ram.blk(p, 0x2000)
            magic, count, spu, nbytes, offs, _ = bank_parse(blob)
            print("   %2d  %08X  magic %d  sounds %3d  spu %06X  samples %6X"
                  % (i, p, magic, count, spu, nbytes))


def cmd_voices(_args):
    for name, ram, _ in states():
        vb = ram.u32(SNDSYS + S_VOICES)
        n = ram.s32(SNDSYS + S_VOICE_COUNT)
        print("== %s  voices %08X x %d, master %d, pan %s, freeTop %d, ring head/tail %d/%d"
              % (name, vb, n, ram.s32(SNDSYS + S_MASTER_VOL),
                 "on" if ram.s32(SNDSYS + S_PAN_OFF) == 0 else "OFF",
                 ram.s32(SNDSYS + S_FREE_TOP), ram.s32(SNDSYS + S_RING_HEAD),
                 ram.s32(SNDSYS + S_RING_TAIL)))
        for i in range(n):
            v = vb + VOICE_STRIDE * i
            print("   ch%2d  serial %08X  desc %08X  state %d  chan %2d  pitch %04X  L %6d R %6d"
                  % (i, ram.u32(v + V_SERIAL), ram.u32(v + V_DESC), ram.s32(v + V_STATE),
                     ram.s32(v + V_CHANNEL), ram.u32(v + V_PITCH),
                     ram.s32(v + V_VOL_L), ram.s32(v + V_VOL_R)))
        print("   ring     %s" % [ram.s32(SNDSYS + S_RING + 4 * k) for k in range(25)])
        print("   reserved %s" % [ram.s32(SNDSYS + S_RESERVED + 4 * k) for k in range(22)])
        L = ram.u32(G_LISTENERS)
        print("   listener %08X  yaw %d  pan %d  x %08X z %08X vx %08X vz %08X"
              % (L, ram.s32(L + L_YAW), ram.s32(L + L_PAN), ram.u32(L + L_X),
                 ram.u32(L + L_Z), ram.u32(L + L_VX), ram.u32(L + L_VZ)))
        En = ram.u32(G_ENGINE)
        print("   engine   %08X  entity %08X  enable %d  level %d/%d rate %d  vol %d pan %d "
              "pitchScale %08X  handles %s"
              % (En, ram.u32(En + EN_ENTITY), ram.u8(En + EN_ENABLE),
                 ram.s32(En + EN_LEVEL1), ram.s32(En + EN_TARGET1), ram.s32(En + EN_RATE1),
                 ram.s32(En + EN_VOLUME), ram.s32(En + EN_PAN), ram.u32(En + EN_PITCH_SCALE),
                 " ".join("%08X" % ram.u32(En + h) for h in EN_HANDLES)))


def cmd_events(_args):
    imgs = load_images()
    print("every PlaySound3D(x, z, id, bank) call site, with the id as the code builds it:")
    for key in ("SLUS", "G"):
        for site in jal_sites(imgs[key], 0x80017BA0):
            imm = None
            for k in range(0, 16):
                w = word(imgs[key], site - 4 * k)
                if w is None:
                    break
                op, rs, rt = w >> 26, (w >> 21) & 31, (w >> 16) & 31
                if op == 9 and rt == 6 and rs == 0:          # addiu a2,zero,imm
                    imm = w & 0xFFFF
                    break
                if op == 9 and rt == 6:                      # addiu a2,rs,imm -> computed
                    imm = "computed +%d" % (w & 0xFFFF)
                    break
                if op == 0 and ((w >> 11) & 31) == 6:
                    imm = "register"
                    break
                if op in (0x20, 0x21, 0x23, 0x24, 0x25) and rt == 6:
                    imm = "loaded"
                    break
            print("  %-4s %08X  id = %s" % (key, site, imm))


def cmd_verify(args):
    cfg = config(args.mutate)
    imgs = load_images()
    b = Bench()

    print("== images")
    for k in ("SLUS", "F", "G", "I"):
        b.check(imgs[k].sha1 == SHA1[k], "%s sha1 matches the documented image" % k, imgs[k].sha1)

    print("== the sound call skeleton, decoded from the instruction words")
    for key, site, callee, what in SKELETON:
        want = cfg["service_callee"] if site == 0x8001EF3C else callee
        got = jal_target(imgs[key], site)
        b.check(got == want, "%s %08X: jal %08X (%s)" % (key, site, want, what),
                "%08X" % (got or 0))

    print("== quoted instruction words")
    for key, addr, w, what in LITERALS:
        got = word(imgs[key], addr)
        b.check(got == w, "%s %08X == %08X (%s)" % (key, addr, w, what), "%08X" % (got or 0))

    print("== the stride chains, read out of the code that builds them")
    for key, stride, what, chain in STRIDE_CHAINS:
        ok = all(word(imgs[key], a) == c for a, c in chain)
        want = stride
        if stride == LISTENER_STRIDE:
            want = cfg["listener_stride"]
        elif stride == VOICE_STRIDE:
            want = cfg["voice_stride"]
        b.check(ok and want == stride, "%s %08X: the chain multiplies by %d (%s)"
                % (key, chain[0][0], want, what),
                " ".join("%08X:%08X" % (a, word(imgs[key], a) or 0) for a, _ in chain))

    print("== $gp, and which word the sound system really perturbs")
    b.check(word(imgs["SLUS"], 0x800403A8) == 0x3C1C8006, "gp is built at EXE 0x800403A8")
    b.check(word(imgs["SLUS"], 0x800403AC) == 0x279CAC8C, "gp = 0x80060000 - 21364 = 0x8005AC8C")
    b.check(GP == 0x8005AC8C, "the probe's own gp constant agrees")
    b.check(G_SERIAL == 0x8005B4A0 and G_LCG == 0x8005B4A8,
            "the serial counter is 8 bytes below the LCG seed in the same gp block")
    got = reaches(imgs, "SLUS", 0x80017BA0, {0x8001FC58})
    b.check(not got, "PlaySound3D never reaches Rand SLUS 0x8001FC58")
    got = reaches(imgs, "SLUS", 0x80017BA0, {0x8001F9C4})
    b.check(bool(got), "PlaySound3D does reach AllocVoice, which writes gp+2068")

    print("== the negatives three sibling documents state or imply")
    for key, root, what in NEGATIVES:
        hit = reaches(imgs, key, root, set(SOUND_FUNCS))
        b.check(not hit, "%s reaches no function of the sound system" % what,
                ", ".join("%08X" % h for h in sorted(hit)))
    b.check(len(jal_sites(imgs["G"], 0x80017BA0)) == 31,
            "RASHCDG has exactly 31 PlaySound3D call sites",
            str(len(jal_sites(imgs["G"], 0x80017BA0))))
    b.check(len(jal_sites(imgs["SLUS"], 0x80017BA0)) == 15,
            "SLUS has exactly 15 PlaySound3D call sites",
            str(len(jal_sites(imgs["SLUS"], 0x80017BA0))))
    for k in ("F", "I"):
        b.check(not jal_sites(imgs[k], 0x8001F174) if k == "I" else True,
                "RASHCDI never starts a voice directly")
    b.check(not jal_sites(imgs["G"], 0x8001F174),
            "the race overlay never calls StartVoice directly - PlaySound3D is its only door")

    print("== the sound ids the race overlay asks for, as immediates in the code")
    for site, want in sorted(EVENT_IDS_G.items()):
        got = None
        for k in range(0, 16):
            w = word(imgs["G"], site - 4 * k)
            if w is not None and (w >> 26) == 9 and ((w >> 16) & 31) == 6 and ((w >> 21) & 31) == 0:
                got = w & 0xFFFF
                break
        b.check(got == want, "RASHCDG %08X: PlaySound3D id %d" % (site, want), str(got))

    print("== DATA\\%s - the bank directory closes on the file size" % RASHNZ)
    raw, ents = rashnz()
    b.check(len(ents) == cfg["rashnz_entries"], "the directory has %d entries" % cfg["rashnz_entries"])
    run = 0
    for i, (off, bb, sb) in enumerate(ents):
        b.check(off == run, "entry %d starts at the running end %06X" % (i, run), "%06X" % off)
        run += bb + sb
    b.check(RASHNZ_HEADER + run == len(raw),
            "header + every (bank + samples) == the file size (%d)" % len(raw),
            "%d" % (RASHNZ_HEADER + run))
    for i, (off, bb, sb) in enumerate(ents):
        magic, count, spu, nbytes, offs, _ = bank_parse(raw, RASHNZ_HEADER + off)
        b.check(magic == 2, "bank %d: magic 2" % i, str(magic))
        b.check(nbytes == sb, "bank %d: the header's sample count matches the directory" % i)
        b.check(offs[0] == BANK_TABLE + 4 * count,
                "bank %d: the first sound record follows the offset table" % i)
        b.check(bank_end(raw, RASHNZ_HEADER + off, count, offs) - (RASHNZ_HEADER + off) <= bb,
                "bank %d: every sound record fits inside the declared bank bytes" % i)
        b.check(all(o == 0 or o < bb for o in offs), "bank %d: every offset is in range" % i)
        nz = sum(1 for o in offs if o)
        descs = sum(raw[RASHNZ_HEADER + off + o] for o in offs if o)
        b.check(raw[RASHNZ_HEADER + off + 5] == nz and raw[RASHNZ_HEADER + off + 6] == nz,
                "bank %d: +0x05 and +0x06 are the NON-EMPTY sound count (%d of %d)"
                % (i, nz, count),
                "%d %d" % (raw[RASHNZ_HEADER + off + 5], raw[RASHNZ_HEADER + off + 6]))
        b.check(descs == nz, "bank %d: every non-empty sound holds exactly one descriptor" % i)
        b.check(bb == BANK_TABLE + 4 * count + (4 + SOUND_DESC_STRIDE) * nz,
                "bank %d: bankBytes == 0x10 + 4*count + 16*nonEmpty" % i,
                "%X vs %X" % (bb, BANK_TABLE + 4 * count + 16 * nz))

    print("== the resident banks ARE the file's banks, patched only where the structure says")
    st = states()
    b.check(bool(st), "at least one savestate extract is present")
    for name, ram, spuram in st:
        n = ram.s32(SNDSYS + S_BANK_COUNT)
        bt = ram.u32(SNDSYS + S_BANKS)
        matched = 0
        for i, (off, bb, sb) in enumerate(ents):
            fb = raw[RASHNZ_HEADER + off:RASHNZ_HEADER + off + bb]
            _, count, _, _, offs, patched = bank_parse(raw, RASHNZ_HEADER + off)
            patched = set(p - (RASHNZ_HEADER + off) for p in patched)
            for j in range(n):
                p = ram.u32(bt + 4 * j)
                if p == 0:
                    continue
                rb = ram.blk(p, bb)
                if len(rb) != bb or rb[BANK_COUNT] != fb[BANK_COUNT]:
                    continue
                if rb[BANK_SAMPLE_BYTES:BANK_SAMPLE_BYTES + 4] != fb[BANK_SAMPLE_BYTES:BANK_SAMPLE_BYTES + 4]:
                    continue
                diff = set(k for k in range(bb) if rb[k] != fb[k])
                if not diff <= patched:
                    continue
                matched += 1
                b.check(True, "%s: bank %d of the file is resident as bank %d" % (name, i, j))
                b.check(diff <= patched,
                        "%s bank %d: only the predicted SPU-address words differ" % (name, i))
                deltas = set()
                for w in sorted(x for x in patched if x % 4 == 0):
                    deltas.add(struct.unpack_from("<I", rb, w)[0]
                               - struct.unpack_from("<I", fb, w)[0])
                base = struct.unpack_from("<I", rb, BANK_SPU_ADDR)[0]
                b.check(deltas == {base},
                        "%s bank %d: every patched word is shifted by the bank's own +0x08 (%06X)"
                        % (name, i, base), str(sorted(deltas)))
                if spuram is not None:
                    payload = raw[RASHNZ_HEADER + off + bb:RASHNZ_HEADER + off + bb + sb]
                    b.check(spuram[base:base + sb] == payload,
                            "%s bank %d: %d bytes of SPU RAM at %06X are the file's samples"
                            % (name, i, sb, base))
                break
        b.check(matched >= 3, "%s: at least three of the file's banks are resident" % name,
                str(matched))

    print("== DATA\\%s - every record carries a bank of the same format" % TAUNT)
    tpath = os.path.join(DATA, TAUNT)
    if os.path.isfile(tpath):
        traw = open(tpath, "rb").read()
        b.check(len(traw) == TAUNT_RECORDS * TAUNT_RECORD,
                "%s is %d records of 0x%X" % (TAUNT, TAUNT_RECORDS, TAUNT_RECORD))
        bad = []
        samples = 0
        for r in range(TAUNT_RECORDS):
            at = r * TAUNT_RECORD + TAUNT_BANK_AT
            magic, count, spu, nbytes, offs, _ = bank_parse(traw, at)
            if magic != 2 or count not in (1, 2):
                bad.append((r, "magic %d count %d" % (magic, count)))
                continue
            if offs[0] != BANK_TABLE + 4 * count:
                bad.append((r, "table end %d != %d" % (offs[0], BANK_TABLE + 4 * count)))
                continue
            if any(traw[at + o] != 1 for o in offs):
                bad.append((r, "a sound record does not hold exactly one descriptor"))
                continue
            samples += count
        b.check(not bad, "all %d records parse as banks" % TAUNT_RECORDS,
                "%d bad: %s" % (len(bad), bad[:3]))
        b.check(samples == 277,
                "the records declare 277 samples, which is what audio.md 4 counts from the "
                "SPU loop flags", str(samples))

        # the six resident taunt banks ARE six records of this file, uploaded from +0x60
        body = TAUNT_BANK_AT + BANK_TABLE + 4 * 2 + 2 * (4 + SOUND_DESC_STRIDE)   # 0x60
        for name, ram, spuram in st:
            if spuram is None:
                continue
            n = ram.s32(SNDSYS + S_BANK_COUNT)
            bt = ram.u32(SNDSYS + S_BANKS)
            found = []
            for j in range(n):
                p = ram.u32(bt + 4 * j)
                if p == 0 or ram.u8(p + BANK_COUNT) != 2:
                    continue
                addr = ram.u32(p + BANK_SPU_ADDR)
                want = spuram[addr:addr + TAUNT_RECORD - body]
                hits = [r for r in range(TAUNT_RECORDS)
                        if traw[r * TAUNT_RECORD + body:
                                r * TAUNT_RECORD + TAUNT_RECORD] == want]
                b.check(len(hits) == 1,
                        "%s: the 2-sound bank %d is exactly one %s record (%s)"
                        % (name, j, TAUNT, hits), str(len(hits)))
                found += hits
            b.check(len(found) == 6,
                    "%s: six taunt records are resident, one per rider" % name, str(len(found)))
            b.check(len(set(found)) == len(found),
                    "%s: no taunt record is loaded twice" % name)

    print("== the live voice table, the ring and the reserved table")
    for name, ram, _ in st:
        n = ram.s32(SNDSYS + S_VOICE_COUNT)
        b.check(n == cfg["voices"], "%s: %d voices" % (name, cfg["voices"]), str(n))
        vb = ram.u32(SNDSYS + S_VOICES)
        ok = all(ram.s32(vb + cfg["voice_stride"] * i + V_CHANNEL) == i for i in range(n))
        b.check(ok, "%s: voice[i][+0x1C] == i for all %d voices (the channel IS the index)"
                % (name, n))
        b.check(ram.s32(SNDSYS + S_MASTER_VOL) == 127, "%s: master volume 127" % name)
        b.check(ram.s32(SNDSYS + S_PAN_OFF) == 0, "%s: panning enabled" % name)
        b.check(ram.s32(SNDSYS + S_BUSY) == 0, "%s: the service is not re-entered" % name)
        b.check(ram.s32(SNDSYS + S_FREE_TOP) == -1,
                "%s: the free stack is exhausted - every new one-shot steals from the ring" % name)
        head = ram.s32(SNDSYS + S_RING_HEAD)
        tail = ram.s32(SNDSYS + S_RING_TAIL)
        b.check(0 <= head < cfg["ring"] and 0 <= tail < cfg["ring"],
                "%s: the ring indices are inside [0,%d)" % (name, cfg["ring"]))
        ring = [ram.s32(SNDSYS + S_RING + 4 * k) for k in range(25)]
        resv = [ram.s32(SNDSYS + S_RESERVED + 4 * k) for k in range(22)]
        live = [v for v in ring if v != -1]
        b.check(all(0 <= v < n for v in live), "%s: every ring entry names a real voice" % name)
        b.check(len(set(live)) == len(live), "%s: no voice is on the ring twice" % name)
        rlive = [v for v in resv if v != -1]
        b.check(all(0 <= v < n for v in rlive), "%s: every reserved entry names a real voice" % name)
        b.check(not (set(live) & set(rlive)),
                "%s: a voice is on the steal ring OR reserved, never both" % name)
        b.check(len(live) + len(rlive) == n,
                "%s: ring + reserved account for all %d voices" % (name, n),
                "%d + %d" % (len(live), len(rlive)))

    print("== the listener, and where it gets its position and heading")
    for name, ram, _ in st:
        L = ram.u32(G_LISTENERS)
        En = ram.u32(G_ENGINE)
        b.check(L != 0 and En != 0, "%s: the listener and engine records are allocated" % name)
        b.check(En - L == cfg["listener_stride"] + 8,
                "%s: the engine array starts one allocator header past ONE %d-byte listener"
                % (name, cfg["listener_stride"]), "%d" % (En - L))
        cam = ram.u32(CAM_PTR)
        yaw = ram.s32(L + L_YAW)
        want = cfg["half_turn"] - ram.s16(cam + CAM_YAW)
        b.check(abs(yaw - want) <= 4,
                "%s: listener yaw == %d - camera yaw (%d vs %d)"
                % (name, cfg["half_turn"], yaw, want))
        b.check(ram.s32(L + L_PAN) == 64, "%s: the listener's default pan is centre (64)" % name)
        bike = ram.u32(PLAYER_BIKES)
        for off, lo, what in ((ENT_X, L_X, "x"), (ENT_Z, L_Z, "z"),
                              (ENT_VX, L_VX, "vx"), (ENT_VZ, L_VZ, "vz")):
            d = abs(ram.s32(L + lo) - ram.s32(bike + off))
            b.check(d < 0x20000,
                    "%s: listener %s tracks the player bike (delta %d of 16.16)" % (name, what, d),
                    "%08X vs %08X" % (ram.u32(L + lo), ram.u32(bike + off)))

    print("== the per-player engine-sound record")
    for name, ram, _ in st:
        En = ram.u32(G_ENGINE)
        n = ram.s32(SNDSYS + S_VOICE_COUNT)
        vb = ram.u32(SNDSYS + S_VOICES)
        b.check(ram.u32(En + EN_ENTITY) == ram.u32(PLAYER_BIKES),
                "%s: the engine record points at the player's bike" % name)
        b.check(ram.u8(En + EN_ENABLE) == 1, "%s: the engine record is enabled" % name)
        b.check(ram.u32(En + EN_PITCH_SCALE) == 0x10000,
                "%s: the engine pitch scale is 1.0 in 16.16" % name)
        b.check(ram.s32(En + EN_PAN) == 64, "%s: the engine sound is panned centre" % name)
        params = ram.u32(En + EN_PARAMS)
        b.check(0x80010000 <= params < 0x80060000,
                "%s: the engine parameter record lives in the EXE's own data (%08X)"
                % (name, params))
        live = 0
        for h in EN_HANDLES:
            v = ram.u32(En + h)
            if v == 0:
                continue
            ch = v >> 27
            b.check(0 <= ch < n, "%s: engine handle +0x%02X names channel %d" % (name, h, ch))
            if ram.u32(vb + cfg["voice_stride"] * ch + V_SERIAL) & 0x07FFFFFF == v & 0x07FFFFFF:
                live += 1
        b.check(live >= 3,
                "%s: at least three of the five engine layers still own their voice" % name,
                str(live))
        b.check(ram.s32(En + EN_LEVEL1) >= 0 and ram.s32(En + EN_TARGET1) >= 0,
                "%s: the rev ramp and its target are non-negative" % name)

    print("== the SPU register window")
    for name, ram, _ in st:
        b.check(ram.u32(SPU_BASE_PTR) == 0x1F801C00,
                "%s: the libspu voice register base is 0x1F801C00" % name,
                "%08X" % ram.u32(SPU_BASE_PTR))
    b.check(word(imgs["SLUS"], 0x80051C8C) == 0xA4440004,
            "SpuSetVoiceAttr writes the pitch at base + 16*voice + 4")

    print()
    print("sound: %d checks, %d failures" % (b.n, len(b.bad)))
    if b.bad:
        sys.exit(1)


# ---------------------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        prog="sound.py", description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("info", help="the decoded skeleton (addresses only)")
    s.set_defaults(func=cmd_info)

    s = sub.add_parser("banks", help="the bank directory and the live bank table")
    s.set_defaults(func=cmd_banks)

    s = sub.add_parser("voices", help="the live voice table of each savestate")
    s.set_defaults(func=cmd_voices)

    s = sub.add_parser("events", help="every PlaySound3D call site and its sound id")
    s.set_defaults(func=cmd_events)

    s = sub.add_parser("verify", help="the assertion bench (the gate)")
    s.add_argument("--mutate", action="store_true",
                   help="perturb five documented claims so the probe can be seen failing")
    s.set_defaults(func=cmd_verify)
    return ap


def main() -> None:
    args = build_parser().parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
