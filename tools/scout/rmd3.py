#!/usr/bin/env python3
"""Scout probe for the RMD3 3D model container of Road Rash: Jailbreak (PS1, SLUS_01053).

Throwaway RE probe -- not production code.  Everything it prints is derived from the
bytes of the player's own disc extract; nothing about the format is assumed, every
structural claim below is re-checked at parse time and a file is only reported as
"pass" when every single byte of it has been assigned to a known field.

Container shape (derived, see docs/formats/rmd3.md):

    file  := RMD3+                       chained, last one ends exactly at EOF
    RMD3  := hdr(0x10) group+            group count = low byte of hdr+0x0C
    group := DOD3 DPD3{n} BBD3           n = DOD3+0x18
    DOD3  := hdr(0x34) blobA verts [normals] [quadsD] [vertNormIdx]
    DPD3  := hdr(0x1C) prim{n}           prim = 20 bytes, textured quad
    BBD3  := hdr(0x10) centre[4h] half[3h] radius[1h]

Sub-commands:
    info <file>            chunk tree + counts
    obj  <file> <out>      export raw (unassembled) geometry to Wavefront OBJ
    asm  <file> <out> [lod]  export the ASSEMBLED rest pose (see docs/formats/rmd3.md section 9)
    skel [overlay]         print the part-attachment programs read from RASHCDG.BIN
    asmscan <dir>          assemble every multi-part group, score against BBD3
    scan <dir> [glob]      parse every matching file as RMD3, report pass/fail
    dmd3 <file>            probe the DMD3 (ANIMTBL*.PSX) sibling family

`asm`, `skel` and `asmscan` need the race overlay RASHCDG.BIN, because the part
attachment topology lives in the overlay, not in the .GEO.  It defaults to
work/disc_us/RASHCDG.BIN and can be pointed elsewhere with --overlay <path>.
No copy of it is kept in this repository.
"""

import os
import struct
import sys
import glob
import collections

# ---------------------------------------------------------------------------
# primitives
# ---------------------------------------------------------------------------

U32 = struct.Struct('<I')
U16 = struct.Struct('<H')


def u32(d, o):
    return U32.unpack_from(d, o)[0]


def u16(d, o):
    return U16.unpack_from(d, o)[0]


def s16(d, o):
    return struct.unpack_from('<h', d, o)[0]


def align4(x):
    return (x + 3) & ~3


class ParseError(Exception):
    pass


# ---------------------------------------------------------------------------
# model
# ---------------------------------------------------------------------------

class Prim:
    """One 20-byte DPD3 record: a textured quad (PS1 POLY_GT4-style UV block)."""
    __slots__ = ('u', 'v', 'clut', 'tpage', 'idx')

    def __init__(self, d, o):
        self.u = (d[o + 0], d[o + 4], d[o + 8], d[o + 10])
        self.v = (d[o + 1], d[o + 5], d[o + 9], d[o + 11])
        self.clut = u16(d, o + 2)
        self.tpage = u16(d, o + 6)
        self.idx = (u16(d, o + 12), u16(d, o + 14), u16(d, o + 16), u16(d, o + 18))


class SubMesh:
    """One DPD3 chunk."""

    def __init__(self, d, o, size, cov):
        if size < 0x1C:
            raise ParseError('DPD3 @%06x size %d < 0x1c' % (o, size))
        self.off = o
        self.size = size
        self.id = u32(d, o + 0x08)
        self.pad0C = d[o + 0x0C]            # always 0
        self.index = d[o + 0x0D]            # sub-mesh ordinal inside the group
        self.nverts = u16(d, o + 0x0E)      # vertices this sub-mesh owns
        self.vbase = u32(d, o + 0x10)       # first vertex index in the DOD3 array
        self.listptr = u32(d, o + 0x14)     # always 0x18 = offset of the count below
        self.count = u32(d, o + 0x18)
        if self.listptr != 0x18:
            raise ParseError('DPD3 @%06x +14 = %#x, expected 0x18' % (o, self.listptr))
        body = size - 0x1C
        if self.count == 0:
            if body != 0:
                raise ParseError('DPD3 @%06x count 0 but %d trailing bytes' % (o, body))
            self.stride = 0
        else:
            if body % self.count:
                raise ParseError('DPD3 @%06x body %d not divisible by count %d'
                                 % (o, body, self.count))
            self.stride = body // self.count
            if self.stride != 20:
                raise ParseError('DPD3 @%06x primitive stride %d, expected 20'
                                 % (o, self.stride))
        self.prims = [Prim(d, o + 0x1C + i * 20) for i in range(self.count)]
        cov.add(o, size, 'DPD3')


