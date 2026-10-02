"""Scout probe for THE RACE HUD of Road Rash: Jailbreak (USA, SLUS_01053).

Checks the HUD's documented behaviour against the player's own disc extract, the captured RAM
images and the frame traces, independently of the C++ tree (a second, Python reading of the same
instructions):

  * `hashes`    - SLUS_010.53 / RASHCDG.BIN / RASHCDI.BIN are the images the reading was made on;
  * `skeleton`  - HudFrame's 33 call sites and their targets, decoded from the instruction words;
  * `gates`     - the constants each documented decision rests on, decoded at their addresses (the
                  split-screen background items 105..108, the countdown's six sound ids, the bar
                  thresholds 33/65/97, the wrong-way threshold 0xC000, the sign codes and items, the
                  mph factor 0x23CA7, the HUD enable flag and the countdown digit art 57);
  * `tables`    - the item-kind packet lengths, the bar colours, the radar mark colours (RASHCDG data)
                  and the graph tiles' colour (RASHCDI data);
  * `loader`    - a Python transcription of the layout loader (DASH1P.CSV through RASHCDI 0x8005F104 /
                  0x8005F1E4 / 0x8005F9EC / 0x8005EFFC / SLUS 0x80013AF8) rebuilds the art table, the
                  texture table and every item's layout fields, compared with all 17 race RAM images;
  * `values`    - in every image, the item records hold exactly the art the last-drawn values of the
                  per-player state record 0x800D4A60 select (speed, odometer, place, radar distance)
                  and the health bars' tiles have the width and colour their values select;
  * `slides`    - in every image, the two panels' item rows sit at the CSV y plus the offset their
                  slide record's state says;
  * `trace`     - for each frame trace (rrverify trace), the set of item records the
                  ORIGINAL linked that frame equals what a Python model of HudFrame's gates predicts
                  from the state at HudFrame's entry and exit (replayed from the trace's own stores).

`verify --mutate` perturbs each claim's model and requires that claim's OWN check to fail.

Reads ONLY work\\disc_us, work\\oracle\\state, work\\oracle\\vr_capture\\ramdumps and work\\hud7
(all gitignored). Never copies game bytes into the repo.

Usage (from the project root):

    python tools\\scout\\hud.py verify
    python tools\\scout\\hud.py verify --mutate

`verify` ends with a single line a gate can match:

    hud: <n> checks, <m> failures
"""

from __future__ import annotations

import argparse
import csv
import glob
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import exe as EX  # noqa: E402

ROOT = EX.ROOT
DISC = EX.DISC
OVL = 0x8005B5E8
SHA1 = {
    "SLUS_010.53": "67ed165a2c517d4e6106fb0dfa324d66dd9a76f1",
    "RASHCDG.BIN": "cfe43a7786759f2cb9c57751cf99e84d1074782c",
    "RASHCDI.BIN": "9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06",
}
TRACES = ("rr-race", "rr-pack", "quick")

# globals
ITEMS_PTR, OT, FRAME_COUNT, SPLIT = 0x8005ACEC, 0x8005B590, 0x8005ACDC, 0x8005AD14
PLAYER_BIKE, FINISHED, DEMO, TARGET = 0x8005B268, 0x8005B288, 0x8005B220, 0x8005AD0C
FIELD, GS_PTR = 0x8005B1FC, 0x8005B2F8
ARTS, TEXTURES = 0x800D45D0, 0x800D49B0
STATE88, DASH224, SLIDE72 = 0x800D4A60, 0x800D6198, 0x800D6358
PLAYER_RECS = 0x800D81D8

MUT: set[str] = set()
MUTANTS = {
    "skeleton_target": "skeleton", "blue_box_first": "gates", "countdown_cue": "gates",
    "bar_threshold": "gates", "len_table": "tables", "bar_colour": "tables",
    "loader_no_origin": "loader", "loader_tile_art": "loader", "speed_factor": "values",
    "odo_tenths": "values", "place_art": "values", "bar_width_half": "values", "slide_sign": "slides",
    "gate_no_pslide": "trace", "gate_star_always": "trace",
}


