#!/usr/bin/env python3
"""Scout probe for the 0x4000 world chunk of Road Rash: Jailbreak (PS1).

Decodes the stream chunk found in DATA\\STREAM<n>.STR and DATA\\RACE<s>_<r>.STP.
Everything here is derived from our own extract of the player's disc and from our
own disassembly; see docs\\formats\\road_chunk.md for the provenance of every
field.

Subcommands
    info   <file> [chunk]        decode one chunk (or list all chunks of a file)
    scan   <dir>                 classify every chunk of every .STR/.STP in dir
    road   <file> <out> [ids..]  export road geometry (.svg = top down centre
                                 line, .obj = road surface)
    route  <dir> <set> <race> <out.svg>
                                 draw one race's route on the network
    verify <dir>                 re-runnable consistency checks

No game bytes are ever written into the repo: every output path must be under
work\\ (the caller's responsibility).
"""
from __future__ import annotations

import math
import os
import struct
import sys
import zlib
from collections import Counter, defaultdict

CHUNK = 0x4000

# ---------------------------------------------------------------------------
# chunk header
# ---------------------------------------------------------------------------
# u32 key            resource key: type = key >> 28, id = key & 0x0FFFFFFF,
#                    id = (group << 16) | index, group a multiple of 0x80.
# { u16 road; u16 fromWorld; u16 toWorld; }*      residency windows
# 0xFFFF padding, terminated by 0xFFFE 0xFFFF
#
# The 11 resource types come from the dispatcher at 0x80031604 (EXE), which does
# `srl s4,a0,0x1c` at 0x8003168c and jumps through the 11-entry table at
# 0x80010D48.
TYPE_NAME = {
    0: "geom0",      # -> 0x80031784 -> 0x80032a20
    1: "tex",        # -> 0x80031734 -> 0x80032cc8 -> 0x80034dec
    2: "tex_sprite",  # -> 0x80031704 -> 0x80032c6c -> 0x80034dec (| 0x8000)
    3: "road",       # -> 0x800317c4 -> 0x8003cec4   <-- the road geometry
    4: "pano",       # -> 0x800316d0 -> 0x800136bc   (checks 'PANO')
    5: "invalid",
    6: "invalid",
    7: "invalid",
    8: "geom8",      # -> 0x80031784 -> 0x80032a20
    9: "geom9",      # -> 0x8003176c -> 0x80032a20
    10: "adpcm",     # -> 0x800317ec -> 0x8001a0c0
}

# Payload start per type, from the helper at 0x80031560:
#   types 1,2       : chunk + 0x0C   (raw image bytes, no sub header)
#   types 0,8       : chunk + 0x20, word[0] = count of u32 in the block
#   everything else : chunk + 0x20
ROAD_BLOCKS_AT = 0x8C          # type 3: 0x20 + 0x6C fixed header

# type 3 sub-block record sizes (proven: every one divides size-8 for all 113
# distinct type-3 objects of set 1 and all 96 of set 2)
REC = {
    "GRPT": 32, "SUBT": 28, "SLCT": 52, "XSIH": 16, "XSDH": 44, "XSAI": 20,
    "DIST": 4, "SEG_": 20, "BGDT": 20, "BSDT": 40, "BZDT": 4, "NMBD": 4,
}

FX = 65536.0        # world coordinates are 16.16 fixed point
ROAD_UNIT = 1024    # 1 RGTS/PMTS road unit = 1024 raw = 1/64 world unit


# ---------------------------------------------------------------------------
# low level
# ---------------------------------------------------------------------------
def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def i32(b, o):
    return struct.unpack_from("<i", b, o)[0]


def chunk_count(path):
    sz = os.path.getsize(path)
    base = 0x800 if is_stp(path) else 0
    return (sz - base) // CHUNK


def is_stp(path):
    with open(path, "rb") as f:
        return f.read(4) == b"MRPS"


def read_chunk(path, i):
    base = 0x800 if is_stp(path) else 0
    with open(path, "rb") as f:
        f.seek(base + i * CHUNK)
        return f.read(CHUNK)


def iter_chunks(path):
    base = 0x800 if is_stp(path) else 0
    with open(path, "rb") as f:
        f.seek(base)
        i = 0
        while True:
            b = f.read(CHUNK)
            if len(b) < CHUNK:
                return
            yield i, b
            i += 1


MAX_ROAD = 64          # both shipped sets have 37 roads
MAX_WORLD = 0x2000     # the longest road is 498632 road units = 7791 world units