class Group:
    """One DOD3 + its DPD3 sub-meshes + the trailing BBD3."""

    HDR = 0x34

    def __init__(self, d, o, size, cov):
        if size < self.HDR:
            raise ParseError('DOD3 @%06x size %d < 0x34' % (o, size))
        self.off = o
        self.size = size
        self.id = u32(d, o + 0x08)
        self.flags = u32(d, o + 0x0C)
        self.radius = u32(d, o + 0x10)
        self.scale = u32(d, o + 0x14)       # 12.12 fixed point, 0x1000 == 1.0
        self.nsub = u32(d, o + 0x18)        # number of DPD3 chunks that follow
        self.w1C = u32(d, o + 0x1C)
        self.ptr = [u32(d, o + 0x20 + 4 * i) for i in range(5)]

        # every non-null pointer is a byte offset relative to the DOD3 tag,
        # they are strictly increasing and the last region ends at the chunk end.
        nz = [p for p in self.ptr if p]
        if nz != sorted(nz) or (nz and nz[0] != self.HDR):
            raise ParseError('DOD3 @%06x pointers not ordered: %s'
                             % (o, [hex(p) for p in self.ptr]))
        for p in nz:
            if p >= size:
                raise ParseError('DOD3 @%06x pointer %#x beyond size %#x' % (o, p, size))
        bounds = nz + [size]
        end = {}
        for i, p in enumerate(nz):
            end[p] = bounds[i + 1]

        # region 0: opaque, variable-length.  Not decoded.
        self.blobA = b''
        if self.ptr[0]:
            self.blobA = d[o + self.ptr[0]: o + end[self.ptr[0]]]

        def counted(slot, stride, name):
            """u32 count followed by count*stride bytes, padded to 4."""
            p = self.ptr[slot]
            if not p:
                return []
            n = u32(d, o + p)
            want = align4(4 + n * stride)
            got = end[p] - p
            if want != got:
                raise ParseError('DOD3 @%06x %s: count %d stride %d -> %d bytes, '
                                 'region is %d' % (o, name, n, stride, want, got))
            return [o + p + 4 + i * stride for i in range(n)]

        self.vofs = counted(1, 8, 'verts')
        self.nofs = counted(2, 8, 'listC')
        self.xofs = counted(3, 8, 'listD')
        self.fofs = counted(4, 2, 'listE')

        self.verts = [struct.unpack_from('<3h', d, p) for p in self.vofs]
        self.listC = [struct.unpack_from('<4h', d, p) for p in self.nofs]
        self.listD = [struct.unpack_from('<4h', d, p) for p in self.xofs]
        self.listE = [u16(d, p) for p in self.fofs]

        if self.listE and len(self.listE) != len(self.verts):
            raise ParseError('DOD3 @%06x listE %d entries != %d vertices'
                             % (o, len(self.listE), len(self.verts)))
        cov.add(o, size, 'DOD3')

        self.subs = []
        self.bbox = None