def mut(n: str) -> bool:
    return n in MUT


def u32(b, a):
    return struct.unpack_from("<I", b, a & 0x1FFFFF)[0]


def s32(b, a):
    return struct.unpack_from("<i", b, a & 0x1FFFFF)[0]


def u16(b, a):
    return struct.unpack_from("<H", b, a & 0x1FFFFF)[0]


def s16(b, a):
    return struct.unpack_from("<h", b, a & 0x1FFFFF)[0]


def u8(b, a):
    return b[a & 0x1FFFFF]


def s8(b, a):
    v = b[a & 0x1FFFFF]
    return v - 256 if v & 0x80 else v


def fixmul(a, b):
    return ((a * b) >> 16) & 0xFFFFFFFF


def sx(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


# ---------------------------------------------------------------------------- inputs
def load():
    ctx = {}
    ctx["slus"] = EX.load_exe()
    ctx["g"] = EX.load_overlay("RASHCDG.BIN", OVL)
    ctx["i"] = EX.load_overlay("RASHCDI.BIN", OVL)
    ctx["csv"] = open(os.path.join(DISC, "DATA", "DASH1P.CSV"), "rb").read()
    images = {}
    files = sorted(glob.glob(os.path.join(ROOT, "work", "oracle", "state", "*", "ram.bin")))
    files += sorted(glob.glob(os.path.join(ROOT, "work", "oracle", "vr_capture", "ramdumps", "*.bin")))
    for f in files:
        b = open(f, "rb").read()
        if len(b) < 0x200000 or u32(b, ITEMS_PTR) != 0x8010449C:
            continue
        images[os.path.relpath(f, ROOT)] = b
    ctx["images"] = images
    return ctx


def word(img, a):
    return img.word(a)


# ---------------------------------------------------------------------------- claims
EXPECTED_JALS = [
    (0x8005E9FC, 0x8003C590), (0x8005EB00, 0x80062F34), (0x8005EB14, 0x8005FE58), (0x8005EB68, 0x80062D9C),
    (0x8005EB80, 0x8005FF84), (0x8005EB98, 0x80062368), (0x8005EBEC, 0x80062C40), (0x8005EC10, 0x80060178),
    (0x8005EC24, 0x800603E4), (0x8005EC38, 0x8005FA68), (0x8005EC68, 0x80061F6C), (0x8005ECAC, 0x8005FAC4),
    (0x8005ECB8, 0x800C52A0), (0x8005ECCC, 0x8005FA68), (0x8005ECE0, 0x8005FA68), (0x8005ED08, 0x80062610),
    (0x8005ED28, 0x8005F030), (0x8005EDB8, 0x80063530), (0x8005EDE4, 0x800636F0), (0x8005EE20, 0x8005F030),
    (0x8005EE40, 0x800606F0), (0x8005EE4C, 0x800C52A0), (0x8005EE60, 0x8005FA68), (0x8005EE74, 0x8005FA68),
    (0x8005EE94, 0x8006148C), (0x8005EEAC, 0x80063408), (0x8005EECC, 0x80060C10), (0x8005EEEC, 0x80061E50),
    (0x8005EF00, 0x8005FE58), (0x8005EF20, 0x8005FB4C), (0x8005EF5C, 0x8005FA68), (0x8005EF70, 0x8005FA68),
    (0x8005EFD0, 0x8004CE14),
]


def c_hashes(ctx):
    f = []
    for k, img in (("SLUS_010.53", ctx["slus"]), ("RASHCDG.BIN", ctx["g"]), ("RASHCDI.BIN", ctx["i"])):
        if img.sha1 != SHA1[k]:
            f.append(f"{k} SHA-1 {img.sha1} != {SHA1[k]}")
    return 3, f


def c_skeleton(ctx):
    g = ctx["g"]
    got = []
    for a in range(0x8005E848, 0x8005F030, 4):
        w = word(g, a)
        if (w >> 26) == 3:
            got.append((a, ((w & 0x3FFFFFF) << 2) | 0x80000000))
    exp = list(EXPECTED_JALS)
    if mut("skeleton_target"):
        exp[4] = (exp[4][0], 0x80060178)
    return 1, ([] if got == exp else [f"HudFrame's call sites differ: {len(got)} decoded, first mismatch "
                                      f"{next((x for x in zip(got, exp) if x[0] != x[1]), None)}"])


def imm_at(img, a):
    w = img.word(a)
    return (w >> 26) & 0x3F, (w >> 21) & 0x1F, (w >> 16) & 0x1F, w & 0xFFFF


def c_gates(ctx):
    g, x = ctx["g"], ctx["slus"]
    # (image, address, opcode, immediate, what)
    table = [
        (g, 0x8005EF38, 0x0C, 0x10, "split-screen bit of the race type (andi 0x10)"),
        (g, 0x8005EF48, 0x09, 2, "split layout 2 (li t1,2)"),
        (g, 0x8005EF58, 0x09, 106 if mut("blue_box_first") else 105, "first background item (li a2,105)"),
        (g, 0x8005EF60, 0x09, 108, "last background item (li a3,108)"),
        (g, 0x80060074, 0x09, 77 if mut("countdown_cue") else 76, "countdown cue 1"),
        (g, 0x80060088, 0x09, 75, "countdown cue 2"),
        (g, 0x8006009C, 0x09, 29, "countdown cue 3"),
        (g, 0x8006011C, 0x09, 65, "end-of-countdown cue 1"),
        (g, 0x80060130, 0x09, 106, "end-of-countdown cue 2"),
        (g, 0x80060144, 0x09, 80, "end-of-countdown cue 3"),
        (g, 0x800610A0, 0x0A, 34 if mut("bar_threshold") else 33, "bar red below 33 (slti)"),
        (g, 0x800610EC, 0x0A, 65, "bar yellow below 65 (slti)"),
        (g, 0x800610F8, 0x0A, 97, "bar green below 97 (slti)"),
        (x, 0x8003C630, 0x0D, 0xC000, "wrong-way |dot| threshold (ori 0xC000)"),
        (g, 0x8005FBFC, 0x09, 4, "sign code 4 = wrong way"),
        (g, 0x8005FC10, 0x09, 3, "sign code 3 = turn right"),
        (g, 0x8005FDAC, 0x09, 81, "wrong-way item 81"),
        (g, 0x8005FDB8, 0x09, 83, "turn-right item 83"),
        (g, 0x8005FDBC, 0x09, 82, "turn-left item 82"),
        (g, 0x800601C4, 0x0F, 0x0002, "mph factor, high half"),
        (g, 0x800601CC, 0x0D, 0x3CA7, "mph factor, low half (0x23CA7)"),
        (g, 0x80060000, 0x09, 18784, "countdown digit art base 0x800D4960 = art 57 (addiu)"),
    ]
    f = []
    for img, a, op, imm, what in table:
        o, _rs, _rt, i = imm_at(img, a)
        if o != op or i != imm:
            f.append(f"{what}: at 0x{a:08X} op 0x{o:02X} imm 0x{i:X}, documented op 0x{op:02X} imm 0x{imm:X}")
    return len(table), f


def c_tables(ctx):
    g, i = ctx["g"], ctx["i"]
    f = []
    lens = [word(g, 0x800CC68C + 4 * k) for k in range(3)]
    exp = [4, 3, 3] if mut("len_table") else [4, 3, 2]
    if lens != exp:
        f.append(f"packet lengths by item kind {lens} != {exp}")
    cols = [word(g, 0x800CC698 + 4 * k) for k in range(3)]
    expc = [0x1818A5, 0x29D6D6, 0x187B29]
    if mut("bar_colour"):
        expc = expc[::-1]
    if cols != expc:
        f.append(f"bar colours {[hex(c) for c in cols]} != {[hex(c) for c in expc]}")
    marks = [word(g, 0x800CCBE0 + 4 * k) for k in range(6)]
    if marks[:3] != [0xFFFFFF, 0xFF00FF, 0x00FFFF]:
        f.append(f"radar mark colours {[hex(c) for c in marks]}")
    graph = i.data[0x8005B738 - OVL:0x8005B738 - OVL + 3]
    if graph != bytes([0x63, 0x84, 0x94]):
        f.append(f"graph tile colour {graph.hex()}")
    return 4, f


# ---------------------------------------------------------------------------- the loader model
def tokenize(b, at):
    """RASHCDI 0x8005F104."""
    fields, n, p = [], 0, at
    c = b[p] if p < len(b) else 0
    if c != 13:
        while n < 80:
            if c == 44:
                fields.append(p + 1)
            p += 1
            c = b[p] if p < len(b) else 0
            n += 1
            if c == 13:
                break

    def atoi(q):
        s = b""
        while q < len(b) and b[q] not in (44, 13, 10):
            s += bytes([b[q]])
            q += 1
        s = s.strip()
        try:
            return int(s)
        except ValueError:
            return 0

    out = [(atoi(fields[k]) & 0xFFFF) if k < len(fields) else 0 for k in range(5)]
    return out, n + 2


def loader(csvb):
    """The layout loader's static results: textures, arts, items (x, y, kind, art, semi)."""
    at = 0
    f, k = tokenize(csvb, at)
    at += k
    clut_x, clut_y = f[0], f[1]
    for _ in range(7 + 4):  # SignFlash .. OSlide, and the four placement lines
        _f, k = tokenize(csvb, at)
        at += k
    f, k = tokenize(csvb, at)
    at += k
    ntex = f[0]
    tex = []
    for t in range(ntex):
        f, k = tokenize(csvb, at)
        at += k
        x, y = f[0] & 0xFF, f[1] & 0xFF
        page = ((((x >> 2) + 960) & 0x3FF) >> 6)
        cx = ((clut_x + ((t & 3) << 6)) >> 2) + 960
        cy = clut_y + (t >> 2)
        clut = ((cy << 6) | ((cx >> 4) & 0x3F)) & 0xFFFF
        tex.append((page, clut, x, y, f[2]))
    arts = []
    for a in range(62):
        f, k = tokenize(csvb, at)
        at += k
        ti = f[4]
        page, clut, tx, ty, _ = tex[ti]
        if mut("loader_no_origin"):
            tx = ty = 0
        x, y, w, h = f[0] & 0xFF, f[1] & 0xFF, f[2] & 0xFF, f[3] & 0xFF
        r = bytearray(16)
        struct.pack_into("<H", r, 0, ti)
        r[2], r[3] = w, h
        r[4], r[5] = (x + tx) & 0xFF, (y + ty) & 0xFF
        struct.pack_into("<H", r, 6, clut)
        r[8], r[9] = (x + w - 1 + tx) & 0xFF, (y + ty) & 0xFF
        struct.pack_into("<H", r, 10, page)
        r[12], r[13] = (x + tx) & 0xFF, (y + h - 1 + ty) & 0xFF
        r[14], r[15] = (x + w - 1 + tx) & 0xFF, (y + h - 1 + ty) & 0xFF
        arts.append(bytes(r))
    items = []
    for it in range(110):
        f, k = tokenize(csvb, at)
        at += k
        kind = f[4] & 0xFF
        art = f[2] & 0xFF if kind == 0 else 0xFF
        if mut("loader_tile_art") and kind == 1:
            art = f[2] & 0xFF
        semi = f[3] & 1 if kind != 2 else 0
        items.append((f[0], f[1], kind, art, semi))
    return tex, arts, items


def slide_offsets(b):
    """The y offset each panel's slide record says its rows carry."""
    out = []
    for k in range(2):
        s = SLIDE72 + 36 * k
        st, cnt, full, sign = s32(b, s), s32(b, s + 4), s32(b, s + 20), s32(b, s + 24)
        off = full * sign if st == 2 else (cnt * sign if st in (1, 3) else 0)
        if mut("slide_sign"):
            off = -off
        out.append((s32(b, s + 28), s32(b, s + 32), off))
    return out


def c_loader(ctx):
    tex, arts, items = loader(ctx["csv"])
    f, n = [], 0
    for name, b in ctx["images"].items():
        n += 1
        bad = []
        for t, (page, clut, x, y, _h) in enumerate(tex):
            a = TEXTURES + 8 * t
            if (u16(b, a), u16(b, a + 2), u8(b, a + 4), u8(b, a + 5)) != (page, clut, x, y):
                bad.append(f"texture {t}")
        for k, r in enumerate(arts):
            got = bytes(b[(ARTS + 16 * k) & 0x1FFFFF:(ARTS + 16 * k + 16) & 0x1FFFFF])
            if got != r:
                bad.append(f"art {k}")
        offs = slide_offsets(b)
        for it, (x, y, kind, art, semi) in enumerate(items):
            a = 0x8010449C + 36 * it
            dy = sum(o for lo, hi, o in offs if lo <= it <= hi)
            if (u16(b, a + 24), (u16(b, a + 26) - dy) & 0xFFFF, u8(b, a + 32), u8(b, a + 33), u16(b, a + 34)) != \
                    (x, y & 0xFFFF, kind, art, semi):
                bad.append(f"item {it}")
        if bad:
            f.append(f"{name}: {len(bad)} records differ from the loader model, first {bad[0]}")
    return n, f


# ---------------------------------------------------------------------------- values -> packets
def art_ptr(i):
    return ARTS + 16 * i


def uvw(b, art):
    """The packet word a sprite item takes from art `art`: its u, v and CLUT (art +4). The inline
    set-art of every element writes this word (+0x0C) and the size, NOT the item's art pointer +0x14 -
    only SetArtFull (SLUS 0x80013AF8) does that."""
    return u32(b, art_ptr(art) + 4)


def got_uv(b, items, i):
    return u32(b, items + 36 * i + 12)


def div(a, b):
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b > 0) else -q