def parse_header(b):
    """key + residency windows.

    Returns (key, type, group, index, windows, end, terminated).
    `terminated` is False for a continuation chunk: a resource longer than
    0x4000 repeats the key but carries no 0xFFFE list terminator."""
    key = u32(b, 0)
    wins = []
    o = 4
    end = None
    while o + 2 <= CHUNK:
        v = u16(b, o)
        if v == 0xFFFE:
            end = o + 4          # 0xFFFE then one 0xFFFF
            break
        if v == 0xFFFF:
            o += 2
            continue
        if o + 6 > CHUNK:
            break
        road, lo, hi = v, u16(b, o + 2), u16(b, o + 4)
        if road >= MAX_ROAD or hi > MAX_WORLD or lo > hi:
            break                # not a window: this chunk has no header list
        wins.append((road, lo, hi))
        o += 6
    term = end is not None
    if end is None:
        wins = []
        end = 0x20
    return key, key >> 28, (key >> 16) & 0xFFF, key & 0xFFFF, wins, max(end, 0x20), term


def road_blocks(b):
    """Walk the tagged sub-block chain of a type-3 chunk."""
    out = {}
    o = ROAD_BLOCKS_AT
    while o + 8 <= CHUNK:
        tag = b[o:o + 4]
        size = u32(b, o + 4)
        if not all(65 <= c <= 95 for c in tag) or size < 8 or o + size > CHUNK:
            break
        out[tag.decode()] = (o, size)
        o += size
    return out


def pano_blocks(b, start):
    """Walk the tagged sub-block chain of a type-4 (PANO) chunk."""
    out = []
    o = start
    while o + 8 <= CHUNK:
        tag = b[o:o + 4]
        size = u32(b, o + 4)
        if not all(65 <= c <= 95 for c in tag) or size < 8 or o + size > CHUNK:
            break
        out.append((tag.decode()[::-1], o, size))
        o += size
    return out


def recs(b, bl, tag):
    if tag not in bl:
        return []
    o, s = bl[tag]
    n = REC[tag]
    return [b[o + 8 + i:o + 8 + i + n] for i in range(0, s - 8, n)]


# ---------------------------------------------------------------------------
# type 3 - the road object
# ---------------------------------------------------------------------------
class Slice:
    """One SLCT record: a cross section of the road centre line."""
    __slots__ = ("idx", "m", "pos", "arclen", "curv", "dist", "bg", "tail")

    def __init__(self, r):
        self.idx = u16(r, 0)
        self.m = struct.unpack_from("<9h", r, 2)       # 3x3, 4096 = 1.0
        self.pos = struct.unpack_from("<3i", r, 20)    # 16.16 world
        self.arclen = u32(r, 32)                       # to the next slice
        self.curv = struct.unpack_from("<hbb", r, 36)  # not fully decoded
        self.dist = u32(r, 40)                         # cumulative, 16.16
        self.bg = struct.unpack_from("<4B", r, 44)     # BGDT/BSDT index runs
        self.tail = u32(r, 48)                         # not decoded

    # row 0 = lateral (right), row 1 = surface normal (Y down), row 2 = tangent
    def right(self):
        return (self.m[0] / 4096.0, self.m[1] / 4096.0, self.m[2] / 4096.0)

    def up(self):
        return (self.m[3] / 4096.0, self.m[4] / 4096.0, self.m[5] / 4096.0)

    def fwd(self):
        return (self.m[6] / 4096.0, self.m[7] / 4096.0, self.m[8] / 4096.0)

    def world(self):
        return (self.pos[0] / FX, self.pos[1] / FX, self.pos[2] / FX)

    def links_to(self, nxt):
        """True when `nxt` is the next point of the same polyline run.

        A junction object holds several disjoint arms in one SLCT array; the
        run break is visible in the data itself: dist + arclen of a slice
        equals dist of its successor only inside a run (tolerance 64 raw =
        0.001 world units)."""
        return abs((self.dist + self.arclen) - nxt.dist) < 64


class RoadObject:
    def __init__(self, b):
        self.raw = b
        (self.key, self.type, self.group, self.index,
         self.windows, _, self.terminated) = parse_header(b)
        self.id = self.key & 0x0FFFFFFF
        self.bl = road_blocks(b)
        self.hdr = struct.unpack_from("<4I", b, 0x20)   # id, pieceKey, owner, half
        g = recs(b, self.bl, "GRPT")
        self.grpt = struct.unpack("<8I", g[0]) if g else (0,) * 8
        self.subt = [struct.unpack("<7I", r) for r in recs(b, self.bl, "SUBT")]
        self.seg = [struct.unpack("<I4i", r) for r in recs(b, self.bl, "SEG_")]
        self.slices = [Slice(r) for r in recs(b, self.bl, "SLCT")]
        self.xsih = [struct.unpack("<4I", r) for r in recs(b, self.bl, "XSIH")]
        self.xsai = [struct.unpack("<8HI", r) for r in recs(b, self.bl, "XSAI")]
        self.bsdt = [struct.unpack("<IHH3i3iII", r) for r in recs(b, self.bl, "BSDT")]
        self.bgdt = [struct.unpack("<HHHHHH2I", r) for r in recs(b, self.bl, "BGDT")]

    # --- derived -----------------------------------------------------------
    @property
    def piece_key(self):
        return self.hdr[1]

    @property
    def owner(self):
        return self.hdr[2]              # node id (junction) or road id (piece)

    @property
    def is_junction(self):
        return self.hdr[3] != 0xFFFFFFFF

    @property
    def span(self):
        """(start, end) along the road, raw units (roadUnits * 1024)."""
        return self.grpt[6], self.grpt[7]

    def runs(self):
        """The SLCT slices split into connected polylines."""
        out = []
        cur = []
        for i, s in enumerate(self.slices):
            cur.append(s)
            if i + 1 < len(self.slices) and not s.links_to(self.slices[i + 1]):
                out.append(cur)
                cur = []
        if cur:
            out.append(cur)
        return out

    def half_width(self):
        """Largest |lateral offset| found in XSDH, in world units.

        XSDH is only partially decoded; the lateral offsets are 16.16 values at
        a fixed stride inside the 44 byte record and are always multiples of a
        per-object lane pitch. Taking the extreme is a measurement, not a
        claim about the record layout."""
        best = 0.0
        o, s = self.bl.get("XSDH", (0, 8))
        for p in range(o + 8, o + s, 4):
            v = i32(self.raw, p)
            if -40 * 65536 < v < 40 * 65536:
                best = max(best, abs(v) / FX)
        return best or 8.0