class Object:
    """One RMD3 chunk."""

    HDR = 0x10

    def __init__(self, d, o, size, cov):
        self.off = o
        self.size = size
        self.id = u32(d, o + 0x08)
        self.w0C = u32(d, o + 0x0C)
        self.ngroups = self.w0C & 0xFF
        self.stamp = self.w0C >> 8
        cov.add(o, self.HDR, 'RMD3hdr')
        self.groups = []

        p = o + self.HDR
        end = o + size
        while p < end:
            tag = d[p:p + 4]
            sz = u32(d, p + 4)
            if sz < 8 or p + sz > end:
                raise ParseError('%s @%06x bad size %#x' % (tag, p, sz))
            if tag != b'DOD3':
                raise ParseError('expected DOD3 @%06x, found %r' % (p, tag))
            g = Group(d, p, sz, cov)
            if g.id != self.id:
                raise ParseError('DOD3 @%06x id %d != RMD3 id %d' % (p, g.id, self.id))
            p += sz
            vbase = 0
            for k in range(g.nsub):
                if p >= end or d[p:p + 4] != b'DPD3':
                    raise ParseError('expected DPD3 @%06x, found %r' % (p, d[p:p + 4]))
                sz = u32(d, p + 4)
                s = SubMesh(d, p, sz, cov)
                if s.id != self.id:
                    raise ParseError('DPD3 @%06x id %d != RMD3 id %d' % (p, s.id, self.id))
                if s.pad0C != 0:
                    raise ParseError('DPD3 @%06x +0C = %d, expected 0' % (p, s.pad0C))
                if s.index != k:
                    raise ParseError('DPD3 @%06x +0D = %d, expected ordinal %d'
                                     % (p, s.index, k))
                if s.vbase != vbase:
                    raise ParseError('DPD3 @%06x vertBase %d, prefix sum says %d'
                                     % (p, s.vbase, vbase))
                vbase += s.nverts
                g.subs.append(s)
                p += sz
            if g.subs and vbase != len(g.verts):
                raise ParseError('DOD3 @%06x sub-mesh vertices %d != %d vertices'
                                 % (g.off, vbase, len(g.verts)))
            if p >= end or d[p:p + 4] != b'BBD3':
                raise ParseError('expected BBD3 @%06x, found %r' % (p, d[p:p + 4]))
            sz = u32(d, p + 4)
            if sz != 0x20:
                raise ParseError('BBD3 @%06x size %#x, expected 0x20' % (p, sz))
            if u32(d, p + 0x08) != self.id:
                raise ParseError('BBD3 @%06x id %d != RMD3 id %d'
                                 % (p, u32(d, p + 0x08), self.id))
            if u32(d, p + 0x0C) != 0:
                raise ParseError('BBD3 @%06x +0C != 0' % p)
            g.bbox = struct.unpack_from('<8h', d, p + 0x10)
            if g.bbox[3] != 0:
                raise ParseError('BBD3 @%06x centre pad != 0' % p)
            if g.radius != (g.bbox[7] & 0xFFFF):
                raise ParseError('BBD3 @%06x radius %d != DOD3 radius %d'
                                 % (p, g.bbox[7], g.radius))
            cov.add(p, sz, 'BBD3')
            p += sz
            self.groups.append(g)

        if len(self.groups) != self.ngroups:
            raise ParseError('RMD3 @%06x header says %d groups, found %d'
                             % (o, self.ngroups, len(self.groups)))


class Coverage:
    """Tracks which bytes of the file a field has claimed."""

    def __init__(self, n):
        self.n = n
        self.map = bytearray(n)
        self.dupes = []

    def add(self, o, size, what):
        for i in range(o, o + size):
            if self.map[i]:
                self.dupes.append((i, what))
                break
            self.map[i] = 1

    def holes(self):
        out = []
        start = None
        for i in range(self.n):
            if not self.map[i]:
                if start is None:
                    start = i
            elif start is not None:
                out.append((start, i - start))
                start = None
        if start is not None:
            out.append((start, self.n - start))
        return out


class Model:
    def __init__(self, path):
        self.path = path
        d = open(path, 'rb').read()
        self.data = d
        self.cov = Coverage(len(d))
        self.objects = []
        o = 0
        while o < len(d):
            if o + 16 > len(d):
                raise ParseError('trailing %d bytes at %06x' % (len(d) - o, o))
            if d[o:o + 4] != b'RMD3':
                raise ParseError('expected RMD3 @%06x, found %r' % (o, d[o:o + 4]))
            sz = u32(d, o + 4)
            if sz < 0x10 or o + sz > len(d):
                raise ParseError('RMD3 @%06x bad size %#x' % (o, sz))
            self.objects.append(Object(d, o, sz, self.cov))
            o += sz
        self.dupes = self.cov.dupes
        self.holes = self.cov.holes()
        if self.dupes:
            raise ParseError('overlapping fields at %06x' % self.dupes[0][0])
        if self.holes:
            raise ParseError('%d unclaimed byte ranges, first at %06x len %d'
                             % (len(self.holes), self.holes[0][0], self.holes[0][1]))

    # -- derived checks ----------------------------------------------------
    def index_report(self):
        """Are primitive indices absolute into the DOD3 vertex array?"""
        bad = []
        for oi, ob in enumerate(self.objects):
            for gi, g in enumerate(ob.groups):
                nv = len(g.verts)
                cover = 0
                for s in g.subs:
                    cover += s.nverts
                    for p in s.prims:
                        for i in p.idx:
                            if i >= nv:
                                bad.append((ob.off, g.off, s.off, i, nv))
                if g.subs and cover != nv:
                    bad.append((ob.off, g.off, None, cover, nv))
        return bad


