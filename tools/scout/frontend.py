"""Scout probes for the FRONT END of Road Rash: Jailbreak (USA, SLUS_01053).

The "front end" is everything the overlay `RASHCDF.BIN` owns: the screen graph, the
menu/widget database, the career shell (venue, unlocks, prize money) and the string
pools `GAMESTRG.LOC` / `DATA\\FE\\FESTRING.LOC`.

Companion document: docs\\formats\\frontend.md.

Reads ONLY from work\\disc_us (the player's own disc extract, gitignored) and, for `scan`,
from a PlayStation memory-card image named on the command line.  Writes ONLY to
stdout or to explicitly named paths under work\\frontend.  Never copies game bytes or game
text into the repository.

Usage (from the project root):

    python tools\\scout\\frontend.py info                 # everything, short form
    python tools\\scout\\frontend.py info screens         # the 58 screen records
    python tools\\scout\\frontend.py info graph           # the screen graph, as edges
    python tools\\scout\\frontend.py info widgets         # widget types and their handlers
    python tools\\scout\\frontend.py info codes           # the 81 menu action codes
    python tools\\scout\\frontend.py info res             # the frontend resource tables
    python tools\\scout\\frontend.py info strings         # the two .LOC pools (counts only)
    python tools\\scout\\frontend.py info career          # session record / venues / prizes
    python tools\\scout\\frontend.py scan <card.mcr|.srm> # decode a PS1 memory card
    python tools\\scout\\frontend.py verify               # the assertion bench
    python tools\\scout\\frontend.py emit [outdir]        # artifacts into work\\frontend

Every probe prints the inputs it used (file + sha1) so results are reproducible.
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
FE_DIR = os.path.join(DISC, "DATA", "FE")

RASHCDF_SHA1 = "a3fec4b4e9292c358d0f6dc529843f5d8f25924a"
OVL_BASE = 0x8005B5E8

# ---------------------------------------------------------------------------
# Addresses.  Every one is derived in docs\formats\frontend.md with the
# instruction address that establishes it.  All are RASHCDF unless marked.
# ---------------------------------------------------------------------------

F_BOOT = 0x8007FEDC             # frontend cold entry (EXE 0x80012288)
F_RESUME = 0x8007FF4C           # frontend entry after a race (EXE 0x800122DC)
F_MAINLOOP = 0x80080274         # while (game_state->state == 2) { ... }
F_BUILD_SCREENS = 0x800806D0    # fills the screen pointer table
F_BUILD_NAV = 0x80068E88        # fills the four navigation tables + handler tables
F_GOTO_SCREEN = 0x800809E0      # GotoScreen(id)
F_ENTER_SCREEN = 0x800809A8     # arm a screen: flags |= 3, f02 = 32767
F_CHANGE_SCREEN = 0x80066EF8    # consume fe->f02
F_TICK = 0x80066C34             # per-frame screen state machine
F_INPUT = 0x800667E4            # per-frame input pass
F_DRAW = 0x8006711C             # per-frame draw pass
F_BIND_ITEM = 0x800680E8        # type-13 widget -> data object
F_CODE_KIND = 0x80072100        # action code -> small "mode kind" (types 12/13)
F_SET_MODE = 0x800685BC         # action code -> the mode word at 0x800D80D8
F_NEW_GAME = 0x80068688         # reset the session record
F_COMMIT = 0x8007F37C           # session record -> game_state
F_MOVE_NEXT = 0x8006C3A4        # move the selection forward
F_MOVE_PREV = 0x8006C558        # move the selection back
F_ALL_DONE_A = 0x8007E724       # all races in [first,last] done?  bitmap +0xF0
F_ALL_DONE_B = 0x8007E770       # ditto, bitmap +0xFC
F_VENUE_STEP = 0x8007D4A0       # per-venue career progress dispatcher
F_RESULT_CAREER = 0x8007BB34    # career/Jailbreak result dispatcher
F_RESULT_SINGLE = 0x8007DDA8    # single-player result dispatcher
F_RESULT_CNR = 0x8007E340       # Cops'n'Robbers result dispatcher

T_SCREEN_PTRS = 0x800A0880      # 59 screen record pointers (runtime)
T_NAV_ADVANCE = 0x8009C6C8      # [screenId] -> screen reached on "advance"
T_NAV_BACK = 0x8009C7B8         # [screenId] -> screen reached on "cancel"
T_CODE_GROUP = 0x8009C9B0       # [actionCode] -> the group's commit action code
T_CODE_SCREEN = 0x8009CAF8      # [actionCode] -> target screen
T_SCREEN_INPUT = 0x8009C8C0     # [screenId] -> per-screen input handler
T_SCREEN_CTX = 0x8009CC40       # [screenId] -> per-screen context handler
T_SCREEN_A = 0x8009D0C0         # [screenId] -> "transition" handler
T_SCREEN_B = 0x8009CFD0         # [screenId] -> "tick" handler
T_WIDGET_UPDATE = 0x8009CF28    # [widgetType] -> update handler (19 entries)
T_WIDGET_DRAW = 0x8009CF78      # [widgetType] -> draw handler (20 entries)
T_BIND_ARMS = 0x8005BAC4        # 72 arms, index = actionCode - 9  (F_BIND_ITEM)
T_CODE_KIND_ARMS = 0x8005C434   # 80 arms, index = actionCode     (F_CODE_KIND)
T_SET_MODE_ARMS = 0x8005BC64    # 30 arms, index = actionCode - 2 (F_SET_MODE)
T_VENUE_STEP_ARMS = 0x8005C988   # 6 arms, index = session+0x04
T_RESUME_ARMS = 0x8005CB80      # 32 arms, index = mode - 1       (F_RESUME)
T_NAMES = 0x8008973C            # 42 pointers to asset file names
T_RESOURCES = 0x800897E4        # 37 resource records, stride 32
T_TAGS_A = 0x80088DF4           # FourCC id table, variant A
T_TAGS_B = 0x800892BC           # FourCC id table, variant B
T_PRIZE = 0x80089F44            # u16 prize[place][venue], row stride 24
S_ALIAS_ALPHABET = 0x80088C9C   # the alias-entry keyboard, one NUL-terminated string
S_CARD_PATH_FMT = 0x8005B66C    # "bu0%1ld:%s"
S_CARD_FILE = 0x8005B678        # the memory-card file name
S_DATA_FMT = 0x8005BF84         # "DATA\%s"

FE_CTX = 0x8009C5D0             # the frontend context struct
FE_CUR_SCREEN = FE_CTX + 0x00   # s16 current screen id
FE_NEXT_SCREEN = FE_CTX + 0x02  # s16 requested screen id
FE_PENDING = FE_CTX + 0x04      # s16 screen to enter after a modal
FE_BUTTONS = FE_CTX + 0x0A      # u16 frame button/edge mask
FE_CUR_ITEM = FE_CTX + 0x84     # Widget* the selected item of the current screen
FE_PRIZE = FE_CTX + 0x70        # s16 race bonus just awarded

SESSION = 0x800D80D8            # the persistent session/career record (rules.md 1.2)
SESSION_PLAYER = 0x800D81D8     # player records, stride 0x24
PAD_ARRAY = 0x800D7128          # pad records, stride 192
PAD_BTN0 = 0x1A                 # first per-button counter, stride 8
PAD_BTN_STRIDE = 8

SCREEN_COUNT = 59               # the loop bound at F_GOTO_SCREEN + 0x3C
SCREEN_STRIDE = 20
WIDGET_STRIDE = 120
ACTION_CODE_COUNT = 81          # index 80 is the -1 terminator of both code tables
SCREEN_START_RACE = 57          # a screen id with no record: "leave the frontend"

# Widget types, named from their handlers (see frontend.md section 4).
WIDGET_TYPES = {
    0: "movie",         # plays names[+0x10] full screen (0x8006DB5C)
    1: "?1",
    2: "?2", 3: "?3", 4: "?4", 5: "?5", 6: "?6",   # share handler 0x8006E4D8
    7: "sprite",        # FourCC at +0x10, colour +0x14, pos +0x18 (0x8006E894)
    8: "logo",          # kind = +0x14 - 48, 0..5 (0x8006E8FC)
    9: "buttonhint",    # pad glyph + label (0x8006E7A4)
    10: "?10",
    11: "panel",
    12: "button",       # action code +0x12, label +0x14 (0x8006F764)
    13: "chooser",      # as 12, plus a bound data object (0x8006EF30)
    14: "?14",
    15: "?15",
    16: "textblock",    # x,y,w,h,+kind (0x8006FAC8)
    17: "slider",       # 0x8006F4A0; +0x64 index < 6, +0x66 slot
    18: "?18",          # 0x8006F0B8
    19: "?19",          # draw-only, 0x80070580
}

# ---------------------------------------------------------------------------
# Low-level helpers
# ---------------------------------------------------------------------------

LOAD_OPS = {0x20: "lb", 0x21: "lh", 0x23: "lw", 0x24: "lbu", 0x25: "lhu"}
STORE_OPS = {0x28: "sb", 0x29: "sh", 0x2B: "sw"}


def _simm(w):
    return (w & 0xFFFF) - 0x10000 if (w & 0x8000) else (w & 0xFFFF)


class Overlay:
    """The frontend overlay as a flat guest image."""

    def __init__(self):
        self.img = E.load_overlay("RASHCDF.BIN", OVL_BASE)
        self.d = self.img.data
        self.base = self.img.base
        self.sha1 = self.img.sha1

    def has(self, a, n=1):
        return self.base <= a and a + n <= self.base + len(self.d)

    def u8(self, a):
        return self.d[a - self.base]

    def u16(self, a):
        return struct.unpack_from("<H", self.d, a - self.base)[0]

    def s16(self, a):
        return struct.unpack_from("<h", self.d, a - self.base)[0]

    def u32(self, a):
        return struct.unpack_from("<I", self.d, a - self.base)[0]

    def s32(self, a):
        return struct.unpack_from("<i", self.d, a - self.base)[0]

    def bytes(self, a, n):
        return self.d[a - self.base: a - self.base + n]

    def cstr(self, a):
        o = a - self.base
        e = self.d.index(b"\0", o)
        return self.d[o:e].decode("latin1")

    def walk_stores(self, start, limit=12000):
        """Interpret a straight-line table-filling function.

        Follows lui/addiu register forming and records every `sw`.  Branches are
        ignored: these functions are unconditional initialisers.  Stops at the
        first `jr ra`.  Returns {address: value}.
        """
        a, reg, out = start, {0: 0}, {}
        for _ in range(limit):
            if not self.has(a, 4):
                break
            w = self.u32(a)
            op, rs, rt = w >> 26, (w >> 21) & 0x1F, (w >> 16) & 0x1F
            if op == 0x0F:
                reg[rt] = (w & 0xFFFF) << 16
            elif op == 0x09:
                reg[rt] = (reg.get(rs, 0) + _simm(w)) & 0xFFFFFFFF
            elif op == 0x2B:
                b = reg.get(rs)
                if b is not None:
                    out[(b + _simm(w)) & 0xFFFFFFFF] = reg.get(rt)
            if w == 0x03E00008:
                break
            a += 4
        return out


# ---------------------------------------------------------------------------
# The screen database
# ---------------------------------------------------------------------------

class Screen:
    __slots__ = ("id", "addr", "flags", "count", "parent", "enter_anim",
                 "leave_anim", "pad_first", "pad_count", "items", "widgets", "rec_id")


class Widget:
    __slots__ = ("index", "addr", "raw", "type", "flags", "code", "string_id",
                 "tag", "x", "y")


def _tag_at(raw, off):
    s = raw[off:off + 4]
    if len(s) == 4 and all(0x41 <= c <= 0x5A or 0x30 <= c <= 0x39 or c == 0x5F for c in s):
        return s.decode("latin1")
    return None


BACKDROP_TAGS = ("BAC1", "BAC2")


def screen_title(s):
    """The screen's title banner: the first type-7 sprite that is not a backdrop."""
    for w in s.widgets:
        if w.type == 7 and w.tag and w.tag not in BACKDROP_TAGS:
            return w.tag
    return ""