def c_values(ctx):
    f, n = [], 0
    for name, b in ctx["images"].items():
        items = 0x8010449C
        st = STATE88
        # speed: the last v at +8, mph = FixMul(v, 0x23CA7) clamped at 0, three BigNum digits (art 5+d)
        v = s32(b, st + 8)
        mph = sx(fixmul(v, 0x10000 if mut("speed_factor") else 0x23CA7))
        mph = max(mph, 0)
        h = div(mph, 100) >> 8
        mph -= h * 25600
        t = div(mph, 10) >> 8
        mph -= t * 2560
        o = mph >> 8
        exp = [uvw(b, 5 + h), uvw(b, 5 + t), uvw(b, 5 + o)]
        got = [got_uv(b, items, i) for i in (10, 11, 12)]
        n += 1
        if got != exp:
            f.append(f"{name}: speed digits {got} != {exp} (v {v})")
        # odometer: the last value at +12 (FixMul(distance, 40)), modulo 100.00, digits d.d.
        w = s32(b, st + 12)
        if w >= 25600:
            w -= div(w, 25600) * 25600
        tens = div(w, 10) >> 8
        w -= tens * 2560
        ones = w >> 8
        w -= ones << 8
        tenths = (w * 10) >> (7 if mut("odo_tenths") else 8)
        tens, ones, tenths = [d if d < 10 else 0 for d in (tens, ones, tenths)]
        exp = [uvw(b, 20 + tens), uvw(b, 20 + ones), uvw(b, 53), uvw(b, 20 + tenths)]
        got = [got_uv(b, items, i) for i in (13, 14, 15, 16)]
        n += 1
        if got != exp:
            f.append(f"{name}: odometer digits {got} != {exp}")
        # place: the last value at +0, BigNum ones in item 46 (tens in 45 when >= 10)
        p = s32(b, st)
        base = 6 if mut("place_art") else 5
        exp = uvw(b, base + (p - 10 if p >= 10 else p))
        n += 1
        if got_uv(b, items, 46) != exp:
            f.append(f"{name}: place ones {got_uv(b, items, 46):08X} != {exp:08X} (place {p})")
        # radar distance: the last value at +16, whole / tenths / hundredths in items 96..98
        a = s32(b, st + 16)
        whole = a >> 16
        frac = a - (whole << 16)
        tenth = (frac * 10) >> 16
        frac -= tenth * 6553
        hund = (frac * 100) >> 16
        hund = 0 if hund >= 10 else hund
        exp = [uvw(b, 20 + whole), uvw(b, 20 + tenth), uvw(b, 20 + hund)]
        got = [got_uv(b, items, i) for i in (96, 97, 98)]
        n += 1
        if got != exp:
            f.append(f"{name}: radar distance digits {got} != {exp}")
        # the bars: tile width = max(value >> 2, 4) (0 when the red flash is off), colour by threshold
        opponent_shown = u32(b, st + 64) != 0 and u32(b, SLIDE72 + 36) != 2
        for item, off in ((20, 24), (23, 28), (34, 40), (37, 44)):
            if item in (34, 37) and not opponent_shown:
                continue  # the opponent's bars are redrawn only while its panel shows
            val = s32(b, st + off)
            flash_on = s32(b, DASH224 + 208)
            shade = (1 - flash_on) if val < 33 else (1 if val < 65 else (2 if val < 97 else 3))
            width = 0 if shade == 0 else max(val >> (1 if mut("bar_width_half") else 2), 4)
            idx = shade - 1 if shade > 0 else 0
            colour = [0x1818A5, 0x29D6D6, 0x187B29][idx]
            got_w = u16(b, items + 36 * item + 28)
            got_c = u32(b, items + 36 * item + 4) & 0xFFFFFF
            n += 1
            if (got_w, got_c) != (width, colour):
                f.append(f"{name}: bar item {item} width/colour {got_w}/{got_c:06X} != {width}/{colour:06X} (value {val})")
    return n, f