# ---------------------------------------------------------------------------
# part skeleton (see docs/formats/rmd3.md section 9)
#
# The attachment topology is NOT in the .GEO.  It is a static table inside the
# race overlay RASHCDG.BIN, so this tool reads it out of the player's own overlay
# under work\ and never stores a copy.
# ---------------------------------------------------------------------------

OVERLAY_BASE = 0x8005B5E8
SKEL_WORDS = 0x800CC790          # 49 u32 link/control words
SKEL_INDEX = 0x800CC854          # 8 x (u8 startWord, u8 linkCount, u8 passes)
SKEL_NWORDS = 49
SKEL_NPROG = 8
DEFAULT_OVERLAY = os.path.join('work', 'disc_us', 'RASHCDG.BIN')


class Skeleton:
    """The per-part attachment programs, decoded out of RASHCDG.BIN.

    One program = an ordered list of (parentPart, vertexOffset).  Entry i places
    part i+1: origin(i+1) = origin(parent) + verts[vertBase(parent) + k].
    """

    def __init__(self, overlay_path):
        d = open(overlay_path, 'rb').read()
        self.path = overlay_path
        wo = SKEL_WORDS - OVERLAY_BASE
        io = SKEL_INDEX - OVERLAY_BASE
        if io + SKEL_NPROG * 3 > len(d):
            raise ParseError('%s is too short to be RASHCDG.BIN' % overlay_path)
        self.words = [u32(d, wo + 4 * i) for i in range(SKEL_NWORDS)]
        self.index = [(d[io + 3 * i], d[io + 3 * i + 1], d[io + 3 * i + 2])
                      for i in range(SKEL_NPROG)]
        self.progs = []
        for e, (start, nlink, npass) in enumerate(self.index):
            links = []
            i = start
            while i < SKEL_NWORDS:
                w = self.words[i]
                i += 1
                if w == 0:
                    break
                if (w & 3) == 3:            # control marker, not a link
                    continue
                links.append(((w >> 13) & 0x1F, ((w >> 6) & 0x70) >> 4))
                if npass <= 1 and len(links) >= nlink:
                    break
                if npass > 1 and len(links) >= nlink * npass - 5:
                    break
            self.progs.append(links)
        # index the programs by the part count they describe (several may share one)
        self.by_parts = {}
        for e, links in enumerate(self.progs):
            self.by_parts.setdefault(len(links) + 1, []).append((e, links))

    def candidates(self, nparts):
        return self.by_parts.get(nparts, [])


def _apply_links(g, links):
    n = len(g.subs)
    pos = {0: (0, 0, 0)}
    for c in range(1, n):
        p, k = links[c - 1]
        if p not in pos or p >= n:
            return None
        vi = g.subs[p].vbase + k
        if vi >= len(g.verts):
            return None
        av = g.verts[vi]
        pos[c] = (pos[p][0] + av[0], pos[p][1] + av[1], pos[p][2] + av[2])
    return pos


def _move(g, pos):
    out = list(g.verts)
    for i, s in enumerate(g.subs):
        t = pos[i]
        for j in range(s.vbase, s.vbase + s.nverts):
            v = g.verts[j]
            out[j] = (v[0] + t[0], v[1] + t[1], v[2] + t[2])
    return out