def load_type3(path, want=None):
    """All distinct type-3 objects of a .STR/.STP, keyed by their 28 bit id."""
    out = {}
    for i, b in iter_chunks(path):
        if b[3] >> 4 != 3:
            continue
        key = u32(b, 0) & 0x0FFFFFFF
        if key in out:
            continue
        if want is not None and key not in want:
            continue
        out[key] = RoadObject(b)
    return out


# ---------------------------------------------------------------------------
# companion files (for cross checks)
# ---------------------------------------------------------------------------
def load_grf(path):
    d = open(path, "rb").read()
    nn, nr = struct.unpack_from("<II", d, 0x0C)
    on, orr = struct.unpack_from("<II", d, 0x14)
    roads = {}
    for i in range(nr):
        rid, ln, a, b = struct.unpack_from("<4i", d, orr + 16 * i)
        roads[rid] = (ln, a, b)
    pm = u32(d, 4)
    pieces = {}
    if d[pm:pm + 4] == b"PMTS":
        nb = u32(d, pm + 0x10)
        ob = u32(d, pm + 0x20)
        for i in range(nb):
            k, road, st, fl, ln = struct.unpack_from("<I4i", d, pm + ob + 20 * i)
            pieces[k] = dict(road=road, start=st, flag=fl, length=ln)
    return roads, pieces


def load_toc(path):
    d = open(path, "rb").read()
    nroad = u32(d, 0x0C)
    orr = u32(d, 0x14)
    out = {}
    for i in range(nroad):
        rd, fo, fs, ro, rs = struct.unpack_from("<5I", d, orr + 20 * i)
        out[rd] = dict(fwd=(fo, fs), rev=(ro, rs))
    return out


def set_of(path):
    """1 or 2, from the file name of a DATA file."""
    n = os.path.basename(path).upper()
    return 2 if ("2" in n and n.startswith(("STREAM", "RACE2"))) else 1


