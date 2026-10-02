#!/usr/bin/env python3
"""Scout probe for the scene-cell chunks of Road Rash: Jailbreak (PS1).

Types 0, 8 and 9 of the 0x4000 stream chunk (see docs\\formats\\road_chunk.md
for the chunk header and the type dispatch).  This file decodes the payload:
the cell body header, its eight regions, the drawn mesh and the object /
placement table.  Everything here comes from our own extract of the player's
disc plus our own disassembly of `SLUS_010.53` / `RASHCDG.BIN`; the provenance
of every field is in docs\\formats\\scene_cell.md.

Subcommands
    info   <file> [chunk]          decode one cell (or list every cell of a file)
    scan   <dir>                   census over every .STR/.STP in a directory
    place  <file> <out.csv>        decoded object placements of every cell
    obj    <file> <chunk> <out>    cell mesh (+ prop markers) as Wavefront OBJ
    plot   <file> <out.svg>        top down: roads, cell meshes, prop positions
    verify [dir]                   the re-runnable checks

No game bytes are ever written into the repo: every output path must be under
work\\ (the caller's responsibility).
"""
from __future__ import annotations

import math
import os
import struct
import sys
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import chunk as C                                    # noqa: E402  (sibling probe)

CHUNK = 0x4000
FX = 65536.0
ROAD_UNIT = 64.0        # 1 world unit = 64 cell/road units  (road_chunk.md 2.5)

# ---------------------------------------------------------------------------
# low level
# ---------------------------------------------------------------------------
u16 = C.u16
u32 = C.u32
i32 = C.i32


def i16(b, o):
    return struct.unpack_from("<h", b, o)[0]


def s32v(b, o, n):
    return struct.unpack_from("<%di" % n, b, o)


# The five object arrays of region 0.  Strides come from the classify helper at
# RASHCDG 0x8009C41C, which multiplies the index by 68 / 76 / 64 / 88 / 64 for
# kinds 3 / 2 / 4 / 6 / 0 and adds *(region0 + 0x24 / 0x28 / 0x2C / 0x30 / 0x34).
OBJ_ARRAYS = [
    # (count offset, pointer offset, stride, kind passed to 0x8009C41C)
    (0x00, 0x24, 68, 3),
    (0x02, 0x28, 76, 2),
    (0x04, 0x2C, 64, 4),
    (0x06, 0x30, 88, 6),
    (0x08, 0x34, 64, 0),
]

KIND_NAME = {0: "rand", 2: "k2", 3: "k3", 4: "prop", 6: "k6"}


class Prim:
    """One drawn primitive of a cell: a textured triangle (20 B) or quad (24 B).

    Layout is the DPD3 primitive of rmd3.md 3.1 with a four byte prefix; the
    clut/texRef split is proven by the fix-up pass at EXE 0x800340DC..0x80034204
    (reads u16 at +0x0A and u8 at +0x01, writes clut to +0x06 and tpage to
    +0x0A) and the per-corner V bytes it biases at +0x05 / +0x09 / +0x0D
    (and +0x0F for the 24 byte form)."""

    __slots__ = ("n", "flags", "pal", "w2", "uv", "clut", "tex", "idx")

    def __init__(self, b, o, n):
        self.n = n
        self.flags = b[o]
        self.pal = b[o + 1]
        self.w2 = u16(b, o + 2)
        self.clut = u16(b, o + 6)
        self.tex = u16(b, o + 10)
        if n == 3:
            self.uv = [(b[o + 4], b[o + 5]), (b[o + 8], b[o + 9]),
                       (b[o + 12], b[o + 13])]
            self.idx = [u16(b, o + 14), u16(b, o + 16), u16(b, o + 18)]
        else:
            self.uv = [(b[o + 4], b[o + 5]), (b[o + 8], b[o + 9]),
                       (b[o + 12], b[o + 13]), (b[o + 14], b[o + 15])]
            self.idx = [u16(b, o + 16), u16(b, o + 18), u16(b, o + 20),
                        u16(b, o + 22)]


class Obj:
    """One record of a region-0 object array."""

    __slots__ = ("kind", "cls", "size", "piece", "normal", "pos", "f20", "f24",
                 "tail", "raw")

    def __init__(self, b, o, size):
        self.raw = b[o:o + size]
        self.size = size
        self.kind = u16(b, o + 0x00)
        self.cls = u16(b, o + 0x02)
        self.piece = u32(b, o + 0x08)
        self.normal = (i16(b, o + 0x0E) / 4096.0, i16(b, o + 0x10) / 4096.0,
                       i16(b, o + 0x12) / 4096.0)
        self.pos = (i32(b, o + 0x14) / FX, i32(b, o + 0x18) / FX,
                    i32(b, o + 0x1C) / FX)
        self.f20 = i32(b, o + 0x20) / FX
        self.f24 = i32(b, o + 0x24) / FX
        self.tail = u32(b, o + size - 4)

    def yaw(self):
        """Heading of the normal in the world XZ plane, radians."""
        return math.atan2(self.normal[0], self.normal[2])