def group_origins(g, skel):
    """Rest-pose origins per part, choosing the program that best reproduces BBD3.

    Returns (origins, programIndex, bboxError) or (None, None, None).
    """
    n = len(g.subs)
    if n < 2:
        return ({0: (0, 0, 0)} if n else {}), None, bbox_error(g, g.verts)
    best = None
    for e, links in skel.candidates(n):
        pos = _apply_links(g, links)
        if pos is None:
            continue
        err = bbox_error(g, _move(g, pos))
        if err is not None and (best is None or err < best[2]):
            best = (pos, e, err)
    return best if best else (None, None, None)


def assembled_verts(g, skel):
    """(verts, origins, programIndex, bboxError); verts is None if no program fits."""
    pos, prog, err = group_origins(g, skel)
    if pos is None:
        return None, None, None, None
    if not pos:
        return list(g.verts), pos, prog, err
    return _move(g, pos), pos, prog, err


def bbd3_box(g):
    b = g.bbox
    return (b[0] - b[4], b[0] + b[4], b[1] - b[5], b[1] + b[5], b[2] - b[6], b[2] + b[6])


def aabb(vs):
    xs = [v[0] for v in vs]; ys = [v[1] for v in vs]; zs = [v[2] for v in vs]
    return (min(xs), max(xs), min(ys), max(ys), min(zs), max(zs))


def bbox_error(g, vs):
    """Max absolute difference between an AABB and the authored BBD3 box."""
    if not vs:
        return None
    t = bbd3_box(g)
    a = aabb(vs)
    return max(abs(a[i] - t[i]) for i in range(6))


def lod_factor(g):
    """Multiplier that puts a group's vertices into the finest (LOD0) unit.

    DOD3+0x0E bits 12..15 is a coordinate shift exponent; the draw path shifts by
    the difference between groups (RASHCDG 0x80066ce8 / 0x80066d24).
    """
    return 1 << (4 - (((g.flags >> 16) & 0xFFFF) >> 12))


# ---------------------------------------------------------------------------
# commands
# ---------------------------------------------------------------------------

def cmd_info(path):
    m = Model(path)
    d = m.data
    print('%s  %d bytes  %d RMD3 object(s)' % (path, len(d), len(m.objects)))
    for ob in m.objects:
        print('%06x RMD3  size=%-6x id=%-5d stamp=%06x groups=%d'
              % (ob.off, ob.size, ob.id, ob.stamp, ob.ngroups))
        for g in ob.groups:
            xs = [v[0] for v in g.verts] or [0]
            ys = [v[1] for v in g.verts] or [0]
            zs = [v[2] for v in g.verts] or [0]
            print('  %06x DOD3 size=%-5x flags=%08x radius=%-5d scale=%-5d nsub=%-3d '
                  'w1C=%-4d blobA=%-3d verts=%-4d listC=%-4d listD=%-3d listE=%-4d'
                  % (g.off, g.size, g.flags, g.radius, g.scale, g.nsub, g.w1C,
                     len(g.blobA), len(g.verts), len(g.listC), len(g.listD),
                     len(g.listE)))
            print('         vert aabb x[%d..%d] y[%d..%d] z[%d..%d]'
                  % (min(xs), max(xs), min(ys), max(ys), min(zs), max(zs)))
            for s in g.subs:
                if s.count:
                    tp = sorted({p.tpage for p in s.prims})
                    cl = sorted({p.clut for p in s.prims})
                    lo = min(min(p.idx) for p in s.prims)
                    hi = max(max(p.idx) for p in s.prims)
                    print('    %06x DPD3 #%-2d vbase=%-4d nverts=%-4d quads=%-4d '
                          'idx[%d..%d] tpage=%s clut=%s'
                          % (s.off, s.index, s.vbase, s.nverts, s.count, lo, hi,
                             ','.join('%04x' % t for t in tp[:4]),
                             ','.join('%04x' % c for c in cl[:4])))
                else:
                    print('    %06x DPD3 #%-2d vbase=%-4d nverts=%-4d quads=0'
                          % (s.off, s.index, s.vbase, s.nverts))
            c = g.bbox
            print('    BBD3 centre=(%d,%d,%d) pad=%d half=(%d,%d,%d) radius=%d'
                  % c)
    bad = m.index_report()
    print('index/vertex-coverage violations: %d' % len(bad))
    for b in bad[:10]:
        print('   ', b)