# ---------------------------------------------------------------------------
# info
# ---------------------------------------------------------------------------
def cmd_info(argv):
    path = argv[0]
    n = chunk_count(path)
    if len(argv) < 2:
        print("%s: %d chunks of 0x4000" % (path, n))
        for i, b in iter_chunks(path):
            key, t, grp, idx, wins, end, term = parse_header(b)
            print("  %4d key=%08x type=%-10s group=0x%03x index=%4d hdr=0x%02x %s %s"
                  % (i, key, TYPE_NAME.get(t, "?"), grp, idx, end,
                     "   " if term else "CNT",
                     " ".join("r%d[%d,%d]" % w for w in wins)))
        return 0
    i = int(argv[1], 0)
    b = read_chunk(path, i)
    key, t, grp, idx, wins, end, term = parse_header(b)
    print("chunk %d of %s   file offset 0x%x" % (i, path, (0x800 if is_stp(path) else 0) + i * CHUNK))
    print("  key          0x%08x" % key)
    print("  type         %d (%s)" % (t, TYPE_NAME.get(t, "?")))
    print("  id           0x%07x  group 0x%03x  index %d" % (key & 0x0FFFFFFF, grp, idx))
    print("  residency    " + (" ".join("road %d [%d..%d] world units" % w for w in wins) or "(none)"))
    print("  header ends  0x%02x%s" % (end, "" if term else "   (continuation chunk: no list terminator)"))
    if t == 3:
        o = RoadObject(b)
        print("  -- type 3: road object --")
        print("  objectIndex  %d   pieceKey 0x%08x   owner %d   half %s"
              % (o.hdr[0], o.hdr[1], o.hdr[2],
                 "junction %d" % o.hdr[3] if o.is_junction else "road piece"))
        s, e = o.span
        print("  GRPT         len=%d start=%d end=%d   (road units %.1f..%.1f, world %.1f..%.1f)"
              % (o.grpt[4], s, e, s / ROAD_UNIT, e / ROAD_UNIT, s / FX, e / FX))
        for r in o.seg:
            print("  SEG_         key=0x%08x road=%d startAlongRoad=%d flag=%d length=%d"
                  % (r[0], r[1], r[2], r[3], r[4]))
        print("  blocks       " + " ".join("%s:0x%x" % (k, v[1]) for k, v in o.bl.items()))
        print("  SLCT         %d slices, path %.1f world units, half width ~%.2f"
              % (len(o.slices),
                 (o.slices[-1].dist - o.slices[0].dist) / FX if o.slices else 0,
                 o.half_width()))
        for sl in o.slices[:6]:
            p = sl.world()
            print("     %3d pos=(%9.2f,%8.2f,%9.2f) fwd=(%6.3f,%6.3f,%6.3f) right=(%6.3f,%6.3f,%6.3f) d=%.1f"
                  % ((sl.idx,) + p + sl.fwd() + sl.right() + (sl.dist / FX,)))
        if len(o.slices) > 6:
            print("     ... %d more" % (len(o.slices) - 6))
    elif t == 4:
        print("  -- type 4: panorama --")
        print("  prologue     %d %d %d" % struct.unpack_from("<3I", b, end))
        for tag, off, size in pano_blocks(b, end + 12):
            print("     %s at 0x%04x size 0x%x" % (tag, off, size))
    elif t in (0, 8, 9):
        nw = u32(b, 0x20)
        print("  -- type %d: geometry/collision cell --" % t)
        print("  u32 count    %d  (block 0x20..0x%x)" % (nw, 0x20 + nw * 4))
        print("  words        " + " ".join("%08x" % u32(b, 0x20 + 4 * k)
                                           for k in range(1, min(nw, 16))))
    elif t in (1, 2):
        packed = ((key >> 23) & 0x1F) << 10 | (key & 0x3FF)
        print("  -- type %d: texture --   packed id 0x%04x%s"
              % (t, packed | (0x8000 if t == 2 else 0),
                 " (sprite bit)" if t == 2 else ""))
        print("  image bytes start at chunk+0x0C")
    elif t == 10:
        print("  -- type 10: SPU ADPCM stream --")
        print("  voice setup  " + b[0x40:0x4C].hex(" "))
        print("  first block  " + b[0x70:0x80].hex(" "))
    return 0


# ---------------------------------------------------------------------------
# scan
# ---------------------------------------------------------------------------
def cmd_scan(argv):
    d = argv[0]
    files = sorted(f for f in os.listdir(d)
                   if f.upper().endswith((".STR", ".STP")))
    print("%-16s %7s  %s" % ("file", "chunks", "type histogram"))
    grand = Counter()
    for f in files:
        p = os.path.join(d, f)
        c = Counter()
        n = 0
        for _, b in iter_chunks(p):
            c[b[3] >> 4] += 1
            n += 1
        grand.update(c)
        print("%-16s %7d  %s" % (f, n, " ".join(
            "%s=%d" % (TYPE_NAME.get(t, t), c[t]) for t in sorted(c))))
    print("-" * 60)
    print("%-16s %7d  %s" % ("TOTAL", sum(grand.values()), " ".join(
        "%s=%d" % (TYPE_NAME.get(t, t), grand[t]) for t in sorted(grand))))
    return 0