class Cell:
    """A type 0 or type 8 scene cell, optionally completed by its type 9 half."""

    def __init__(self, b, raw9=None):
        self.raw = b
        self.raw9 = raw9
        (self.key, self.type, self.group, self.index,
         self.windows, _, self.terminated) = C.parse_header(b)
        self.id = self.key & 0x0FFFFFFF
        self.nwords = u32(b, 0x20)
        self.npairs = self.nwords - 13
        self.pairs = [u16(b, 0x24 + 2 * j) for j in range(2 * self.npairs)]
        self.extent = []
        eo = 0x20 + self.nwords * 4 - 48
        for j in range(4):
            r, a, c = struct.unpack_from("<3I", b, eo + 12 * j)
            if r != 0xFFFFFFFF:
                self.extent.append((r, a, c))
        self.body = 0x20 + self.nwords * 4
        bo = self.body
        self.h0 = u32(b, bo)
        self.na = u16(b, bo + 4)              # "A"
        self.nb = u16(b, bo + 6)              # "B"
        self.origin_raw = s32v(b, bo + 8, 3)
        self.vec2_raw = s32v(b, bo + 0x14, 3)
        self.rel = [u32(b, bo + 0x20 + 4 * j) for j in range(8)]
        self.R = [0x20 + r for r in self.rel]
        self.rsize = [self.R[j + 1] - self.R[j] for j in range(7)]

        # region 1: vertex banks; region 5: the vertex array (4 byte prefix)
        self.banks = [(u32(b, self.R[1] + 8 * j), u32(b, self.R[1] + 8 * j + 4))
                      for j in range(self.rsize[1] // 8)]
        self.nvert = sum(c for _, c in self.banks)
        self.verts = [struct.unpack_from("<4h", b, self.R[5] + 4 + 8 * j)
                      for j in range(self.nvert)]

        # region 2: primitive group table, A+3B records of 12 bytes
        n2 = self.rsize[2] // 12
        self.groups = [struct.unpack_from("<3I", b, self.R[2] + 12 * j)
                       for j in range(n2)]

        # region 0: the object arrays
        r0 = self.R[0]
        self.r0_id = u32(b, r0 + 0x0C)
        self.objs = defaultdict(list)
        for (co, po, stride, kind) in OBJ_ARRAYS:
            n = u16(b, r0 + co)
            base = r0 + u32(b, r0 + po)
            for j in range(n):
                self.objs[kind].append(Obj(b, base + stride * j, stride))

        self.prims6 = self._prims(b, self.R[6], self.groups[:self.na + self.nb])
        band12 = self.groups[self.na + self.nb:]
        if self.type == 8:
            self.prims7 = (self._prims(raw9, 0x20, band12) if raw9 else [])
        else:
            self.prims7 = self._prims(b, self.R[7], band12)

    @staticmethod
    def _prims(b, base, groups):
        out = []
        if b is None:
            return out
        for (ofs, n3, n4) in groups:
            o = base + ofs
            for _ in range(n3):
                if o + 20 > len(b):
                    return out
                out.append(Prim(b, o, 3))
                o += 20
            for _ in range(n4):
                if o + 24 > len(b):
                    return out
                out.append(Prim(b, o, 4))
                o += 24
        return out

    # -- derived ------------------------------------------------------------
    @property
    def origin(self):
        """Cell origin in world units.  RASHCDG 0x800A8584 loads the three
        words at body+0x08/0x0C/0x10 and shifts each left by 10 to make a 16.16
        world coordinate, i.e. the stored value is world * 64."""
        return tuple(v / ROAD_UNIT for v in self.origin_raw)

    def world(self, vi):
        v = self.verts[vi]
        o = self.origin_raw
        return ((o[0] + v[0]) / ROAD_UNIT, (o[1] + v[1]) / ROAD_UNIT,
                (o[2] + v[2]) / ROAD_UNIT)

    def prim_bytes_needed(self, lo, hi):
        return sum(n3 * 20 + n4 * 24 for (_, n3, n4) in self.groups[lo:hi])


# ---------------------------------------------------------------------------
# loading
# ---------------------------------------------------------------------------
def load_cells(path, want_type=(0, 8)):
    """Every distinct cell of a .STR/.STP, type 8 joined to its type 9 half."""
    nine = {}
    raw = {}
    for i, b in C.iter_chunks(path):
        t = b[3] >> 4
        if t not in (0, 8, 9):
            continue
        rid = u32(b, 0) & 0x0FFFFFFF
        if t == 9:
            nine.setdefault(rid, b)
        else:
            raw.setdefault((t, rid), (i, b))
    out = {}
    for (t, rid), (i, b) in sorted(raw.items()):
        if t not in want_type:
            continue
        out[(t, rid)] = (i, Cell(b, nine.get(rid)))
    return out


def stream_files(d):
    return sorted(os.path.join(d, f) for f in os.listdir(d)
                  if f.upper().endswith((".STR", ".STP")))


# ---------------------------------------------------------------------------
# road geometry (for the placement sanity test)
# ---------------------------------------------------------------------------
class RoadIndex:
    """Every road slice of a set, in a coarse XZ grid."""

    CELL = 64.0

    def __init__(self, path):
        self.objs = C.load_type3(path)
        self.grid = defaultdict(list)
        self.n = 0
        for oid, o in self.objs.items():
            for s in o.slices:
                w = s.world()
                self.grid[(int(w[0] // self.CELL), int(w[2] // self.CELL))].append(
                    (w, s.right(), s.dist, oid, o.owner))
                self.n += 1

    def nearest(self, p):
        cx, cz = int(p[0] // self.CELL), int(p[2] // self.CELL)
        best, bd = None, 1e30
        for dx in (-1, 0, 1):
            for dz in (-1, 0, 1):
                for e in self.grid.get((cx + dx, cz + dz), ()):
                    w = e[0]
                    d = (w[0] - p[0]) ** 2 + (w[2] - p[2]) ** 2
                    if d < bd:
                        bd, best = d, e
        return best, math.sqrt(bd)


# ---------------------------------------------------------------------------
# info
# ---------------------------------------------------------------------------
def cmd_info(argv):
    path = argv[0]
    if len(argv) > 1:
        idx = int(argv[1], 0)
        b = C.read_chunk(path, idx)
        t = b[3] >> 4
        if t == 9:
            print("chunk %d key %08x: type 9 - the region-7 half of cell %08x"
                  % (idx, u32(b, 0), u32(b, 0) & 0x0FFFFFFF))
            print("payload starts at +0x20 and is the byte image of that "
                  "cell's region 7")
            return
        if t not in (0, 8):
            print("chunk %d is type %d, not a scene cell" % (idx, t))
            return
        nine = None
        rid = u32(b, 0) & 0x0FFFFFFF
        for i, c in C.iter_chunks(path):
            if c[3] >> 4 == 9 and (u32(c, 0) & 0x0FFFFFFF) == rid:
                nine = c
                break
        dump_cell(Cell(b, nine), idx)
        return
    cells = load_cells(path)
    print("%-6s %-9s %-4s %-5s %-6s %-6s %-6s %s"
          % ("chunk", "key", "type", "verts", "prim6", "prim7", "objs", "extent"))
    for (t, rid), (i, c) in cells.items():
        no = sum(len(v) for v in c.objs.values())
        ext = " ".join("r%d[%d,%d]" % e for e in c.extent)
        print("%-6d %08x  %-4d %-5d %-6d %-6d %-6d %s"
              % (i, c.key, t, c.nvert, len(c.prims6), len(c.prims7), no, ext))


def dump_cell(c, idx=None, out=sys.stdout):
    p = out.write
    p("chunk %s key %08x  type %d  group %03x  index %d\n"
      % (idx, c.key, c.type, c.group, c.index))
    p("  residency windows : %s\n"
      % " ".join("r%d[%d,%d]" % w for w in c.windows))
    p("  nWords            : %d  -> %d texture pair(s), body at +0x%x\n"
      % (c.nwords, c.npairs, c.body))
    p("  texture pairs     : %s\n" % " ".join("%04x" % v for v in c.pairs))
    p("  extent[4]         : %s\n"
      % " ".join("(road %d, %d..%d)" % e for e in c.extent))
    p("  body +0x00        : %08x\n" % c.h0)
    p("  body +0x04 A / +0x06 B : %d / %d   (region 2 holds A+3B = %d records)\n"
      % (c.na, c.nb, len(c.groups)))
    p("  origin (raw/64)   : %s -> world %s\n"
      % (list(c.origin_raw), ["%.2f" % v for v in c.origin]))
    p("  body +0x14 vec    : %s -> /64 %s\n"
      % (list(c.vec2_raw), ["%.2f" % (v / ROAD_UNIT) for v in c.vec2_raw]))
    for j in range(8):
        end = c.R[j + 1] if j < 7 else None
        p("  region %d          : +0x%04x .. %s  (%s bytes)\n"
          % (j, c.R[j], ("+0x%04x" % end) if end else "?",
             c.rsize[j] if j < 7 else "?"))
    p("  vertex banks      : %s  (total %d)\n" % (c.banks, c.nvert))
    p("  primitive groups  : %d\n" % len(c.groups))
    for j, (o, n3, n4) in enumerate(c.groups):
        band = 0 if j < c.na + c.nb else (1 if j < c.na + 2 * c.nb else 2)
        p("     [%2d] band %d  ofs 0x%05x  tri %-4d quad %-4d\n"
          % (j, band, o, n3, n4))
    p("  region 6 prims    : %d   textures %s\n"
      % (len(c.prims6), dict(Counter("%04x" % q.tex for q in c.prims6))))
    p("  region 7 prims    : %d   textures %s\n"
      % (len(c.prims7), dict(Counter("%04x" % q.tex for q in c.prims7))))
    for kind in (3, 2, 4, 6, 0):
        lst = c.objs.get(kind, [])
        if not lst:
            continue
        p("  object array kind %d (%s), %d records of %d bytes\n"
          % (kind, KIND_NAME[kind], len(lst), lst[0].size))
        for j, o in enumerate(lst[:8]):
            p("     [%2d] class %-3d piece %08x  pos (%9.2f,%8.2f,%9.2f) "
              "n (%6.3f,%6.3f,%6.3f)  +20 %10.2f  +24 %10.2f\n"
              % (j, o.cls, o.piece, o.pos[0], o.pos[1], o.pos[2],
                 o.normal[0], o.normal[1], o.normal[2], o.f20, o.f24))
        if len(lst) > 8:
            p("     ... %d more\n" % (len(lst) - 8))


# ---------------------------------------------------------------------------
# scan
# ---------------------------------------------------------------------------
def cmd_scan(argv):
    d = argv[0]
    tot = Counter()
    cls = defaultdict(Counter)
    for p in stream_files(d):
        seen = set()
        n = Counter()
        for i, b in C.iter_chunks(p):
            t = b[3] >> 4
            if t not in (0, 8, 9):
                continue
            n["type%d" % t] += 1
            rid = u32(b, 0) & 0x0FFFFFFF
            if (t, rid) in seen:
                continue
            seen.add((t, rid))
            n["distinct%d" % t] += 1
        if not n:
            continue
        for k, v in n.items():
            tot[k] += v
        print("%-16s %s" % (os.path.basename(p),
                            " ".join("%s=%d" % (k, n[k]) for k in sorted(n))))
        for (t, rid), (i, c) in load_cells(p).items():
            for kind, lst in c.objs.items():
                for o in lst:
                    cls[kind][o.cls] += 1
                    tot["obj%d" % kind] += 1
    print("\nTOTAL %s" % " ".join("%s=%d" % (k, tot[k]) for k in sorted(tot)))
    for kind in sorted(cls):
        print("  kind %d class histogram: %s"
              % (kind, dict(sorted(cls[kind].items()))))


# ---------------------------------------------------------------------------
# place
# ---------------------------------------------------------------------------
def cmd_place(argv):
    path, out = argv[0], argv[1]
    road = None
    setdir = os.path.dirname(path)
    for cand in ("STREAM1.STR", "STREAM2.STR"):
        q = os.path.join(setdir, cand)
        if os.path.exists(q):
            road = RoadIndex(q)
            break
    cells = load_cells(path)
    rows = 0
    with open(out, "w") as f:
        f.write("cellKey,cellType,chunk,kind,index,class,pieceKey,"
                "x,y,z,nx,ny,nz,yawDeg,f20,f24,tail,roadDist,roadObj,roadOwner\n")
        for (t, rid), (i, c) in cells.items():
            for kind in (3, 2, 4, 6, 0):
                for j, o in enumerate(c.objs.get(kind, [])):
                    rd, ro, rw = "", "", ""
                    if road:
                        e, d = road.nearest(o.pos)
                        if e:
                            rd, ro, rw = "%.2f" % d, e[3], e[4]
                    f.write("%08x,%d,%d,%d,%d,%d,%08x,%.3f,%.3f,%.3f,"
                            "%.4f,%.4f,%.4f,%.1f,%.3f,%.3f,%08x,%s,%s,%s\n"
                            % (c.key, t, i, kind, j, o.cls, o.piece,
                               o.pos[0], o.pos[1], o.pos[2],
                               o.normal[0], o.normal[1], o.normal[2],
                               math.degrees(o.yaw()), o.f20, o.f24, o.tail,
                               rd, ro, rw))
                    rows += 1
    print("wrote %d placements to %s" % (rows, out))


# ---------------------------------------------------------------------------
# obj
# ---------------------------------------------------------------------------
def cmd_obj(argv):
    path, idx, out = argv[0], int(argv[1], 0), argv[2]
    b = C.read_chunk(path, idx)
    rid = u32(b, 0) & 0x0FFFFFFF
    nine = None
    for i, q in C.iter_chunks(path):
        if q[3] >> 4 == 9 and (u32(q, 0) & 0x0FFFFFFF) == rid:
            nine = q
            break
    c = Cell(b, nine)
    write_obj([c], out, path)


def write_obj(cells, out, path=None):
    """Cell meshes in world units, plus an oriented marker per placed object.

    OBJ is right handed and Y up while the game has Y down, so Y is negated on
    the way out; X and Z are untouched."""
    road = None
    if path:
        d = os.path.dirname(path)
        for cand in ("STREAM1.STR", "STREAM2.STR"):
            q = os.path.join(d, cand)
            if os.path.exists(q):
                road = C.load_type3(q)
                break
    with open(out, "w") as f:
        f.write("# Road Rash: Jailbreak scene cell(s), world units\n")
        base = 1
        for c in cells:
            f.write("o cell_%08x\n" % c.key)
            for vi in range(c.nvert):
                w = c.world(vi)
                f.write("v %.4f %.4f %.4f\n" % (w[0], -w[1], w[2]))
            for grp, prims in (("r6", c.prims6), ("r7", c.prims7)):
                f.write("g cell_%08x_%s\n" % (c.key, grp))
                for q in prims:
                    if max(q.idx) >= c.nvert:
                        continue
                    a, bb, cc = q.idx[0] + base, q.idx[1] + base, q.idx[2] + base
                    if q.n == 3:
                        f.write("f %d %d %d\n" % (a, bb, cc))
                    else:
                        dd = q.idx[3] + base
                        f.write("f %d %d %d\n" % (a, bb, cc))
                        f.write("f %d %d %d\n" % (bb, dd, cc))
            base += c.nvert
            # object markers: a 4 world unit cross along the record's normal
            for kind in (4, 6, 2, 3, 0):
                lst = c.objs.get(kind, [])
                if not lst:
                    continue
                f.write("g cell_%08x_obj%d\n" % (c.key, kind))
                for o in lst:
                    x, y, z = o.pos
                    n = o.normal
                    pts = [(x, y - 4.0, z), (x, y + 1.0, z),
                           (x + 4 * n[0], y + 4 * n[1], z + 4 * n[2])]
                    for px, py, pz in pts:
                        f.write("v %.4f %.4f %.4f\n" % (px, -py, pz))
                    f.write("f %d %d %d\n" % (base, base + 1, base + 2))
                    base += 3
        if road:
            f.write("g road_centreline\n")
            for oid, o in road.items():
                for run in o.runs():
                    if len(run) < 2:
                        continue
                    first = base
                    for s in run:
                        w = s.world()
                        f.write("v %.4f %.4f %.4f\n" % (w[0], -w[1] + 0.5, w[2]))
                    base += len(run)
                    for k in range(len(run) - 1):
                        f.write("f %d %d %d\n" % (first + k, first + k + 1,
                                                  first + k + 1))
    print("wrote %s" % out)


# ---------------------------------------------------------------------------
# plot
# ---------------------------------------------------------------------------
def cmd_plot(argv):
    path, out = argv[0], argv[1]
    d = os.path.dirname(path)
    strm = None
    for cand in ("STREAM1.STR", "STREAM2.STR"):
        q = os.path.join(d, cand)
        if os.path.exists(q):
            strm = q
            break
    roads = C.load_type3(strm) if strm else {}
    cells = load_cells(path)
    seg, props, mesh = [], [], []
    bb = [1e30, -1e30, 1e30, -1e30]
    for (t, rid), (i, c) in cells.items():
        for vi in range(c.nvert):
            w = c.world(vi)
            bb[0] = min(bb[0], w[0]); bb[1] = max(bb[1], w[0])
            bb[2] = min(bb[2], w[2]); bb[3] = max(bb[3], w[2])
    for oid, o in roads.items():
        for run in o.runs():
            for k in range(len(run) - 1):
                a, b2 = run[k].world(), run[k + 1].world()
                if not (bb[0] - 50 < a[0] < bb[1] + 50
                        and bb[2] - 50 < a[2] < bb[3] + 50):
                    continue
                seg.append((a[0], a[2], b2[0], b2[2]))
    for (t, rid), (i, c) in cells.items():
        for q in c.prims6 + c.prims7:
            if max(q.idx) >= c.nvert:
                continue
            pts = [c.world(v) for v in q.idx]
            for k in range(len(pts)):
                a, b2 = pts[k], pts[(k + 1) % len(pts)]
                mesh.append((a[0], a[2], b2[0], b2[2]))
        for kind, lst in c.objs.items():
            for o in lst:
                props.append((o.pos[0], o.pos[2], kind))
    xs = [v for s in seg + mesh for v in (s[0], s[2])] + [p[0] for p in props]
    zs = [v for s in seg + mesh for v in (s[1], s[3])] + [p[1] for p in props]
    if not xs:
        print("nothing to plot")
        return
    x0, x1, z0, z1 = min(xs), max(xs), min(zs), max(zs)
    W = 1600.0
    sc = W / max(x1 - x0, z1 - z0, 1.0)
    H = (z1 - z0) * sc + 20
    col = {4: "#d43f00", 6: "#0066cc", 2: "#00a000", 3: "#aa00aa", 0: "#888800"}

    def px(x, z):
        return (x - x0) * sc + 10, (z1 - z) * sc + 10

    with open(out, "w") as f:
        f.write('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
                'viewBox="0 0 %d %d">\n' % (W + 20, H, W + 20, H))
        f.write('<rect width="100%%" height="100%%" fill="white"/>\n')
        f.write('<g stroke="#cccccc" stroke-width="0.4" fill="none">\n')
        for (ax, az, bx, bz) in mesh:
            p, q = px(ax, az), px(bx, bz)
            f.write('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f"/>\n'
                    % (p[0], p[1], q[0], q[1]))
        f.write('</g>\n<g stroke="#000000" stroke-width="1.4" fill="none">\n')
        for (ax, az, bx, bz) in seg:
            p, q = px(ax, az), px(bx, bz)
            f.write('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f"/>\n'
                    % (p[0], p[1], q[0], q[1]))
        f.write('</g>\n')
        for (x, z, kind) in props:
            p = px(x, z)
            f.write('<circle cx="%.1f" cy="%.1f" r="2.0" fill="%s"/>\n'
                    % (p[0], p[1], col.get(kind, "#000")))
        f.write('</svg>\n')
    png = os.path.splitext(out)[0] + ".png"
    _raster(png, seg, mesh, props, x0, x1, z0, z1)
    print("wrote %s and %s  (%d road segments, %d mesh edges, %d objects)"
          % (out, png, len(seg), len(mesh), len(props)))


def _raster(fn, seg, mesh, props, x0, x1, z0, z1):
    """A PNG next to the SVG so a human can look without a browser."""
    import zlib
    W = 1400
    sc = W / max(x1 - x0, z1 - z0, 1e-6)
    H = int((z1 - z0) * sc) + 2
    W += 2
    px = bytearray(b"\x0b\x0b\x12" * W * H)

    def put(x, y, c):
        x, y = int(x), int(y)
        if 0 <= x < W and 0 <= y < H:
            i = (y * W + x) * 3
            px[i], px[i + 1], px[i + 2] = c

    def line(ax, az, bx, bz, c):
        p0 = ((ax - x0) * sc, H - (az - z0) * sc)
        p1 = ((bx - x0) * sc, H - (bz - z0) * sc)
        n = int(max(abs(p1[0] - p0[0]), abs(p1[1] - p0[1]))) + 1
        for k in range(n + 1):
            t = k / n
            put(p0[0] + (p1[0] - p0[0]) * t, p0[1] + (p1[1] - p0[1]) * t, c)

    for (ax, az, bx, bz) in mesh:
        line(ax, az, bx, bz, (56, 56, 72))
    for (ax, az, bx, bz) in seg:
        line(ax, az, bx, bz, (64, 208, 255))
    col = {4: (255, 90, 40), 6: (90, 160, 255), 2: (80, 230, 80),
           3: (230, 80, 230), 0: (230, 230, 80)}
    for (x, z, kind) in props:
        cx, cy = (x - x0) * sc, H - (z - z0) * sc
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                put(cx + dx, cy + dy, col.get(kind, (255, 255, 255)))

    raw = b"".join(b"\x00" + bytes(px[y * W * 3:(y + 1) * W * 3])
                   for y in range(H))

    def ck(t, dd):
        return (struct.pack(">I", len(dd)) + t + dd
                + struct.pack(">I", zlib.crc32(t + dd) & 0xFFFFFFFF))
    open(fn, "wb").write(
        b"\x89PNG\r\n\x1a\n"
        + ck(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
        + ck(b"IDAT", zlib.compress(raw, 6)) + ck(b"IEND", b""))


# ---------------------------------------------------------------------------
# verify
# ---------------------------------------------------------------------------
def cmd_verify(argv):
    d = argv[0] if argv else os.path.join("work", "disc_us", "DATA")
    fails = Counter()
    n = Counter()

    def chk(ok, name):
        n[name] += 1
        if not ok:
            fails[name] += 1

    print("== structural checks over every distinct type 0/8 cell ==")
    ids = defaultdict(lambda: defaultdict(set))
    for p in stream_files(d):
        seen = set()
        for i, b in C.iter_chunks(p):
            t = b[3] >> 4
            if t not in (0, 8, 9):
                continue
            rid = u32(b, 0) & 0x0FFFFFFF
            ids[p][t].add(rid)
            if t == 9 or (t, rid) in seen:
                continue
            seen.add((t, rid))
            nw = u32(b, 0x20)
            body = 0x20 + nw * 4
            chk(body + 0x44 <= CHUNK, "1 body header inside the chunk")
            if body + 0x44 > CHUNK:
                continue
            c = Cell(b)
            chk(c.rel[0] == (body - 0x20) + 0x40,
                "2 region 0 starts at body+0x40")
            chk(all(c.rel[j] <= c.rel[j + 1] for j in range(7)),
                "3 region offsets ascend")
            chk(c.rsize[2] % 12 == 0 and len(c.groups) == c.na + 3 * c.nb,
                "4 region 2 holds A+3B records of 12 bytes")
            run = 0
            okb = True
            for (o, cnt) in c.banks:
                okb = okb and o == run * 8
                run += cnt
            chk(okb, "5 region 1 bank offsets are the running vertex prefix")
            chk(run * 8 + 4 == c.rsize[5],
                "6 region 1 vertex total fills region 5")
            chk(c.prim_bytes_needed(0, c.na + c.nb) == c.rsize[6],
                "7 region 6 size == band 0 primitive bytes")
            r0 = c.R[0]
            o0 = [u32(b, r0 + 0x24 + 4 * j) for j in range(5)]
            cnts = [u16(b, r0 + 2 * j) for j in range(5)]
            chk(o0 == sorted(o0), "8 region 0 sub-array offsets ascend")
            okk = True
            for k in range(4):
                stride = OBJ_ARRAYS[k][2]
                okk = okk and (o0[k + 1] - o0[k]) == cnts[k] * stride
            okk = okk and (c.rsize[0] - o0[4]) == cnts[4] * OBJ_ARRAYS[4][2]
            chk(okk, "9 region 0 sub-array sizes == count * stride")
            chk(u32(b, r0 + 0x0C) == c.id, "10 region 0 repeats the resource id")
            bad = 0
            for kind, lst in c.objs.items():
                for o in lst:
                    if o.kind != kind:
                        bad += 1
                    m = math.sqrt(sum(v * v for v in o.normal))
                    if abs(m - 1.0) > 0.002:
                        bad += 1
            chk(bad == 0, "11 object kind tag matches its array, normal is unit")
            allp = c.prims6 + c.prims7
            chk(all(max(q.idx) < c.nvert for q in allp) if allp else True,
                "12 primitive indices are inside the vertex array")
            chk(all(q.clut in (0xFFFF, 0x0CBF, 0x0BFF) for q in allp)
                if allp else True,
                "13 clut is one of three on-disc values")
            texok = set(q.tex for q in allp)
            chk(texok <= set(c.pairs) | {0x7C00, 0x7800},
                "14 texture ref is a header pair value, 0x7800 or 0x7C00")
    for p in ids:
        s8, s9 = ids[p][8], ids[p][9]
        if s8 or s9:
            print("   %-16s type 8 ids %d, type 9 ids %d, type 9 without a "
                  "type 8: %d" % (os.path.basename(p), len(s8), len(s9),
                                  len(s9 - s8)))
            chk(not (s9 - s8), "15 every type 9 id has a type 8 partner")
    for name in sorted(n):
        print("   %-58s %5d checked, %d failed" % (name, n[name], fails[name]))

    # -- type 9 payload is the cell's region 7 ------------------------------
    print("== type 9 payload == region 7 of the type 8 cell with the same id ==")
    okc = badc = 0
    for p in stream_files(d):
        cells = load_cells(p, want_type=(8,))
        for (t, rid), (i, c) in cells.items():
            if c.raw9 is None:
                continue
            o = 0x20
            ok = True
            for (ofs, n3, n4) in c.groups[c.na + c.nb:]:
                if 0x20 + ofs != o:
                    ok = False
                o += n3 * 20 + n4 * 24
            ok = ok and o <= CHUNK
            okc += ok
            badc += (not ok)
    print("   %d cells consistent, %d not" % (okc, badc))

    # -- the oracle ---------------------------------------------------------
    ram = os.path.join("work", "oracle", "state", "rr-race", "ram.bin")
    if os.path.exists(ram):
        print("== oracle: relocated region pointers in live RAM ==")
        verify_oracle(d, ram)
    else:
        print("== oracle: %s absent, skipped ==" % ram)

    # -- placement ----------------------------------------------------------
    s1 = os.path.join(d, "STREAM1.STR")
    if os.path.exists(s1):
        print("== placement: every object position lies beside a road ==")
        ri = RoadIndex(s1)
        ds = []
        for (t, rid), (i, c) in load_cells(s1).items():
            for kind, lst in c.objs.items():
                for o in lst:
                    e, dist = ri.nearest(o.pos)
                    if e:
                        ds.append(dist)
        ds.sort()
        if ds:
            print("   %d objects: median %.1f, p90 %.1f, p99 %.1f, max %.1f "
                  "world units from the nearest road slice"
                  % (len(ds), ds[len(ds) // 2], ds[int(len(ds) * 0.9)],
                     ds[int(len(ds) * 0.99)], ds[-1]))
        print("== placement: rec+0x20 is the signed lateral offset ==")
        err, lats = [], []
        for (t, rid), (i, c) in load_cells(s1).items():
            for kind, lst in c.objs.items():
                for o in lst:
                    e, dist = ri.nearest(o.pos)
                    if e is None or dist > 60:
                        continue
                    w, r = e[0], e[1]
                    dx = (o.pos[0] - w[0], o.pos[1] - w[1], o.pos[2] - w[2])
                    lat = dx[0] * r[0] + dx[1] * r[1] + dx[2] * r[2]
                    err.append(min(abs(lat - o.f20), abs(lat + o.f20)))
                    lats.append(lat)
        err.sort()
        print("   %d objects: |lateral(geometry) -+ rec+0x20| median %.3f, "
              "p90 %.3f, p99 %.3f world units"
              % (len(err), err[len(err) // 2], err[int(len(err) * 0.9)],
                 err[int(len(err) * 0.99)]))
        on = sum(1 for x in lats if abs(x) < 3.0)
        print("   only %d of %d (%.2f%%) sit within 3 world units of the "
              "centre line - the props line the road, they are not on it"
              % (on, len(lats), 100.0 * on / len(lats)))
        print("== placement: cell mesh vertices lie beside a road ==")
        vs = []
        for kk, ((t, rid), (i, c)) in enumerate(load_cells(s1).items()):
            if kk % 17:
                continue
            for vi in range(0, c.nvert, 7):
                e, dist = ri.nearest(c.world(vi))
                if e:
                    vs.append(dist)
        vs.sort()
        if vs:
            print("   %d sampled vertices: median %.1f, p90 %.1f, max %.1f"
                  % (len(vs), vs[len(vs) // 2], vs[int(len(vs) * 0.9)], vs[-1]))


def verify_oracle(d, rampath):
    ram = open(rampath, "rb").read()
    index = {}
    for p in stream_files(d):
        for i, b in C.iter_chunks(p):
            t = b[3] >> 4
            if t not in (0, 8, 9):
                continue
            key = (t, u32(b, 0) & 0x0FFFFFFF)
            index.setdefault(key, b)
    bases = {}
    for key, b in index.items():
        o = ram.find(b[0x1000:0x1040])
        if o >= 0:
            bases[key] = o - 0x1000
    ok = bad = n9 = 0
    for key in sorted(bases):
        if key[0] == 9:
            continue
        base = bases[key]
        b = index[key]
        body = 0x20 + u32(b, 0x20) * 4
        for j in range(8):
            live = u32(ram, base + body + 0x20 + 4 * j)
            exp = 0x80000000 + base + 0x20 + u32(b, body + 0x20 + 4 * j)
            if live == exp:
                ok += 1
            elif j == 7 and key[0] == 8:
                p9 = bases.get((9, key[1]))
                if live == 0 or (p9 is not None and live == 0x80000000 + p9 + 0x20):
                    n9 += 1
                elif live and u32(ram, (live & 0x1FFFFF) - 0x20) >> 28 == 9:
                    n9 += 1
                else:
                    bad += 1
            else:
                bad += 1
    print("   %d cell chunks resident verbatim; %d/%d region pointers equal "
          "chunkBase+0x20+offset, %d type-8 region 7 pointers redirected to a "
          "type 9 payload (or null), %d unexplained"
          % (len([k for k in bases if k[0] != 9]), ok, ok + bad + n9, n9, bad))
    # the cell slot table
    SLOT = 0x000D87E8
    live = []
    for i in range(24):
        o = SLOT + 112 * i
        rid = u32(ram, o)
        if rid == 0xFFFFFFFF:
            continue
        live.append((i, rid, u32(ram, o + 4), u32(ram, o + 0x4C),
                     ram[o + 0x48], ram[o + 0x49]))
    print("   cell slot table at guest 0x800D87E8, %d of 24 slots in use:"
          % len(live))
    for (i, rid, body, ext, b48, b49) in live:
        exp = ""
        for key in bases:
            if key[1] == rid and key[0] != 9:
                bb = index[key]
                nw = u32(bb, 0x20)
                want = 0x80000000 + bases[key] + 0x20 + nw * 4
                wext = 0x80000000 + bases[key] + 0x20 + nw * 4 - 48
                exp = " body %s extent %s" % ("OK" if body == want else "**",
                                              "OK" if ext == wext else "**")
        print("      slot %2d id %08x body %08x extentPtr %08x B+1=%d A+2B=%d%s"
              % (i, rid, body, ext, b48, b49, exp))


# ---------------------------------------------------------------------------
def main(argv):
    if not argv:
        print(__doc__)
        return 1
    cmd, rest = argv[0], argv[1:]
    fn = {"info": cmd_info, "scan": cmd_scan, "place": cmd_place,
          "obj": cmd_obj, "plot": cmd_plot, "verify": cmd_verify}.get(cmd)
    if not fn:
        print(__doc__)
        return 1
    fn(rest)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