def c_slides(ctx):
    _tex, _arts, items = loader(ctx["csv"])
    f, n = [], 0
    for name, b in ctx["images"].items():
        for lo, hi, off in slide_offsets(b):
            n += 1
            bad = [it for it in range(lo, hi + 1)
                   if (u16(b, 0x8010449C + 36 * it + 26) - items[it][1]) & 0xFFFF != off & 0xFFFF]
            if bad:
                f.append(f"{name}: panel items {lo}..{hi}: {len(bad)} rows not at CSV y + {off}")
    return n, f


# ---------------------------------------------------------------------------- the trace
def replay(ram, trace, upto):
    b = bytearray(ram)
    with open(os.path.join(trace, "watch.csv")) as fh:
        for r in csv.DictReader(fh):
            if int(r["seq"]) >= upto:
                break
            if r["kind"] != "write":
                continue
            a, size, v = int(r["address"], 16) & 0x1FFFFF, int(r["size"]), int(r["value"], 16)
            for k in range(size):
                b[a + k] = (v >> (8 * k)) & 0xFF
    return bytes(b)


def gate_model(pre, post):
    """Which item records a normal-race HudFrame links, from the state it saw."""
    gs = u32(post, GS_PTR)
    typ, state = u8(post, gs + 4), s8(post, gs)
    bike = u32(post, PLAYER_BIKE)
    rd = u32(post, bike + 1084)
    items = {0}
    if state == 6:
        return items
    if s32(post, DEMO) != 0 or u32(post, FINISHED) != 0 or u32(post, rd + 40) != 0:
        return items  # the messages path draws text only
    if (u32(post, rd) & 0x60) == 0x40:
        items.add(52)
    held = ((u32(post, bike + 564) >> 9) & 1) + s8(post, bike + 848)
    cap = min(u8(post, PLAYER_RECS + 36 * u16(post, bike + 172) + 20), held)
    shown = min(u8(post, u32(post, bike + 556) + 445) + s8(post, 0x800D80F5) + cap, 8)
    if shown > 0:
        items |= set(range(84, 84 + shown))
    items |= {9} | set(range(10, 13)) | set(range(13, 17))
    target = u32(pre, TARGET)  # the radar distance runs before this frame's target pick
    if not typ & 4 and target:
        items |= set(range(95, 100))
    # place and field
    fin = u32(post, FINISHED)
    if not typ & 4 and s32(post, DEMO) == 0 and (fin == 0 or u8(post, rd + 39) == 255):
        place = s32(post, STATE88)
        items |= {46} | ({45} if place >= 10 else set())
        items |= {47, 49} | ({48} if s32(post, FIELD) >= 10 else set())
    items |= {8, 104}
    # the two panels: the opponent's is linked BEFORE this frame's slide step, the player's after it
    s = SLIDE72
    if u32(post, STATE88 + 64) and (u32(pre, s + 36) != 2):
        tb = u32(post, STATE88 + 64)
        trd = u32(post, tb + 1084)
        star = mut("gate_star_always") or (u8(post, trd + 47) and u8(post, trd + 46) != 9)
        items |= set(range(32, 39)) | ({39} if star else set())
    if mut("gate_no_pslide") or u32(post, s) != 2:
        items |= set(range(18, 25)) | ({25} if u8(post, rd + 47) and u8(post, rd + 46) != 9 else set())
    # the sign
    code = s32(post, STATE88 + 60)
    if code and u32(post, DASH224 + 16):
        el = 81 if code in (4, 5) else (82 if code == 1 else 83)
        if not (s32(post, DASH224 + 12) < 0 and el == 81):
            items.add(el)
    return items