def _obj_group(fh, g, base, tbase, name, scale, verts=None, mul=1):
    """Write one group.  `base` / `tbase` are the file-global v / vt counts so far."""
    if not g.verts:
        return base, tbase
    s = (g.scale / 4096.0 if scale else 1.0) * mul
    src = verts if verts is not None else g.verts
    fh.write('o %s\n' % name)
    for x, y, z in src:
        # PS1 screen space is Y-down; flip Y so the OBJ is Y-up.
        fh.write('v %.4f %.4f %.4f\n' % (x * s, -y * s, z * s))
    uvs = []
    for sm in g.subs:
        for p in sm.prims:
            for k in range(4):
                uvs.append((p.u[k], p.v[k]))
    for uu, vv in uvs:
        fh.write('vt %.6f %.6f\n' % (uu / 256.0, 1.0 - vv / 256.0))
    t = tbase
    for sm in g.subs:
        if not sm.count:
            continue
        fh.write('g %s_sub%02d\n' % (name, sm.index))
        for p in sm.prims:
            a, b, c, e = [i + base + 1 for i in p.idx]
            # PS1 quad vertex order is 0,1,2,3 == two triangles (0,1,2) (1,3,2)
            ta, tb, tc, td = t + 1, t + 2, t + 3, t + 4
            t += 4
            fh.write('f %d/%d %d/%d %d/%d\n' % (a, ta, b, tb, c, tc))
            fh.write('f %d/%d %d/%d %d/%d\n' % (b, tb, e, td, c, tc))
    return base + len(g.verts), tbase + len(uvs)


def cmd_obj(path, out, lod=None, scale=True):
    m = Model(path)
    base = 0
    tbase = 0
    nfaces = 0
    with open(out, 'w') as fh:
        fh.write('# %s -> %s (tools/scout/rmd3.py)\n' % (os.path.basename(path), out))
        for oi, ob in enumerate(m.objects):
            for gi, g in enumerate(ob.groups):
                if lod is not None and gi != lod:
                    continue
                name = 'obj%02d_id%d_lod%d' % (oi, ob.id, gi)
                nfaces += sum(s.count for s in g.subs) * 2
                base, tbase = _obj_group(fh, g, base, tbase, name, scale)
    # report bounds of what we wrote
    allv = [v for ob in m.objects for gi, g in enumerate(ob.groups)
            if lod is None or gi == lod for v in g.verts]
    if allv:
        xs = [v[0] for v in allv]
        ys = [v[1] for v in allv]
        zs = [v[2] for v in allv]
        print('%s -> %s  verts=%d tris=%d  aabb x[%d..%d] y[%d..%d] z[%d..%d]'
              % (path, out, len(allv), nfaces,
                 min(xs), max(xs), min(ys), max(ys), min(zs), max(zs)))
    else:
        print('%s -> %s  (empty)' % (path, out))


def cmd_asm(path, out, overlay, lod=0, scale=True):
    """Export the ASSEMBLED rest pose: every part moved to its attachment point."""
    skel = Skeleton(overlay)
    m = Model(path)
    base = tbase = nfaces = 0
    rows = []
    with open(out, 'w') as fh:
        fh.write('# %s assembled rest pose -> %s (tools/scout/rmd3.py asm)\n'
                 % (os.path.basename(path), out))
        fh.write('# skeleton programs read from %s\n' % overlay)
        for oi, ob in enumerate(m.objects):
            for gi, g in enumerate(ob.groups):
                if lod is not None and gi != lod:
                    continue
                vs, pos, prog, err = assembled_verts(g, skel)
                if vs is None:
                    vs = g.verts
                    err = bbox_error(g, g.verts)
                    state = 'NO PROGRAM (%d parts)' % g.nsub
                elif g.nsub < 2:
                    state = 'single part, nothing to assemble'
                else:
                    state = 'assembled, program[%d]' % prog
                raw = bbox_error(g, g.verts)
                mul = lod_factor(g)
                rows.append((ob.id, gi, g.nsub, state, err, raw))
                name = 'obj%02d_id%d_lod%d' % (oi, ob.id, gi)
                nfaces += sum(s.count for s in g.subs) * 2
                base, tbase = _obj_group(fh, g, base, tbase, name, scale, vs, mul)
    print('%s -> %s  (%d tris)' % (path, out, nfaces))
    print('  %-5s %-4s %-5s %-24s %-10s %s'
          % ('id', 'lod', 'parts', 'state', 'bbox err', 'bbox err if NOT assembled'))
    for mid, gi, ns, state, err, raw in rows:
        print('  %-5d %-4d %-5d %-24s %-10s %s'
              % (mid, gi, ns, state,
                 '-' if err is None else err, '-' if raw is None else raw))
    return 0