# ---------------------------------------------------------------------------
# road export
# ---------------------------------------------------------------------------
def _png(w, h, px, fn):
    raw = b"".join(b"\x00" + bytes(px[y * w * 3:(y + 1) * w * 3]) for y in range(h))

    def ck(t, dd):
        return struct.pack(">I", len(dd)) + t + dd + struct.pack(
            ">I", zlib.crc32(t + dd) & 0xFFFFFFFF)
    open(fn, "wb").write(
        b"\x89PNG\r\n\x1a\n"
        + ck(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        + ck(b"IDAT", zlib.compress(raw, 6)) + ck(b"IEND", b""))


def cmd_road(argv):
    src, out = argv[0], argv[1]
    objs = load_type3(src)
    if not objs:
        print("no type-3 (road) chunks in %s" % src)
        return 1
    ext = os.path.splitext(out)[1].lower()
    if ext == ".obj":
        return _export_obj(objs, out, argv[2:])
    return _export_svg(objs, out, src)


def _export_svg(objs, out, src):
    lines = []
    for k in sorted(objs):
        o = objs[k]
        for run in o.runs():
            if len(run) > 1:
                lines.append((k, o.is_junction, [s.world() for s in run]))
    xs = [p[0] for _, _, pp in lines for p in pp]
    zs = [p[2] for _, _, pp in lines for p in pp]
    minx, maxx, minz, maxz = min(xs), max(xs), min(zs), max(zs)
    W = 1600.0
    sc = W / max(maxx - minx, maxz - minz, 1e-6)
    H = (maxz - minz) * sc + 4
    s = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
         'viewBox="0 0 %d %d"><rect width="100%%" height="100%%" fill="#0b0b12"/>'
         % (W + 4, H, W + 4, H)]
    s.append('<text x="8" y="20" fill="#888" font-family="monospace" '
             'font-size="14">%s : %d road objects, %d slices</text>'
             % (os.path.basename(src), len(lines),
                sum(len(p) for _, _, p in lines)))
    for k, isj, pp in lines:
        col = "#e04040" if isj else "#40d0ff"
        d = " ".join("%s%.1f,%.1f" % ("M" if i == 0 else "L",
                                      (p[0] - minx) * sc + 2,
                                      H - (p[2] - minz) * sc)
                     for i, p in enumerate(pp))
        s.append('<path d="%s" fill="none" stroke="%s" stroke-width="1.2"/>' % (d, col))
    s.append("</svg>")
    open(out, "w").write("".join(s))
    # a PNG next to it so a human can look without a browser
    png = os.path.splitext(out)[0] + ".png"
    w = 1400
    sc = w / max(maxx - minx, maxz - minz, 1e-6)
    h = int((maxz - minz) * sc) + 2
    px = bytearray(b"\x0b\x0b\x12" * (w + 2) * h)

    def put(x, y, c):
        x, y = int(x), int(y)
        if 0 <= x < w + 2 and 0 <= y < h:
            i = (y * (w + 2) + x) * 3
            px[i], px[i + 1], px[i + 2] = c

    for k, isj, pp in lines:
        c = (224, 64, 64) if isj else (64, 208, 255)
        for i in range(len(pp) - 1):
            x0 = (pp[i][0] - minx) * sc
            y0 = h - (pp[i][2] - minz) * sc
            x1 = (pp[i + 1][0] - minx) * sc
            y1 = h - (pp[i + 1][2] - minz) * sc
            n = int(max(abs(x1 - x0), abs(y1 - y0))) + 1
            for j in range(n + 1):
                t = j / n
                put(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, c)
    _png(w + 2, h, px, png)
    print("wrote %s and %s (%d objects, %d slices)"
          % (out, png, len(lines), sum(len(p) for _, _, p in lines)))
    return 0


def _export_obj(objs, out, rest):
    only = set(int(x, 0) for x in rest) if rest else None
    v = []
    faces = []
    for k in sorted(objs):
        if only is not None and k not in only:
            continue
        o = objs[k]
        if len(o.slices) < 2:
            continue
        hw = o.half_width()
        for run in o.runs():
            if len(run) < 2:
                continue
            base = len(v) + 1
            for s in run:
                p = s.world()
                r = s.right()
                v.append((p[0] - r[0] * hw, p[1] - r[1] * hw, p[2] - r[2] * hw))
                v.append((p[0] + r[0] * hw, p[1] + r[1] * hw, p[2] + r[2] * hw))
            for i in range(len(run) - 1):
                a = base + 2 * i
                faces.append((a, a + 1, a + 3, a + 2))
    with open(out, "w") as f:
        f.write("# Road Rash: Jailbreak road surface, from type-3 stream chunks\n")
        f.write("# X/Y/Z are the game's 16.16 world coordinates / 65536; Y is down.\n")
        for x, y, z in v:
            f.write("v %.4f %.4f %.4f\n" % (x, -y, z))
        for q in faces:
            f.write("f %d %d %d %d\n" % q)
    print("wrote %s: %d vertices, %d quads" % (out, len(v), len(faces)))
    return 0


# ---------------------------------------------------------------------------
# verify
# ---------------------------------------------------------------------------
ORACLE = os.path.join("work", "oracle", "vr_capture", "ramdumps")