def traced_items(trace, exit_seq, items_base):
    got = set()
    heap = 0
    first = None
    with open(os.path.join(trace, "prims.csv")) as fh:
        for r in csv.DictReader(fh):
            seq = int(r["seq"])
            if seq <= exit_seq:
                continue
            if first is None:
                first = seq
            if seq != first:
                break
            a = int(r["srcAddr"], 16) - 4
            if items_base <= a < items_base + 3960 and (a - items_base) % 36 == 0:
                got.add((a - items_base) // 36)
            elif 0x800D9000 <= a < 0x80100000:
                heap += 1
    return got, heap


def c_trace(ctx):
    f, n = [], 0
    for st in TRACES:
        trace = os.path.join(ROOT, "work", "hud7", "trace_" + st)
        ram_path = os.path.join(ROOT, "work", "oracle", "state", st, "ram.bin")
        if not os.path.exists(os.path.join(trace, "probes.csv")):
            f.append(f"{st}: no trace under {trace}")
            n += 1
            continue
        seqs = {}
        with open(os.path.join(trace, "probes.csv")) as fh:
            for r in csv.DictReader(fh):
                seqs.setdefault(r["name"], int(r["seq"]))
        ram = open(ram_path, "rb").read()
        pre = replay(ram, trace, seqs["hud_entry"])
        post = replay(ram, trace, seqs["hud_exit"])
        exp = gate_model(pre, post)
        got, heap = traced_items(trace, seqs["hud_exit"], 0x8010449C)
        n += 1
        if got != exp:
            f.append(f"{st}: the original linked items {sorted(got)}, the gate model says {sorted(exp)}")
    return n, f


CLAIMS = {
    "hashes": c_hashes, "skeleton": c_skeleton, "gates": c_gates, "tables": c_tables, "loader": c_loader,
    "values": c_values, "slides": c_slides, "trace": c_trace,
}


def verify(ctx, only=None):
    total, fails = 0, []
    for name, fn in CLAIMS.items():
        if only and name != only:
            continue
        n, f = fn(ctx)
        total += n
        fails += [f"{name}: {x}" for x in f]
    return total, fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["verify"])
    ap.add_argument("--mutate", action="store_true")
    a = ap.parse_args()
    ctx = load()
    print(f"hud: {len(ctx['images'])} race RAM images")
    if a.mutate:
        caught = 0
        for m_, owner in MUTANTS.items():
            MUT.clear()
            MUT.add(m_)
            _n, fl = verify(ctx, owner)
            own = [x for x in fl if x.startswith(owner + ": ")]
            print(f"  mutant {m_:24s} {'CAUGHT by ' + owner if own else 'MISSED'}  {own[:1]}")
            caught += 1 if own else 0
        MUT.clear()
        print(f"hud --mutate: {caught} of {len(MUTANTS)} mutants caught by their own claim, "
              f"{len(MUTANTS) - caught} missed")
        return 0 if caught == len(MUTANTS) else 1
    n, fl = verify(ctx)
    for x in fl:
        print("  FAIL", x)
    print(f"hud: {n} checks, {len(fl)} failures")
    return 0 if not fl else 1


if __name__ == "__main__":
    sys.exit(main())