def cmd_asmscan(dirname, overlay):
    """Assemble every multi-part group in every .GEO and score it against BBD3."""
    skel = Skeleton(overlay)
    files = sorted(glob.glob(os.path.join(dirname, '*.GEO')))
    seen = set()
    rows = []
    stats = collections.Counter()
    for p in files:
        m = Model(p)
        for ob in m.objects:
            for gi, g in enumerate(ob.groups):
                if g.nsub < 2:
                    stats['single-part groups'] += 1
                    continue
                if (ob.id, gi) in seen:
                    continue
                seen.add((ob.id, gi))
                vs, pos, prog, err = assembled_verts(g, skel)
                raw = bbox_error(g, g.verts)
                if vs is None:
                    stats['NO PROGRAM'] += 1
                    rows.append((os.path.basename(p), ob.id, gi, g.nsub, None, None, raw))
                    continue
                stats['assembled'] += 1
                stats['bbox error <= 9'] += 1 if err <= 9 else 0
                rows.append((os.path.basename(p), ob.id, gi, g.nsub, prog, err, raw))
    print('%-14s %-5s %-4s %-5s %-8s %-9s %s'
          % ('file', 'id', 'lod', 'parts', 'program', 'bbox err', 'err if NOT assembled'))
    for f, mid, gi, ns, prog, err, raw in rows:
        print('%-14s %-5d %-4d %-5d %-8s %-9s %s'
              % (f, mid, gi, ns, '-' if prog is None else prog,
                 '-' if err is None else err, raw))
    print('-' * 74)
    print('distinct multi-part groups: %d' % len(rows))
    print('%s' % dict(stats))
    worst = max((r[5] for r in rows if r[5] is not None), default=None)
    print('worst bounding-box error over all assembled groups: %s units' % worst)
    return 1 if stats['NO PROGRAM'] else 0


def cmd_skel(overlay):
    skel = Skeleton(overlay)
    print('skeleton programs decoded from %s' % overlay)
    print('  words at %08x (file +%05x), index at %08x (file +%05x)'
          % (SKEL_WORDS, SKEL_WORDS - OVERLAY_BASE, SKEL_INDEX, SKEL_INDEX - OVERLAY_BASE))
    for e, (start, nlink, npass) in enumerate(skel.index):
        links = skel.progs[e]
        print('  program[%d] startWord=%-3d linkCount=%-3d passes=%-3d -> %d parts'
              % (e, start, nlink, npass, len(links) + 1))
        print('      %s' % ' '.join('part%d<-part%d.v[%d]' % (i + 1, p, k)
                                    for i, (p, k) in enumerate(links)))
    return 0


def cmd_scan(d, patterns=('*.GEO',)):
    files = []
    for pat in patterns:
        files += sorted(glob.glob(os.path.join(d, pat)))
    ok = fail = 0
    stats = collections.Counter()
    rows = []
    for p in files:
        try:
            m = Model(p)
        except (ParseError, struct.error) as e:
            fail += 1
            rows.append('FAIL %-14s %s' % (os.path.basename(p), e))
            continue
        bad = m.index_report()
        nv = sum(len(g.verts) for ob in m.objects for g in ob.groups)
        nq = sum(s.count for ob in m.objects for g in ob.groups for s in g.subs)
        stats['objects'] += len(m.objects)
        stats['groups'] += sum(len(ob.groups) for ob in m.objects)
        stats['submeshes'] += sum(len(g.subs) for ob in m.objects for g in ob.groups)
        stats['verts'] += nv
        stats['quads'] += nq
        if bad:
            fail += 1
            rows.append('FAIL %-14s %d index/coverage violations (first %s)'
                        % (os.path.basename(p), len(bad), bad[0]))
        else:
            ok += 1
            rows.append('pass %-14s %8d B  obj=%-3d grp=%-4d sub=%-4d verts=%-6d quads=%-6d'
                        % (os.path.basename(p), len(m.data), len(m.objects),
                           sum(len(o.groups) for o in m.objects),
                           sum(len(g.subs) for o in m.objects for g in o.groups),
                           nv, nq))
    for r in rows:
        print(r)
    print('-' * 70)
    print('%d files: %d pass (every byte accounted for), %d fail' % (len(files), ok, fail))
    print('totals: %s' % dict(stats))
    return fail