def load_screens(ov: Overlay):
    """Replay F_BUILD_SCREENS, then decode each 20-byte record and its widgets."""
    stores = ov.walk_stores(F_BUILD_SCREENS)
    table = {}
    for addr, val in stores.items():
        if T_SCREEN_PTRS <= addr < T_SCREEN_PTRS + 4 * SCREEN_COUNT:
            table[(addr - T_SCREEN_PTRS) // 4] = val
    screens = {}
    for sid in sorted(table):
        p = table[sid]
        if p is None or not ov.has(p, SCREEN_STRIDE):
            continue
        s = Screen()
        (s.flags, _f2, _cur, rec_id, s.count, s.parent,
         s.enter_anim, s.leave_anim, s.pad_first, s.pad_count,
         s.items) = struct.unpack_from("<HhhhhhBBBBI", ov.d, p - ov.base)
        s.id, s.addr = sid, p
        s.widgets = []
        if s.items and ov.has(s.items, WIDGET_STRIDE * max(s.count, 0)):
            for k in range(s.count):
                wa = s.items + WIDGET_STRIDE * k
                raw = ov.bytes(wa, WIDGET_STRIDE)
                w = Widget()
                w.index, w.addr, w.raw = k, wa, raw
                w.type, w.flags = struct.unpack_from("<hH", raw, 8)
                w.code = struct.unpack_from("<H", raw, 0x12)[0]
                w.string_id = struct.unpack_from("<H", raw, 0x14)[0]
                w.x, w.y = struct.unpack_from("<HH", raw, 0x24)
                w.tag = None
                for off in (0x10, 0x28):
                    t = _tag_at(raw, off)
                    if t:
                        w.tag = t
                        break
                s.widgets.append(w)
        s.rec_id = rec_id
        screens[sid] = s
    return screens


def load_nav(ov: Overlay):
    """Replay F_BUILD_NAV and split its stores into the six tables it fills."""
    stores = ov.walk_stores(F_BUILD_NAV)

    def slice_(base, n):
        return {(a - base) // 4: v for a, v in stores.items() if base <= a < base + 4 * n}

    return {
        "advance": slice_(T_NAV_ADVANCE, SCREEN_COUNT),
        "back": slice_(T_NAV_BACK, SCREEN_COUNT),
        "code_group": slice_(T_CODE_GROUP, ACTION_CODE_COUNT),
        "code_screen": slice_(T_CODE_SCREEN, ACTION_CODE_COUNT),
        "input": slice_(T_SCREEN_INPUT, SCREEN_COUNT),
        "ctx": slice_(T_SCREEN_CTX, SCREEN_COUNT),
    }


def load_screen_handlers(ov: Overlay):
    """The A/B handler tables, filled by a loop with per-screen overrides."""
    stores = ov.walk_stores(0x8006D174)
    a = {(x - T_SCREEN_A) // 4: v for x, v in stores.items()
         if T_SCREEN_A <= x < T_SCREEN_A + 4 * SCREEN_COUNT}
    b = {(x - T_SCREEN_B) // 4: v for x, v in stores.items()
         if T_SCREEN_B <= x < T_SCREEN_B + 4 * SCREEN_COUNT}
    upd = {(x - T_WIDGET_UPDATE) // 4: v for x, v in stores.items()
           if T_WIDGET_UPDATE <= x < T_WIDGET_UPDATE + 4 * 20}
    drw = {(x - T_WIDGET_DRAW) // 4: v for x, v in stores.items()
           if T_WIDGET_DRAW <= x < T_WIDGET_DRAW + 4 * 20}
    return a, b, upd, drw


def load_names(ov: Overlay):
    """The 42-entry asset-name pointer array at T_NAMES."""
    out = []
    for i in range(42):
        p = ov.u32(T_NAMES + 4 * i)
        out.append(ov.cstr(p) if ov.has(p) else None)
    return out


def load_resources(ov: Overlay):
    """The 37 frontend resource records at T_RESOURCES, stride 32."""
    out = []
    for i in range(37):
        o = T_RESOURCES + 32 * i
        flags, klass, rid, data = struct.unpack_from("<HBBI", ov.d, o - ov.base)
        name = ov.bytes(o + 8, 16).split(b"\0")[0].decode("latin1")
        lba, rsv = struct.unpack_from("<iI", ov.d, o - ov.base + 24)
        out.append(dict(index=i, addr=o, flags=flags, klass=klass, id=rid,
                        data=data, name=name, lba=lba, reserved=rsv))
    return out


def load_tags(ov: Overlay, base: int, end: int):
    """A run of 4-character resource ids, used as the game's asset-id enum."""
    tags = []
    a = base
    while a + 4 <= end:
        t = _tag_at(ov.bytes(a, 4), 0)
        if t is None:
            break
        tags.append(t)
        a += 4
    return tags


def load_prize_table(ov: Overlay, rows=20, venues=6):
    """u16 prize[place][venue], row stride 24 bytes (rules.md 7.4)."""
    return [[ov.s16(T_PRIZE + 24 * r + 4 * v) for v in range(venues)] for r in range(rows)]


def load_bind_targets(ov: Overlay):
    """actionCode -> the data-object pointer F_BIND_ITEM installs in fe->+0x94."""
    out = {}
    for i in range(72):
        arm = ov.u32(T_BIND_ARMS + 4 * i)
        code = i + 9
        if not ov.has(arm, 4):
            out[code] = None
            continue
        a, reg, found = arm, {}, None
        for _ in range(14):
            w = ov.u32(a)
            op, rs, rt = w >> 26, (w >> 21) & 0x1F, (w >> 16) & 0x1F
            if op == 0x0F:
                reg[rt] = (w & 0xFFFF) << 16
            elif op == 0x09:
                reg[rt] = (reg.get(rs, 0) + _simm(w)) & 0xFFFFFFFF
            elif op in LOAD_OPS and found is None:
                b = reg.get(rs)
                if b is not None:
                    found = (b + _simm(w)) & 0xFFFFFFFF
            if op == 0x02 or w == 0x03E00008:
                break
            a += 4
        out[code] = found
    return out


def load_mode_codes(ov: Overlay):
    """actionCode -> the mode word F_SET_MODE stores (rules.md 2.1)."""
    out = {}
    for i in range(30):
        arm = ov.u32(T_SET_MODE_ARMS + 4 * i)
        code = i + 2
        if not ov.has(arm, 8):
            continue
        # arms are   li v0, N ; sw v0, lo(0x800D80D8)(rX)   or a bare jump to the no-op
        a, val = arm, None
        for _ in range(6):
            w = ov.u32(a)
            if (w >> 26) == 0x09 and ((w >> 21) & 0x1F) == 0 and ((w >> 16) & 0x1F) == 2:
                val = _simm(w)
            if (w >> 26) == 0x2B and val is not None:
                out[code] = val
                break
            if (w >> 26) == 0x02:
                break
            a += 4
    return out


# ---------------------------------------------------------------------------
# .LOC string pools  (structure and counts only; never the text)
# ---------------------------------------------------------------------------

def loc_info(path):
    raw = open(path, "rb").read()
    if raw[:4] != b"LOCH":
        raise ValueError("%s: not a LOCH container" % path)
    hdr_size, z0, ver, chunk_off = struct.unpack_from("<IIII", raw, 4)
    magic = raw[chunk_off:chunk_off + 4]
    chunk_size, z1, count = struct.unpack_from("<III", raw, chunk_off + 4)
    offs = list(struct.unpack_from("<%dI" % count, raw, chunk_off + 16))
    return dict(path=path, size=len(raw), sha1=E.sha1_of(path),
                hdr_size=hdr_size, z0=z0, version=ver, chunk_off=chunk_off,
                chunk_magic=magic.decode("latin1"), chunk_size=chunk_size, z1=z1,
                count=count, index_end=16 + 4 * count, first_off=offs[0] if offs else None,
                offs=offs, raw=raw)


def loc_lengths(info):
    """Byte length of each string, without ever returning the text itself."""
    raw, c = info["raw"], info["chunk_off"]
    out = []
    for o in info["offs"]:
        e = raw.index(b"\0", c + o)
        out.append(e - (c + o))
    return out


# ---------------------------------------------------------------------------
# PlayStation memory card
# ---------------------------------------------------------------------------

CARD_FRAME = 128
CARD_BLOCK = 8192
CARD_SIZE = 128 * 1024

# The save payload, as RASHCDF builds it.  See frontend.md section 8.
SAVE_BYTES = 6948               # a2 at 0x8005F1BC / 0x8005F1F8
SAVE_HDR = 512                  # block header template, 0x80080E8C -> ctx+0x20
SAVE_RECORDS_OFF = 0x204        # 1584-byte records table (seeded from SLUS 0x80053A88)
SAVE_RECORDS_LEN = 1584
SAVE_SLOT0 = 0x834              # first career slot inside the block
SAVE_SLOT_STRIDE = 484
SAVE_SLOT_COUNT = 10
SAVE_SLOT_PLAYERS = 0x004       # 216 bytes <- guest 0x800D81D8
SAVE_SLOT_PLAYERS_LEN = 216
SAVE_SLOT_SESSION = 0x0DC       # 256 bytes <- guest 0x800D80D8
SAVE_SLOT_SESSION_LEN = 256
SAVE_SLOT_CSUM = 0x1DC          # two u32
SAVE_CSUM_LEN = 476
HDR_TEMPLATE = 0x80080E8C       # the static "SC" title frame inside RASHCDF


def save_checksum(rec: bytes, slot: int):
    """RASHCDF 0x8005E9D0, over the first 476 bytes of a career record.

    a   = a + x + i + acc
    b   = ((x ^ b) << 1) + acc
    acc = acc + (slot + 1)
    """
    a = b = acc = 0
    M = 0xFFFFFFFF
    for i in range(SAVE_CSUM_LEN):
        x = rec[i]
        a = (a + x + i + acc) & M
        b = (((x ^ b) << 1) + acc) & M
        acc = (acc + slot + 1) & M
    return a, b


def card_directory(raw, card_off=0):
    """The 15 directory frames of block 0.  Structure is generic PS1, verified
    against the player's own card in `scan`."""
    ents = []
    for i in range(15):
        o = card_off + CARD_FRAME * (i + 1)
        f = raw[o:o + CARD_FRAME]
        state, size, link = struct.unpack_from("<IIH", f, 0)
        name = f[0x0A:0x0A + 20].split(b"\0")[0].decode("latin1", "replace")
        xor = 0
        for b in f[:0x7F]:
            xor ^= b
        ents.append(dict(slot=i, state=state, size=size, link=link, name=name,
                         xor_ok=(xor == f[0x7F])))
    return ents


def card_images(path):
    """Yield (label, offset) for each 128 KiB card image inside `path`.

    A `.mcr` is one raw card.  A RetroArch `.srm` for a two-slot core is two
    cards back to back.  Anything else is reported, not guessed at.
    """
    size = os.path.getsize(path)
    if size % CARD_SIZE == 0 and size >= CARD_SIZE:
        for i in range(size // CARD_SIZE):
            yield ("card %d" % i, i * CARD_SIZE)
    else:
        yield ("whole file (size %d is not a multiple of %d)" % (size, CARD_SIZE), 0)


# ---------------------------------------------------------------------------
# info
# ---------------------------------------------------------------------------

def _hdr(ov):
    print("RASHCDF.BIN  sha1=%s  base=0x%08X  size=%d" % (ov.sha1, ov.base, len(ov.d)))
    if ov.sha1 != RASHCDF_SHA1:
        print("  !! sha1 differs from the documented image")


def cmd_info(args):
    ov = Overlay()
    what = args.what or "all"
    _hdr(ov)
    screens = load_screens(ov)
    nav = load_nav(ov)

    if what in ("all", "screens"):
        print("\n--- screen records (table at 0x%08X, built by 0x%08X) ---" % (
            T_SCREEN_PTRS, F_BUILD_SCREENS))
        print("  id  addr      flags  items parent enter/leave pads  itemArray   title  adv back")
        for sid in sorted(screens):
            s = screens[sid]
            title = screen_title(s)
            print("  %3d %08X %#06x %4d %6d  %3d/%-3d  %d+%d  0x%08X  %-5s %4s %4s" % (
                sid, s.addr, s.flags, s.count, s.parent, s.enter_anim, s.leave_anim,
                s.pad_first, s.pad_count, s.items, title,
                nav["advance"].get(sid, "-"), nav["back"].get(sid, "-")))
        missing = [i for i in range(SCREEN_COUNT) if i not in screens]
        print("  screen ids with no record: %s" % missing)

    if what in ("all", "graph"):
        print("\n--- screen graph (edges) ---")
        print("  from  kind        to     via")
        for sid in sorted(screens):
            a = nav["advance"].get(sid)
            if a is not None:
                print("  %4d  advance   %4d     [0x%08X + 4*%d]" % (sid, a, T_NAV_ADVANCE, sid))
            b = nav["back"].get(sid)
            if b is not None:
                print("  %4d  cancel    %4d     [0x%08X + 4*%d]" % (sid, b, T_NAV_BACK, sid))
            for w in screens[sid].widgets:
                if w.type in (12, 13):
                    t = nav["code_screen"].get(w.code)
                    if t is not None and t != 0xFFFFFFFF:
                        print("  %4d  code %-3d  %4d     item %d, str id %d" % (
                            sid, w.code, t, w.index, w.string_id))

    if what in ("all", "widgets"):
        a, b, upd, drw = load_screen_handlers(ov)
        print("\n--- widget types (tables 0x%08X update / 0x%08X draw) ---" % (
            T_WIDGET_UPDATE, T_WIDGET_DRAW))
        used = {}
        for s in screens.values():
            for w in s.widgets:
                used[w.type] = used.get(w.type, 0) + 1
        for t in range(20):
            print("  %2d %-11s update=%s draw=%s  instances=%d" % (
                t, WIDGET_TYPES.get(t, "?"),
                ("0x%08X" % upd[t]) if upd.get(t) else "-",
                ("0x%08X" % drw[t]) if drw.get(t) else "-",
                used.get(t, 0)))
        print("\n  per-screen handlers: transition table 0x%08X, tick table 0x%08X" % (
            T_SCREEN_A, T_SCREEN_B))
        print("  defaults 0x8006D5B0 / 0x8006D630; overrides:")
        for sid in sorted(set(a) | set(b)):
            print("    screen %3d  A=%s B=%s" % (
                sid, ("0x%08X" % a[sid]) if a.get(sid) else "-",
                ("0x%08X" % b[sid]) if b.get(sid) else "-"))

    if what in ("all", "codes"):
        binds = load_bind_targets(ov)
        modes = load_mode_codes(ov)
        owner = {}
        for sid, s in screens.items():
            for w in s.widgets:
                if w.type in (12, 13):
                    owner.setdefault(w.code, []).append((sid, w.index, w.type, w.string_id))
        print("\n--- menu action codes (0..%d) ---" % (ACTION_CODE_COUNT - 1))
        print("  code  used on            type  strId  ->screen  group  mode  dataObj")
        for c in range(ACTION_CODE_COUNT):
            u = owner.get(c)
            tgt = nav["code_screen"].get(c)
            grp = nav["code_group"].get(c)
            if u is None and tgt is None and grp is None:
                continue
            use = ",".join("s%d.i%d" % (x[0], x[1]) for x in u) if u else "-"
            typ = u[0][2] if u else "-"
            sid = u[0][3] if u else "-"
            print("  %4d  %-17s %4s %6s  %8s %6s %5s  %s" % (
                c, use, typ, sid,
                "-" if tgt is None else ("term" if tgt == 0xFFFFFFFF else tgt),
                "-" if grp is None else ("term" if grp == 0xFFFFFFFF else grp),
                modes.get(c, "-"),
                "-" if binds.get(c) is None else "0x%08X" % binds[c]))

    if what in ("all", "res"):
        names = load_names(ov)
        res = load_resources(ov)
        print("\n--- asset name array (0x%08X, 42 entries) ---" % T_NAMES)
        for i, n in enumerate(names):
            print("  %2d  %s" % (i, n))
        print("\n--- resource records (0x%08X, 37 x 32 bytes) ---" % T_RESOURCES)
        print("   i  flags  class id  name")
        for r in res:
            print("  %2d %#06x %5d %3d  %s" % (r["index"], r["flags"], r["klass"],
                                               r["id"], r["name"]))
        ta = load_tags(ov, T_TAGS_A, T_TAGS_B)
        tb = load_tags(ov, T_TAGS_B, T_NAMES)
        print("\n--- FourCC id tables --- A at 0x%08X: %d tags; B at 0x%08X: %d tags" % (
            T_TAGS_A, len(ta), T_TAGS_B, len(tb)))

    if what in ("all", "strings"):
        print("\n--- string pools ---")
        for p in (os.path.join(DISC, "DATA", "GAMESTRG.LOC"),
                  os.path.join(FE_DIR, "FESTRING.LOC")):
            if not os.path.exists(p):
                print("  missing: %s" % p)
                continue
            i = loc_info(p)
            L = loc_lengths(i)
            print("  %s" % os.path.relpath(p, ROOT))
            print("    sha1=%s size=%d hdr=%d ver=%d chunk='%s' chunkSize=%d count=%d" % (
                i["sha1"], i["size"], i["hdr_size"], i["version"], i["chunk_magic"],
                i["chunk_size"], i["count"]))
            print("    index ends at 0x%X, first string at 0x%X, empty strings=%d, "
                  "longest=%d bytes" % (i["index_end"], i["first_off"],
                                        sum(1 for x in L if x == 0), max(L)))

    if what in ("all", "career"):
        print("\n--- career shell ---")
        print("  session record        0x%08X (rules.md 1.2), player records 0x%08X stride 0x24"
              % (SESSION, SESSION_PLAYER))
        print("  venue                 session+0x04, advanced by 'sb' inside the arms of 0x%08X"
              % F_VENUE_STEP)
        print("  race-done bitmap      session+0xF0, bit = race id;  all-done test 0x%08X" % F_ALL_DONE_A)
        print("  second bitmap         session+0xFC, bit = session+0x07; test 0x%08X" % F_ALL_DONE_B)
        print("  bitmap +0xF0 setters  0x8007BF8C, 0x8007D904, 0x8007E0E8, 0x8007E2A4")
        print("  bitmap +0xFC setters  0x8007E47C, 0x8007E54C")
        print("  prize table           0x%08X, u16[place][venue], row stride 24" % T_PRIZE)
        pt = load_prize_table(ov)
        print("   place  v0    v1    v2    v3    v4    v5")
        for r, row in enumerate(pt):
            print("   %5d %s" % (r + 1, " ".join("%5d" % x for x in row)))
        print("  result dispatchers    career 0x%08X, single 0x%08X, cops'n'robbers 0x%08X"
              % (F_RESULT_CAREER, F_RESULT_SINGLE, F_RESULT_CNR))
        print("  race bonus lands in   fe+0x70 (0x%08X)" % FE_PRIZE)

    if what in ("all", "save"):
        print("\n--- memory card (static facts from RASHCDF) ---")
        print("  path format  0x%08X  %r" % (S_CARD_PATH_FMT, ov.cstr(S_CARD_PATH_FMT)))
        print("  file name    0x%08X  %r" % (S_CARD_FILE, ov.cstr(S_CARD_FILE)))
        print("  screens      43 Load, 44 Load Records, 45 Save, 46 Save Records")
    return 0


# ---------------------------------------------------------------------------
# scan
# ---------------------------------------------------------------------------

def cmd_scan(args):
    path = args.card
    if not os.path.exists(path):
        print("no such file: %s" % path)
        return 2
    ov = Overlay()
    want = ov.cstr(S_CARD_FILE)
    raw = open(path, "rb").read()
    print("card image: %s" % path)
    print("  size=%d sha1=%s" % (len(raw), E.sha1_of(path)))
    print("  looking for a directory entry whose name starts with %r" % want[:12])
    found = 0
    for label, off in card_images(path):
        magic = raw[off:off + 2]
        print("  %s at offset 0x%X: header magic %r" % (label, off, magic))
        if magic != b"MC":
            print("    not a PS1 card header ('MC' expected) - skipping")
            continue
        for e in card_directory(raw, off):
            if e["state"] == 0xA0:
                continue  # free, formatted
            print("    slot %2d state=%#04x size=%d link=%d xor=%s name=%r" % (
                e["slot"], e["state"], e["size"], e["link"],
                "ok" if e["xor_ok"] else "BAD", e["name"]))
            if e["name"].startswith(want[:12]):
                found += 1
                blk = off + CARD_BLOCK * (e["slot"] + 1)
                _decode_block(ov, raw[blk:blk + CARD_BLOCK], blk)
    print("  entries matching this game: %d" % found)
    if found == 0:
        print("  NEGATIVE: no block of this game on this image.")
    return 0


def _decode_block(ov: Overlay, blk: bytes, at: int):
    print("      block at 0x%X, %d bytes" % (at, len(blk)))
    head = blk[:SAVE_HDR]
    tmpl = ov.bytes(HDR_TEMPLATE, SAVE_HDR)
    print("      header: magic %r icon flag %#04x block no %d; identical to the static"
          " template at RASHCDF 0x%08X: %s" % (
              head[0:2], head[2], head[3], HDR_TEMPLATE,
              "yes" if head == tmpl else "NO (%d differing bytes)" % sum(
                  1 for x, y in zip(head, tmpl) if x != y)))
    print("      +0x200 current career slot = %d, +0x201 records-table initialised = %d"
          % (blk[0x200], blk[0x201]))
    rec_tbl = blk[SAVE_RECORDS_OFF:SAVE_RECORDS_OFF + SAVE_RECORDS_LEN]
    print("      +0x%03X records/best-times table: %d non-zero bytes of %d"
          % (SAVE_RECORDS_OFF, sum(1 for x in rec_tbl if x), SAVE_RECORDS_LEN))
    print("      career slots (checksum = RASHCDF 0x8005E9D0 over the first %d bytes):"
          % SAVE_CSUM_LEN)
    print("       slot  off     state    storedA    calcA      storedB    calcB      ok")
    for s in range(SAVE_SLOT_COUNT):
        o = SAVE_SLOT0 + SAVE_SLOT_STRIDE * s
        rec = blk[o:o + SAVE_SLOT_STRIDE]
        state = struct.unpack_from("<I", rec, 0)[0]
        sa, sb = struct.unpack_from("<II", rec, SAVE_SLOT_CSUM)
        ca, cb = save_checksum(rec, s)
        print("       %4d  0x%04X  %-7s 0x%08X 0x%08X 0x%08X 0x%08X  %s" % (
            s, o, "empty" if state == 1 else "IN USE", sa, ca, sb, cb,
            "yes" if (sa, sb) == (ca, cb) else "NO"))
    for s in range(SAVE_SLOT_COUNT):
        o = SAVE_SLOT0 + SAVE_SLOT_STRIDE * s
        rec = blk[o:o + SAVE_SLOT_STRIDE]
        if struct.unpack_from("<I", rec, 0)[0] == 1:
            continue
        sess = rec[SAVE_SLOT_SESSION:SAVE_SLOT_SESSION + SAVE_SLOT_SESSION_LEN]
        plyr = rec[SAVE_SLOT_PLAYERS:SAVE_SLOT_PLAYERS + SAVE_SLOT_PLAYERS_LEN]
        print("      --- slot %d session record (guest 0x%08X) ---" % (s, SESSION))
        print("        +0x00 mode=%d  +0x04 venue=%d  +0x05 opts=%#04x  +0x06 players=%d"
              "  +0x08 raceId=%d" % (
                  struct.unpack_from("<I", sess, 0)[0], sess[4], sess[5], sess[6], sess[8]))
        print("        +0x09..0x0B progress=%d,%d,%d  +0x11=%d  +0x20=%d +0x22=%d +0x24=%d"
              " +0x26=%d" % (
                  sess[9], sess[0x0A], sess[0x0B], sess[0x11],
                  struct.unpack_from("<H", sess, 0x20)[0],
                  struct.unpack_from("<H", sess, 0x22)[0],
                  struct.unpack_from("<H", sess, 0x24)[0],
                  struct.unpack_from("<H", sess, 0x26)[0]))
        ai_nz = sum(1 for x in sess[0x40:0xC0] if x)
        bm1 = sess[0xF0:0xF9]
        bm2 = sess[0xFC:0xFF]
        pop = lambda bs, n: [i for i in range(n) if (bs[i >> 3] >> (i & 7)) & 1]
        print("        +0x40..0xBF AI identities: %d non-zero bytes of 128" % ai_nz)
        print("        +0xF0 65-bit bitmap: bits %s" % pop(bm1, 65))
        print("        +0xFC 18-bit bitmap: bits %s" % pop(bm2, 18))
        print("      --- slot %d player records (guest 0x%08X, 6 x 0x24) ---"
              % (s, SESSION_PLAYER))
        print("        k  +0x00       +0x06 +0x07 +0x09 +0x20")
        for k in range(6):
            r = plyr[0x24 * k:0x24 * (k + 1)]
            print("        %d  %10d  %5d %5d %5d %5d" % (
                k, struct.unpack_from("<I", r, 0)[0], r[6], r[7], r[9], r[0x20]))
    tail = blk[SAVE_BYTES:]
    print("      bytes past the %d-byte payload (%d): %d non-zero - the card driver"
          " snapshot the 8192-byte create path writes (0x8005E98C)"
          % (SAVE_BYTES, len(tail), sum(1 for x in tail if x)))


# ---------------------------------------------------------------------------
# verify
# ---------------------------------------------------------------------------

class Bench:
    def __init__(self):
        self.ok = 0
        self.bad = 0

    def check(self, name, cond, detail=""):
        if cond:
            self.ok += 1
            print("  PASS  %s" % name)
        else:
            self.bad += 1
            print("  FAIL  %s  %s" % (name, detail))


def cmd_verify(args):
    ov = Overlay()
    _hdr(ov)
    b = Bench()
    print("\n--- frontend bench ---")

    b.check("RASHCDF sha1 matches the documented image", ov.sha1 == RASHCDF_SHA1, ov.sha1)

    screens = load_screens(ov)
    nav = load_nav(ov)
    a_tab, b_tab, upd, drw = load_screen_handlers(ov)

    b.check("screen table holds 58 records, ids 0..56 and 58",
            sorted(screens) == [i for i in range(SCREEN_COUNT) if i != SCREEN_START_RACE],
            str(sorted(screens)))

    b.check("every screen record's +0x06 equals its table index",
            all(s.rec_id == sid for sid, s in screens.items()))

    b.check("screen id %d has no record (it means 'start the race')" % SCREEN_START_RACE,
            SCREEN_START_RACE not in screens)

    b.check("every screen's widget array fits inside the overlay",
            all(ov.has(s.items, WIDGET_STRIDE * s.count) for s in screens.values()))

    b.check("every screen's parent is -1 or a real screen id",
            all(s.parent == -1 or s.parent in screens for s in screens.values()),
            str([(s.id, s.parent) for s in screens.values()
                 if s.parent != -1 and s.parent not in screens]))

    b.check("every screen's pad window lies in 0..1",
            all(0 <= s.pad_first <= 1 and 1 <= s.pad_count <= 2 and s.pad_first + s.pad_count <= 2
                for s in screens.values()))

    types = sorted({w.type for s in screens.values() for w in s.widgets})
    b.check("every widget type used is in 0..19", types and types[0] >= 0 and types[-1] <= 19,
            str(types))

    b.check("every used widget type except 0 has a draw handler",
            all(drw.get(t) for t in types if t != 0),
            str([t for t in types if t != 0 and not drw.get(t)]))

    codes = sorted({w.code for s in screens.values() for w in s.widgets if w.type in (12, 13)})
    b.check("every action code is < %d" % ACTION_CODE_COUNT,
            codes and codes[-1] < ACTION_CODE_COUNT, str(codes[-8:]))

    fe = loc_info(os.path.join(FE_DIR, "FESTRING.LOC"))
    gm = loc_info(os.path.join(DISC, "DATA", "GAMESTRG.LOC"))
    b.check("FESTRING.LOC is a LOCH/LOCL container with %d strings" % fe["count"],
            fe["chunk_magic"] == "LOCL" and fe["count"] == 1945, str(fe["count"]))
    b.check("GAMESTRG.LOC is a LOCH/LOCL container with %d strings" % gm["count"],
            gm["chunk_magic"] == "LOCL" and gm["count"] == 181, str(gm["count"]))
    for i in (fe, gm):
        b.check("%s: chunkSize + 0x14 == file size" % os.path.basename(i["path"]),
                i["chunk_size"] + 0x14 == i["size"])
        b.check("%s: the offset index ends exactly where the first string starts"
                % os.path.basename(i["path"]),
                i["index_end"] == i["first_off"])
        b.check("%s: every offset is inside the chunk" % os.path.basename(i["path"]),
                all(0 <= o < i["chunk_size"] for o in i["offs"]))

    strids = sorted({w.string_id for s in screens.values() for w in s.widgets
                     if w.type in (12, 13)})
    b.check("every menu label id is a valid FESTRING id",
            strids and strids[-1] < fe["count"], str(strids[-4:]))

    valid_targets = set(screens) | {SCREEN_START_RACE}
    bad = [(k, v) for k, v in nav["code_screen"].items()
           if v not in valid_targets and v != 0xFFFFFFFF]
    b.check("every action-code screen target is a real screen (or 57 = start race)",
            not bad, str(bad))
    bad = [(k, v) for k, v in nav["advance"].items() if v not in valid_targets]
    b.check("every 'advance' target is a real screen", not bad, str(bad))
    bad = [(k, v) for k, v in nav["back"].items() if v not in valid_targets]
    b.check("every 'cancel' target is a real screen", not bad, str(bad))

    b.check("both action-code tables are terminated by -1 at index %d" % (ACTION_CODE_COUNT - 1),
            nav["code_screen"].get(ACTION_CODE_COUNT - 1) == 0xFFFFFFFF
            and nav["code_group"].get(ACTION_CODE_COUNT - 1) == 0xFFFFFFFF)

    names = load_names(ov)
    b.check("the asset-name array has 42 readable names", all(n for n in names), str(names[:3]))
    missing = [n for n in names if not os.path.exists(os.path.join(FE_DIR, n.upper()))]
    b.check("every name in the array names a file in DATA\\FE", not missing, str(missing))

    movie_ids = sorted({struct.unpack_from("<I", w.raw, 0x10)[0]
                        for s in screens.values() for w in s.widgets if w.type == 0})
    wve = [i for i, n in enumerate(names) if n.endswith(".wve")]
    b.check("every movie widget indexes a .wve entry of the name array",
            all(i in wve for i in movie_ids), str(movie_ids))

    res = load_resources(ov)
    missing = [r["index"] for r in res
               if r["name"] and not os.path.exists(os.path.join(FE_DIR, r["name"].upper()))]
    # Records 30..36 name seven per-mode sprite sheets (spr*.psh) that were never
    # shipped: FEMISC.PSH is the only .psh on the disc.  Their flags word is 0x0000
    # and no code takes their address.  This is a shipped-data fact, so the bench
    # asserts it exactly rather than tolerating it.
    b.check("exactly resource records 30..36 name files absent from the disc",
            missing == [30, 31, 32, 33, 34, 35, 36], str(missing))
    b.check("the seven absent records all carry flags 0x0000",
            all(res[i]["flags"] == 0 for i in range(30, 37)))
    b.check("resource classes are 1..6 or the record is nameless",
            all(1 <= r["klass"] <= 6 for r in res))
    b.check("resource records 20..25 are the six result screens",
            [r["name"] for r in res[20:26]] ==
            ["bust.str", "lose.str", "win.str", "wreck.str", "jailed.str", "escape.str"],
            str([r["name"] for r in res[20:26]]))

    modes = load_mode_codes(ov)
    expect = {2: 32, 3: 32, 5: 32, 6: 1, 7: 4, 27: 16, 28: 17, 29: 8, 30: 24, 31: 4}
    b.check("the menu-code -> mode map agrees with rules.md 2.1", modes == expect, str(modes))

    pt = load_prize_table(ov)
    b.check("prize table row 1 is 420/780/780/1260/1260/1260",
            pt[0] == [420, 780, 780, 1260, 1260, 1260], str(pt[0]))
    b.check("prize table places 1..15 fall monotonically in every venue column",
            all(all(pt[r][v] > pt[r + 1][v] for r in range(14)) for v in range(6)))

    # The two bitmap accessors: re-derive their shift/mask constants from the code.
    def bitmap_probe(fn, expect_off):
        # lbu vX, <off>(vY)  where vY = session + (i >> 3)
        for a in range(fn, fn + 0x50, 4):
            w = ov.u32(a)
            if (w >> 26) == 0x24 and _simm(w) == expect_off:
                return True
        return False
    b.check("0x%08X reads session+0xF0 as a byte array" % F_ALL_DONE_A,
            bitmap_probe(F_ALL_DONE_A, 0xF0))
    b.check("0x%08X reads session+0xFC as a byte array" % F_ALL_DONE_B,
            bitmap_probe(F_ALL_DONE_B, 0xFC))

    # The save-block checksum, exercised without needing a card: a *fresh* career
    # record is 484 bytes of zero with rec[0] = 1 ("empty"), and RASHCDF writes its
    # checksum pair before the file is ever created (0x8005EA54 <- 0x8005F1E4).
    # These ten pairs are what the algorithm produces for that input; they are also
    # exactly what a real, never-touched card holds in slots 1..9.
    empty = b"\x01" + bytes(SAVE_SLOT_STRIDE - 1)
    expect_csum = [
        (0x00037335, 0xFFFFFE23), (0x00052CCF, 0xFFFFFC46), (0x0006E669, 0xFFFFFA69),
        (0x0008A003, 0xFFFFF88C), (0x000A599D, 0xFFFFF6AF), (0x000C1337, 0xFFFFF4D2),
        (0x000DCCD1, 0xFFFFF2F5), (0x000F866B, 0xFFFFF118), (0x00114005, 0xFFFFEF3B),
        (0x0012F99F, 0xFFFFED5E),
    ]
    got = [save_checksum(empty, s) for s in range(SAVE_SLOT_COUNT)]
    b.check("the save checksum reproduces the pair of a fresh record in all 10 slots",
            got == expect_csum, str(got[:2]))

    b.check("the block-header template at 0x%08X starts with 'SC'" % HDR_TEMPLATE,
            ov.bytes(HDR_TEMPLATE, 2) == b"SC")
    b.check("the block-header template declares 3 icon frames and block number 1",
            ov.u8(HDR_TEMPLATE + 2) == 0x13 and ov.u8(HDR_TEMPLATE + 3) == 1)
    b.check("the template's three icon frames are byte-identical",
            ov.bytes(HDR_TEMPLATE + 0x80, 0x80) == ov.bytes(HDR_TEMPLATE + 0x100, 0x80)
            == ov.bytes(HDR_TEMPLATE + 0x180, 0x80))
    b.check("the save payload accounts for every byte: 512 + 4 + 1584 + 10*484 + 8 == %d"
            % SAVE_BYTES,
            SAVE_HDR + 4 + SAVE_RECORDS_LEN + SAVE_SLOT_COUNT * SAVE_SLOT_STRIDE + 8
            == SAVE_BYTES)
    b.check("a career record is exactly 216 + 256 + 4 + 2*4 bytes plus its state word",
            4 + SAVE_SLOT_PLAYERS_LEN + SAVE_SLOT_SESSION_LEN + 8 == SAVE_SLOT_STRIDE)

    b.check("the alias keyboard string is 47 characters",
            len(ov.cstr(S_ALIAS_ALPHABET)) == 47, repr(len(ov.cstr(S_ALIAS_ALPHABET))))
    b.check("the memory-card path format is 'bu0%1ld:%s'",
            ov.cstr(S_CARD_PATH_FMT) == "bu0%1ld:%s")
    b.check("the memory-card file name is 20 characters",
            len(ov.cstr(S_CARD_FILE)) == 20, repr(len(ov.cstr(S_CARD_FILE))))

    print("\n%d checks, %d failures" % (b.ok + b.bad, b.bad))
    return 1 if b.bad else 0


# ---------------------------------------------------------------------------
# emit
# ---------------------------------------------------------------------------

def cmd_emit(args):
    out = args.outdir or os.path.join(ROOT, "work", "frontend")
    os.makedirs(out, exist_ok=True)
    ov = Overlay()
    screens = load_screens(ov)
    nav = load_nav(ov)
    a_tab, b_tab, upd, drw = load_screen_handlers(ov)
    names = load_names(ov)
    res = load_resources(ov)
    binds = load_bind_targets(ov)
    modes = load_mode_codes(ov)

    p = os.path.join(out, "screens.csv")
    with open(p, "w", encoding="ascii") as f:
        f.write("id,addr,flags,itemCount,parent,enterAnim,leaveAnim,padFirst,padCount,"
                "itemArray,titleTag,advance,cancel,inputHandler,tickHandler\n")
        for sid in sorted(screens):
            s = screens[sid]
            title = screen_title(s)
            f.write("%d,0x%08X,0x%04X,%d,%d,%d,%d,%d,%d,0x%08X,%s,%s,%s,%s,%s\n" % (
                sid, s.addr, s.flags, s.count, s.parent, s.enter_anim, s.leave_anim,
                s.pad_first, s.pad_count, s.items, title,
                nav["advance"].get(sid, ""), nav["back"].get(sid, ""),
                ("0x%08X" % nav["input"][sid]) if nav["input"].get(sid) else "",
                ("0x%08X" % b_tab[sid]) if b_tab.get(sid) else ""))
    print("wrote %s" % p)

    p = os.path.join(out, "widgets.csv")
    with open(p, "w", encoding="ascii") as f:
        f.write("screen,index,addr,type,typeName,flags,code,stringId,tag,x,y\n")
        for sid in sorted(screens):
            for w in screens[sid].widgets:
                f.write("%d,%d,0x%08X,%d,%s,0x%04X,%s,%s,%s,%d,%d\n" % (
                    sid, w.index, w.addr, w.type, WIDGET_TYPES.get(w.type, "?"), w.flags,
                    w.code if w.type in (12, 13) else "",
                    w.string_id if w.type in (12, 13) else "",
                    w.tag or "", w.x, w.y))
    print("wrote %s" % p)

    p = os.path.join(out, "action_codes.csv")
    with open(p, "w", encoding="ascii") as f:
        f.write("code,targetScreen,groupCommitCode,modeWord,dataObject,usedOnScreens\n")
        owner = {}
        for sid, s in screens.items():
            for w in s.widgets:
                if w.type in (12, 13):
                    owner.setdefault(w.code, []).append(sid)
        for c in range(ACTION_CODE_COUNT):
            t = nav["code_screen"].get(c)
            g = nav["code_group"].get(c)
            f.write("%d,%s,%s,%s,%s,%s\n" % (
                c,
                "" if t is None else ("-1" if t == 0xFFFFFFFF else t),
                "" if g is None else ("-1" if g == 0xFFFFFFFF else g),
                modes.get(c, ""),
                "" if binds.get(c) is None else "0x%08X" % binds[c],
                " ".join(str(x) for x in owner.get(c, []))))
    print("wrote %s" % p)

    p = os.path.join(out, "graph.dot")
    with open(p, "w", encoding="ascii") as f:
        f.write("digraph frontend {\n  rankdir=LR;\n  node [shape=box,fontsize=9];\n")
        f.write("  s57 [label=\"57\\nSTART RACE\",shape=doubleoctagon];\n")
        for sid in sorted(screens):
            s = screens[sid]
            title = screen_title(s)
            mv = next((struct.unpack_from("<I", w.raw, 0x10)[0]
                       for w in s.widgets if w.type == 0), None)
            if mv is not None and mv < len(names):
                label = "%d\\n%s" % (sid, names[mv])
                f.write("  s%d [label=\"%s\",shape=ellipse];\n" % (sid, label))
            else:
                f.write("  s%d [label=\"%d\\n%s\"];\n" % (sid, sid, title))
        for sid in sorted(screens):
            a = nav["advance"].get(sid)
            if a is not None:
                f.write("  s%d -> s%d [style=dashed,label=\"advance\"];\n" % (sid, a))
            bk = nav["back"].get(sid)
            if bk is not None:
                f.write("  s%d -> s%d [style=dotted,label=\"back\"];\n" % (sid, bk))
            for w in screens[sid].widgets:
                if w.type in (12, 13):
                    t = nav["code_screen"].get(w.code)
                    if t is not None and t != 0xFFFFFFFF:
                        f.write("  s%d -> s%d [label=\"%d\"];\n" % (sid, t, w.code))
        f.write("}\n")
    print("wrote %s" % p)

    p = os.path.join(out, "graph.txt")
    with open(p, "w", encoding="ascii") as f:
        f.write("Road Rash: Jailbreak (USA) - frontend screen graph\n")
        f.write("built from RASHCDF %s by frontend.py emit\n\n" % ov.sha1)
        f.write("Legend:  ==>  a menu button (its action code in brackets)\n")
        f.write("         -->  the 'advance' table 0x%08X[screen]\n" % T_NAV_ADVANCE)
        f.write("         <--  the 'cancel'  table 0x%08X[screen]\n\n" % T_NAV_BACK)
        for sid in sorted(screens):
            sc = screens[sid]
            title = screen_title(sc)
            mv = next((struct.unpack_from("<I", w.raw, 0x10)[0]
                       for w in sc.widgets if w.type == 0), None)
            kind = ("movie %s" % names[mv]) if (mv is not None and mv < len(names)) \
                else (title or "panel")
            f.write("screen %-3d %-22s items=%-3d parent=%-3d pads=%d+%d\n" % (
                sid, kind, sc.count, sc.parent, sc.pad_first, sc.pad_count))
            a = nav["advance"].get(sid)
            if a is not None:
                f.write("            --> %d\n" % a)
            bk = nav["back"].get(sid)
            if bk is not None:
                f.write("            <-- %d\n" % bk)
            for w in sc.widgets:
                if w.type not in (12, 13):
                    continue
                t = nav["code_screen"].get(w.code)
                g = nav["code_group"].get(w.code)
                if t is not None and t != 0xFFFFFFFF:
                    dest = "==> %d%s" % (
                        t, "  (START THE RACE)" if t == SCREEN_START_RACE else "")
                elif g is not None and g != 0xFFFFFFFF:
                    dest = "(chooser; confirm jumps to the item with code %d)" % g
                else:
                    dest = "(chooser, edits in place)"
                f.write("            [%2d] type %2d  str %-4d %s\n" % (
                    w.code, w.type, w.string_id, dest))
            f.write("\n")
        f.write("screen %-3d %-22s no record: leaving the frontend starts the race\n" % (
            SCREEN_START_RACE, "START RACE"))
    print("wrote %s" % p)

    p = os.path.join(out, "resources.csv")
    with open(p, "w", encoding="ascii") as f:
        f.write("index,addr,flags,class,idInClass,name\n")
        for r in res:
            f.write("%d,0x%08X,0x%04X,%d,%d,%s\n" % (
                r["index"], r["addr"], r["flags"], r["klass"], r["id"], r["name"]))
        f.write("\n# asset name array at 0x%08X\n" % T_NAMES)
        f.write("nameIndex,name\n")
        for i, n in enumerate(names):
            f.write("%d,%s\n" % (i, n))
    print("wrote %s" % p)

    p = os.path.join(out, "prizes.csv")
    with open(p, "w", encoding="ascii") as f:
        f.write("row,venue0,venue1,venue2,venue3,venue4,venue5\n")
        for r, row in enumerate(load_prize_table(ov)):
            f.write("%d,%s\n" % (r, ",".join(str(x) for x in row)))
    print("wrote %s" % p)
    return 0


def build_parser():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd")
    a = sub.add_parser("info")
    a.add_argument("what", nargs="?",
                   choices=["all", "screens", "graph", "widgets", "codes", "res",
                            "strings", "career", "save"])
    a.set_defaults(func=cmd_info)
    a = sub.add_parser("scan")
    a.add_argument("card")
    a.set_defaults(func=cmd_scan)
    a = sub.add_parser("verify")
    a.set_defaults(func=cmd_verify)
    a = sub.add_parser("emit")
    a.add_argument("outdir", nargs="?")
    a.set_defaults(func=cmd_emit)
    return ap


def main(argv=None):
    ap = build_parser()
    args = ap.parse_args(argv)
    if not getattr(args, "func", None):
        ap.print_help()
        return 2
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