def _verify_oracle(objs):
    """Compare the decoded centre line against 14 live RAM dumps.

    The dumps were taken during an actual race on this disc. Two things are checked:
      * the 0x4000 stream chunk is resident verbatim, so the id of every
        resident type-3 object can be read straight out of guest RAM;
      * the racer position array the game itself maintains lies on the centre
        line we decoded, within a road width, in every frame."""
    if not os.path.isdir(ORACLE):
        print("  [--] oracle dumps not present, skipped")
        return 0
    files = sorted(f for f in os.listdir(ORACLE) if f.endswith(".bin"))
    if not files:
        return 0
    f = 0
    rams = [open(os.path.join(ORACLE, x), "rb").read() for x in files]
    resident = set()
    for k, o in objs.items():
        if rams[0].find(o.raw[0x8C:0x8C + 256]) >= 0:
            resident.add(k)
    f += _chk("type-3 chunks are resident in guest RAM byte for byte "
              "(sub-block area unmodified)",
              bool(resident), "objects %s in %s" % (sorted(resident), files[0]))
    grid = defaultdict(list)
    for k in resident:
        for s in objs[k].slices:
            grid[(s.pos[0] >> 22, s.pos[2] >> 22)].append(s.pos)

    def lateral(p):
        best = None
        cx, cz = p[0] >> 22, p[2] >> 22
        for dx in (-1, 0, 1):
            for dz in (-1, 0, 1):
                for q in grid[(cx + dx, cz + dz)]:
                    d = math.hypot(q[0] - p[0], q[2] - p[2])
                    if abs(q[1] - p[1]) < 8 * 65536 and (best is None or d < best):
                        best = d
        return best

    # the racer array: 12 byte 16.16 positions, moving a few world units a frame
    best = None
    for a in range(0x800C0000 - 0x80000000, 0x800E0000 - 0x80000000, 4):
        lat = []
        step = []
        prev = None
        okall = True
        for ram in rams:
            p = struct.unpack_from("<3i", ram, a)
            if abs(p[0]) > 8000 * 65536 or abs(p[2]) > 8000 * 65536:
                okall = False
                break
            d = lateral(p)
            if d is None or d > 20 * 65536:
                okall = False
                break
            lat.append(d / FX)
            if prev:
                step.append(math.hypot(p[0] - prev[0], p[2] - prev[2]) / FX)
            prev = p
        if okall and step and all(1 < s < 200 for s in step):
            best = (a, max(lat), sum(step) / len(step))
            break
    f += _chk("the live racer position lies on the decoded centre line in all "
              "%d frames" % len(rams), best is not None,
              "" if not best else
              "0x%08x, max lateral %.2f world units, %.1f units travelled per frame"
              % (0x80000000 + best[0], best[1], best[2]))
    return f


def cmd_verify(argv):
    d = argv[0] if argv else os.path.join("work", "disc_us", "DATA")
    fails = 0
    for n in (1, 2):
        str_p = os.path.join(d, "STREAM%d.STR" % n)
        grf_p = os.path.join(d, "STREAM%d.GRF" % n)
        toc_p = os.path.join(d, "STREAM%d.TOC" % n)
        if not os.path.exists(str_p):
            continue
        print("=== set %d ===" % n)
        roads, pieces = load_grf(grf_p)
        toc = load_toc(toc_p)
        objs = load_type3(str_p)
        fails += _verify_set(str_p, roads, pieces, toc, objs)
        if n == 1:
            fails += _verify_oracle(objs)
    print("\n%s" % ("VERIFY OK" if fails == 0 else "VERIFY: %d FAILURES" % fails))
    return 1 if fails else 0


def _chk(name, ok, detail=""):
    print("  [%s] %s%s" % ("ok" if ok else "FAIL", name,
                           ("  " + detail) if detail else ""))
    return 0 if ok else 1