def cmd_dmd3(path, verbose=False):
    """DMD3 (ANIMTBL*.PSX) -- sibling container.  Container is solved, the
    per-block animation payload is only partially decoded."""
    d = open(path, 'rb').read()
    o = 0
    n = 0
    rows = []
    while o < len(d):
        if d[o:o + 4] != b'DMD3':
            print('%s: expected DMD3 @%06x, found %r' % (path, o, d[o:o + 4]))
            return 1
        sz = u32(d, o + 4)
        if sz < 0x18 or o + sz > len(d):
            print('%s: bad block size %#x @%06x' % (path, sz, o))
            return 1
        rows.append((o, sz, u32(d, o + 8), d[o + 0x0C], d[o + 0x0D], d[o + 0x0E],
                     d[o + 0x0F], u16(d, o + 0x10), u16(d, o + 0x12),
                     u16(d, o + 0x14), u16(d, o + 0x16)))
        o += sz
        n += 1
    print('%-16s %7d B  %3d DMD3 blocks, chain ends at %d (%s)'
          % (os.path.basename(path), len(d), n, o,
             'EOF, exact' if o == len(d) else 'MISMATCH'))
    if not rows:
        return 0
    print('   id=%d  +0C=%d  +0D=%d  parts(+0F)=%s  blockIndex(+0E)=0..%d  ptr(+14)=%s'
          % (rows[0][2], rows[0][3], rows[0][4],
             sorted({r[6] for r in rows}), max(r[5] for r in rows),
             sorted({hex(r[9]) for r in rows})))
    if verbose:
        for r in rows:
            print('   @%06x size=%-6x idx=%-3d parts=%-3d A=%-4d B=%-4d body=%d'
                  % (r[0], r[1], r[5], r[6], r[7], r[8], r[1] - 0x18))
    return 0


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    cmd = sys.argv[1]
    if cmd == 'info':
        cmd_info(sys.argv[2])
    elif cmd == 'obj':
        lod = int(sys.argv[4]) if len(sys.argv) > 4 else None
        cmd_obj(sys.argv[2], sys.argv[3], lod)
    elif cmd == 'asm':
        args = sys.argv[2:]
        overlay = DEFAULT_OVERLAY
        if '--overlay' in args:
            i = args.index('--overlay')
            overlay = args[i + 1]
            del args[i:i + 2]
        lod = int(args[2]) if len(args) > 2 else 0
        return cmd_asm(args[0], args[1], overlay, lod)
    elif cmd == 'skel':
        return cmd_skel(sys.argv[2] if len(sys.argv) > 2 else DEFAULT_OVERLAY)
    elif cmd == 'asmscan':
        args = sys.argv[2:]
        overlay = DEFAULT_OVERLAY
        if '--overlay' in args:
            i = args.index('--overlay')
            overlay = args[i + 1]
            del args[i:i + 2]
        return cmd_asmscan(args[0], overlay)
    elif cmd == 'scan':
        pats = sys.argv[3:] or ['*.GEO']
        return 1 if cmd_scan(sys.argv[2], pats) else 0
    elif cmd == 'dmd3':
        args = [a for a in sys.argv[2:] if a != '-v']
        vb = '-v' in sys.argv
        rc = 0
        for a in args:
            for p in (sorted(glob.glob(a)) or [a]):
                rc |= cmd_dmd3(p, vb) or 0
        return rc
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