def _verify_set(str_p, roads, pieces, toc, objs):
    f = 0
    # 1. every chunk key decodes to a known resource type
    bad = Counter()
    ntot = 0
    for _, b in iter_chunks(str_p):
        t = b[3] >> 4
        ntot += 1
        if t not in TYPE_NAME or TYPE_NAME[t] == "invalid":
            bad[t] += 1
    f += _chk("all %d chunk keys use a live resource type" % ntot, not bad,
              str(dict(bad)))

    # 2. every type-3 chunk's block chain ends exactly at a tag boundary and
    #    every block size is an exact multiple of its record size
    nb = 0
    badrec = []
    for k, o in objs.items():
        for tag, (off, size) in o.bl.items():
            nb += 1
            if (size - 8) % REC[tag]:
                badrec.append((k, tag, size))
    f += _chk("%d type-3 sub-blocks, all sizes exact multiples of the record size"
              % nb, not badrec, str(badrec[:4]))

    # 3. object count == BTT_ object count, ids are 0..N-1 with no holes
    ids = sorted(objs)
    f += _chk("type-3 object ids are 0..%d with no holes" % (len(ids) - 1),
              ids == list(range(len(ids))))

    # 4. junction objects come in pairs (2 per node), piece objects carry -1
    nodes = Counter(o.owner for o in objs.values() if o.is_junction)
    f += _chk("every intersection owns exactly 2 type-3 objects",
              all(v == 2 for v in nodes.values()) and len(nodes) == len(
                  {r[1] for r in roads.values()} | {r[2] for r in roads.values()}),
              "%d nodes" % len(nodes))

    # 5. SLCT +0x20 is the chord to the next slice, +0x28 the running sum
    worst = 0.0
    npair = 0
    for o in objs.values():
        for run in o.runs():
            for i in range(len(run) - 1):
                npair += 1
                d = math.dist(run[i].pos, run[i + 1].pos)
                if run[i].arclen:
                    worst = max(worst, abs(d - run[i].arclen) / run[i].arclen)
    f += _chk("SLCT +0x20 == chord to the next slice, +0x28 == running sum",
              worst < 0.002,
              "%d linked pairs, worst relative error %.5f" % (npair, worst))

    # 6. SLCT matrix row 2 is the unit tangent, row 0 is perpendicular to it
    wt = 0.0
    wr = 0.0
    for o in objs.values():
        for run in o.runs():
            for i in range(len(run) - 1):
                a, b = run[i], run[i + 1]
                L = math.dist(a.pos, b.pos)
                if L < 1:
                    continue
                t = [(b.pos[j] - a.pos[j]) / L for j in range(3)]
                fw, ri = a.fwd(), a.right()
                wt = max(wt, abs(1.0 - sum(x * y for x, y in zip(fw, t))))
                wr = max(wr, abs(sum(x * y for x, y in zip(ri, t))))
    f += _chk("SLCT 3x3 row 2 == unit tangent", wt < 0.01, "max |1-dot| = %.5f" % wt)
    f += _chk("SLCT 3x3 row 0 _|_ tangent", wr < 0.01, "max |dot| = %.5f" % wr)

    # 7. GRPT[4] is the object span; objects of one road chain end->start
    bad = []
    byroad = defaultdict(list)
    for k, o in objs.items():
        if o.grpt[2] in pieces:
            byroad[pieces[o.grpt[2]]["road"]].append((o.grpt[6], o.grpt[7], k))
    joins = 0
    for r, L in byroad.items():
        L.sort()
        for i in range(len(L) - 1):
            joins += 1
            if L[i][1] != L[i + 1][0]:
                bad.append((r, L[i][2], L[i + 1][2], L[i + 1][0] - L[i][1]))
    # one break per road is legal: only 25 of the 74 road ends have a junction
    # approach object, so the rest leave a hole between the road objects and
    # the junction object at that end.
    f += _chk("consecutive type-3 objects of a road join exactly (end == next start)",
              len(bad) <= 8,
              "%d/%d joins broken (all of them road<->junction, never road<->road)"
              % (len(bad), joins))

    # 8. the scale: a SEG_ piece boundary inside an object coincides with an
    #    XSIH cross-section boundary when multiplied by 1024
    tot = hit = 0
    worst = 0
    for o in objs.values():
        if not o.grpt[7]:
            continue
        xb = set()
        for x in o.xsih:
            xb.add(x[2])
            xb.add(x[3])
        for s in o.seg:
            if s[2] <= 0:
                continue
            c = s[2] * ROAD_UNIT
            if o.grpt[6] + ROAD_UNIT < c < o.grpt[7] - ROAD_UNIT:
                tot += 1
                dmin = min(abs(c - v) for v in xb) if xb else 1 << 40
                if dmin < 8192:
                    hit += 1
                    worst = max(worst, dmin)
    f += _chk("1 road unit == 1024 raw world units (SEG_ vs XSIH boundaries)",
              tot and hit >= tot - 2,
              "%d/%d boundaries coincide, worst error %d raw (%.4f world)"
              % (hit, tot, worst, worst / FX))

    # 9. the header residency window names the object's own road, and its
    #    upper bound never exceeds the road length in world units
    okw = badw = 0
    for o in objs.values():
        if o.grpt[2] not in pieces:
            continue
        road = pieces[o.grpt[2]]["road"]
        ws = [w for w in o.windows if w[0] == road]
        if ws and all(w[2] <= roads[road][0] // 64 + 2 for w in ws):
            okw += 1
        else:
            badw += 1
    f += _chk("header residency window names the object's road, bounded by its length",
              badw == 0, "%d ok, %d bad" % (okw, badw))

    # 10. the type-3 chunks of a road's stream appear in distance order
    dis = 0
    seq = 0
    base = 0
    with open(str_p, "rb") as fh:
        for road, rec in toc.items():
            for dirn in ("fwd", "rev"):
                off, size = rec[dirn]
                if size == 0 or off + size > os.path.getsize(str_p):
                    continue
                order = []
                for c in range(size // CHUNK):
                    fh.seek(off + c * CHUNK)
                    b = fh.read(4)
                    key = struct.unpack("<I", b)[0]
                    if key >> 28 != 3:
                        continue
                    o = objs.get(key & 0x0FFFFFFF)
                    if o and o.grpt[2] in pieces and pieces[o.grpt[2]]["road"] == road:
                        order.append(o.grpt[6])
                if len(order) > 1:
                    seq += 1
                    mono = all(order[i] < order[i + 1] for i in range(len(order) - 1))
                    rmono = all(order[i] > order[i + 1] for i in range(len(order) - 1))
                    if mono or rmono:
                        dis += 1
    f += _chk("a road's own type-3 chunks appear in the stream in distance order",
              dis == seq, "%d/%d directional streams monotonic" % (dis, seq))
    return f


# ---------------------------------------------------------------------------
# route - the geometry of one race, in route order
# ---------------------------------------------------------------------------
def load_roadgrf(path, race_id):
    """Minimal [RACEID] block reader; the full grammar is in road.md 1.1."""
    blocks = []
    cur = None
    for ln in open(path, "r", errors="replace"):
        t = ln.strip()
        if t.startswith("[BEGIN]"):
            cur = {}
        elif t.startswith("[END]"):
            if cur is not None:
                blocks.append(cur)
            cur = None
        elif cur is None:
            continue
        elif "=" in t:
            k, v = t.split("=", 1)
            w = v.split()
            vals = [int(x) for x in w] if w and all(
                x.lstrip("-").isdigit() for x in w) else []
            if k == "[RACEINTS]":
                cur["nints"] = vals[0] if vals else 0
                cur["ints"] = []
            else:
                cur[k] = vals
        elif t and "ints" in cur:
            w = t.split()
            if len(w) == 19 and all(x.lstrip("-").isdigit() for x in w):
                cur["ints"].append([int(x) for x in w])
    for b in blocks:
        if b.get("[RACEID]", [None])[0] == race_id:
            return b
    return None


def route_roads(blk):
    """Walk start node -> finish node, yielding (road, direction)."""
    ints = {r[0]: r for r in blk.get("ints", [])}
    road, _d, d0, node = blk["[START]"]
    out = [(road, d0)]
    fin_node = blk["[FINISH]"][3]
    guard = 0
    while node != fin_node and node in ints and guard < 200:
        r = ints[node]
        links = [(r[3 + 2 * i], r[4 + 2 * i]) for i in range(4)]
        rroads = [x for x in r[11:15] if x != -1]
        if not rroads:
            break
        nxt = rroads[0]
        slot = next((i for i, l in enumerate(links) if l[0] == nxt), None)
        if slot is None:
            break
        out.append((nxt, links[slot][1]))
        node = r[15 + slot]
        guard += 1
    return out


def cmd_route(argv):
    data, sset, race, out = argv[0], int(argv[1]), int(argv[2]), argv[3]
    blk = load_roadgrf(os.path.join(data, "ROADGRF%d.TXT" % sset), race)
    if blk is None:
        print("race %d not in ROADGRF%d.TXT" % (race, sset))
        return 1
    roads, pieces = load_grf(os.path.join(data, "STREAM%d.GRF" % sset))
    objs = load_type3(os.path.join(data, "STREAM%d.STR" % sset))
    byroad = defaultdict(list)
    for k, o in objs.items():
        if o.grpt[2] in pieces:
            byroad[pieces[o.grpt[2]]["road"]].append(o)
    for v in byroad.values():
        v.sort(key=lambda o: o.grpt[6])
    seq = route_roads(blk)
    pts = []
    total = 0.0
    for road, dirn in seq:
        arms = []
        for o in byroad.get(road, []):
            for run in o.runs():
                if len(run) > 1:
                    arms.append([s.world() for s in run])
        if dirn < 0:
            arms = [a[::-1] for a in arms[::-1]]
        for a in arms:
            total += sum(math.dist(a[i], a[i + 1]) for i in range(len(a) - 1))
            pts.append((road, a))
    rgts = sum(roads[r][0] for r, _ in seq)
    print("race %d of set %d: %d roads on the route" % (race, sset, len(seq)))
    print("  roads            %s"
          % " ".join("%d%s" % (r, "+" if d > 0 else "-") for r, d in seq))
    print("  RGTS length sum  %d road units = %.1f world units" % (rgts, rgts / 64.0))
    print("  extracted road   %.1f world units over %d polylines" % (total, len(pts)))
    print("  coverage         %.1f%% (the junction fans are separate objects)"
          % (100.0 * total / max(rgts / 64.0, 1e-6)))
    _route_svg(pts, objs, out)
    return 0


def _route_svg(pts, objs, out):
    allp = [[s.world() for s in run]
            for o in objs.values() for run in o.runs() if len(run) > 1]
    xs = [p[0] for pp in allp for p in pp]
    zs = [p[2] for pp in allp for p in pp]
    minx, maxx, minz, maxz = min(xs), max(xs), min(zs), max(zs)
    W = 1500.0
    sc = W / max(maxx - minx, maxz - minz, 1e-6)
    H = (maxz - minz) * sc + 4

    def path(pp):
        return " ".join("%s%.1f,%.1f" % ("M" if i == 0 else "L",
                                         (p[0] - minx) * sc + 2,
                                         H - (p[2] - minz) * sc)
                        for i, p in enumerate(pp))
    s = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
         'viewBox="0 0 %d %d"><rect width="100%%" height="100%%" fill="#0b0b12"/>'
         % (W + 4, H, W + 4, H)]
    for pp in allp:
        s.append('<path d="%s" fill="none" stroke="#2a3340" stroke-width="1"/>'
                 % path(pp))
    for _road, pp in pts:
        s.append('<path d="%s" fill="none" stroke="#ffd24a" stroke-width="2.4"/>'
                 % path(pp))
    s.append("</svg>")
    open(out, "w").write("".join(s))
    print("  wrote %s" % out)


# ---------------------------------------------------------------------------
def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd, rest = argv[1], argv[2:]
    if cmd == "info":
        return cmd_info(rest)
    if cmd == "scan":
        return cmd_scan(rest)
    if cmd == "road":
        return cmd_road(rest)
    if cmd == "route":
        return cmd_route(rest)
    if cmd == "verify":
        return cmd_verify(rest)
    print("unknown subcommand %r" % cmd)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
