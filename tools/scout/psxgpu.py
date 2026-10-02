#!/usr/bin/env python3
"""psxgpu.py - draw the ORIGINAL's frame from the GP0 words our interpreter recorded.

`rrverify trace` records every word the guest sends to the GPU (`gpu.bin`) but rasterises nothing.
This probe is the missing half of that oracle: a small
software model of the PlayStation GPU's drawing commands, run over the recorded words on top of the
snapshot's own VRAM, so the frame the original built can be looked at and compared with ours.

    psxgpu.py frame  --gpu work\\render\\trace\\gpu.bin --vram work\\oracle\\state\\rr-race\\vram.bin
                     [--frame 0] --out work\\render\\orig.png [--prims out.csv]
                     [--gpujson <state>\\gpu.json] [--owners orig_owners.bin]
    psxgpu.py parity orig.png ours.png --prims orig.csv --owners orig_owners.bin [--diff d.png]
                     [--min-close P] [--min-prims P] [--min-category NAME:P]

What is modelled: GP0 02 (fill), 20..3F (flat / Gouraud, textured / untextured, opaque /
semi-transparent, raw / modulated polygons), 60..7F (rectangles and sprites), 80 (VRAM copy),
A0 (CPU->VRAM), E1..E6 (draw mode, texture window, drawing area, drawing offset, mask). Textures at
4, 8 and 15 bits per pixel, CLUTs, the texture window, the four semi-transparency modes, and the
"texel 0x0000 is transparent" rule. Texture modulation is `(texel5 * colour8) >> 7` per channel.

Lines (GP0 40..5F, flat / Gouraud, single / poly-line) are drawn with both end points (the slanted ones' inner
stepping is an approximation). The fill rule of polygons is the console's: pixel (x, y) is drawn when its integer point
is inside the triangle or on a top / left edge (`_tl`; PSXGPU_EDGE=br / lb: the mirrored rule / left-and-bottom, as
controls); a quad is triangles (v0, v1, v2) and (v1, v2, v3); a sprite covers
[x, x + w) x [y, y + h).

What is NOT modelled, stated plainly: dithering, the GPU's fixed-point span stepping (the exact edge test stands for
it), the mask bit on VRAM copies and fills, and interlace. The mask bit of polygons and rectangles IS modelled (GP0 E6: bit 0 sets
bit 15 of every pixel drawn, bit 1 leaves a pixel whose bit 15 is set untouched - the game's shadow packets are each
wrapped in E6 3 / E6 0, so a pixel two shadow quads cover is darkened once). PSXGPU_MASK=off: ignored. A frame from here is the original's primitives drawn by OUR rasteriser, so it
is right about WHAT is drawn where and with which texels, and at polygon edges up to the span stepping.

Nothing is stored in the repository: the words and the VRAM are read from the player's own captures.
"""
import argparse
import os
import struct
import sys
import zlib

W, H = 1024, 512
OWN_BEFORE, OWN_SPRITE, OWN_FILL = 0, 0xFFFE, 0xFFFD  # pixel owners besides a polygon's prim row


def write_png(path, w, h, rgb):
    raw = b''.join(b'\x00' + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def read_png(path):
    """8-bit RGB / RGBA PNG -> (w, h, [r,g,b per pixel]). Enough for our own shots and this tool's."""
    data = open(path, 'rb').read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', path
    pos, idat, w = 8, b'', 0
    while pos < len(data):
        n, t = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if t == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', body[:10])
            assert depth == 8 and ctype in (2, 6), 'unsupported PNG'
            bpp = 3 if ctype == 2 else 4
        elif t == b'IDAT':
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = w * bpp
    out = bytearray(w * h * 3)
    prev = bytearray(stride)
    i = 0
    for y in range(h):
        f = raw[i]
        line = bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 255
        for x in range(w):
            out[(y * w + x) * 3:(y * w + x) * 3 + 3] = line[x * bpp:x * bpp + 3]
        prev = line
    return w, h, out


def cmd_compare(args):
    wa, ha, a = read_png(args.a)
    wb, hb, b = read_png(args.b)
    if (wa, ha) != (wb, hb):
        print('sizes differ: %dx%d vs %dx%d' % (wa, ha, wb, hb))
        return 1
    def stats(y0, y1):
        n = err = close = 0
        for y in range(y0, y1):
            for x in range(wa):
                o = (y * wa + x) * 3
                d = max(abs(a[o] - b[o]), abs(a[o + 1] - b[o + 1]), abs(a[o + 2] - b[o + 2]))
                err += (abs(a[o] - b[o]) + abs(a[o + 1] - b[o + 1]) + abs(a[o + 2] - b[o + 2])) / 3.0
                close += d <= args.close
                n += 1
        return err / n, 100.0 * close / n
    for name, y0, y1 in (('whole frame', 0, ha), ('lower third (the near road)', ha * 2 // 3, ha),
                         ('below the horizon row %d' % args.horizon, args.horizon, ha)):
        mae, pct = stats(y0, y1)
        print('%-32s rows %3d..%3d: mean |diff| %6.2f of 255, %5.1f%% of pixels within %d' %
              (name, y0, y1 - 1, mae, pct, args.close))


def read_words(path):
    data = open(path, 'rb').read()
    out = []
    for i in range(0, len(data) - 23, 24):
        seq, pc, value, address, port, source = struct.unpack_from('<QIIIBB', data, i)
        out.append((seq, port, value, address))
    return out


def split_frames(words):
    """A frame is every GP0 word up to and including the batch that precedes a GP1(05h) swap."""
    frames, cur = [], []
    for seq, port, value, address in words:
        if port == 1:
            if (value >> 24) == 0x05:
                frames.append(cur)
                cur = []
            continue
        cur.append((seq, value, address))
    if cur:
        frames.append(cur)
    return frames


class Gpu:
    def __init__(self, vram):
        self.v = list(struct.unpack('<%dH' % (W * H), vram))
        self.tpx = self.tpy = 0
        self.tdepth = 0
        self.semi = 0
        self.twin = (0, 0, 0, 0)
        self.twin_word = 0xE2000000
        self.x1 = self.y1 = 0
        self.x2, self.y2 = 1023, 511
        self.ox = self.oy = 0
        self.mask_set = self.mask_check = 0
        self.mask_on = os.environ.get('PSXGPU_MASK', '') != 'off'
        self.skipped = {}
        self.drawn = []   # (cmd, verts, tpage, clut, srcAddr)
        self.area = None  # the drawing area the frame's first polygon was drawn into
        self.skip_tpages = set()  # polygons / sprites on these texture pages are not drawn
        self.skip_untextured = False
        self.skip_cmds = set()
        self.skip_sprites = False
        self.owner = [0] * (W * H)  # per VRAM pixel: the packet that wrote it last (OWN_* or 1-based prim row)
        self.cur = 0

    # ------------------------------------------------------------------ texels
    def texel(self, u, v, clutx, cluty):
        mx, my, ofx, ofy = self.twin
        u &= 0xFF
        v &= 0xFF
        u = (u & ~(mx * 8)) | ((ofx & mx) * 8)
        v = (v & ~(my * 8)) | ((ofy & my) * 8)
        if self.tdepth == 0:
            p = self.v[(self.tpy + v) % H * W + (self.tpx + (u >> 2)) % W]
            idx = (p >> ((u & 3) * 4)) & 0xF
            return self.v[cluty * W + (clutx + idx) % W]
        if self.tdepth == 1:
            p = self.v[(self.tpy + v) % H * W + (self.tpx + (u >> 1)) % W]
            idx = (p >> ((u & 1) * 8)) & 0xFF
            return self.v[cluty * W + (clutx + idx) % W]
        return self.v[(self.tpy + v) % H * W + (self.tpx + u) % W]

    def plot(self, x, y, c15, semi):
        if x < self.x1 or x > self.x2 or y < self.y1 or y > self.y2:
            return
        i = y * W + x
        if self.mask_check and self.v[i] & 0x8000:
            return
        if semi and (c15 & 0x8000 or semi == 2):
            b = self.v[i]
            br, bg, bb = b & 31, (b >> 5) & 31, (b >> 10) & 31
            fr, fg, fb = c15 & 31, (c15 >> 5) & 31, (c15 >> 10) & 31
            m = self.semi
            if m == 0:
                r, g, bl = (br + fr) >> 1, (bg + fg) >> 1, (bb + fb) >> 1
            elif m == 1:
                r, g, bl = min(31, br + fr), min(31, bg + fg), min(31, bb + fb)
            elif m == 2:
                r, g, bl = max(0, br - fr), max(0, bg - fg), max(0, bb - fb)
            else:
                r, g, bl = min(31, br + (fr >> 2)), min(31, bg + (fg >> 2)), min(31, bb + (fb >> 2))
            c15 = (c15 & 0x8000) | r | (g << 5) | (bl << 10)
        self.v[i] = c15 | (0x8000 if self.mask_set else 0)
        self.owner[i] = self.cur

    # ------------------------------------------------------------------ polygons
    def triangle(self, p, tex, gour, raw, semi, clut):
        (x0, y0, c0, t0), (x1, y1, c1, t1), (x2, y2, c2, t2) = p
        area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
        if area == 0:
            return
        minx, maxx = max(min(x0, x1, x2), self.x1), min(max(x0, x1, x2), self.x2)
        miny, maxy = max(min(y0, y1, y2), self.y1), min(max(y0, y1, y2), self.y2)
        if minx > maxx or miny > maxy:
            return
        # the hardware refuses polygons wider than 1023 or taller than 511
        if max(x0, x1, x2) - min(x0, x1, x2) >= 1024 or max(y0, y1, y2) - min(y0, y1, y2) >= 512:
            return
        clutx, cluty = (clut & 0x3F) * 16, (clut >> 6) & 0x1FF
        inv = 1.0 / area
        for y in range(miny, maxy + 1):
            py = y
            for x in range(minx, maxx + 1):
                w0 = (x1 - x) * (y2 - py) - (x2 - x) * (y1 - py)
                w1 = (x2 - x) * (y0 - py) - (x0 - x) * (y2 - py)
                w2 = (x0 - x) * (y1 - py) - (x1 - x) * (y0 - py)
                if area > 0:
                    if w0 < 0 or w1 < 0 or w2 < 0:
                        continue
                    # top-left rule: exclude the right and bottom edges
                    if (w0 == 0 and not self._tl(x1, y1, x2, y2)) or (w1 == 0 and not self._tl(x2, y2, x0, y0)) \
                            or (w2 == 0 and not self._tl(x0, y0, x1, y1)):
                        continue
                else:
                    if w0 > 0 or w1 > 0 or w2 > 0:
                        continue
                    if (w0 == 0 and not self._tl(x2, y2, x1, y1)) or (w1 == 0 and not self._tl(x0, y0, x2, y2)) \
                            or (w2 == 0 and not self._tl(x1, y1, x0, y0)):
                        continue
                a, b, c = w0 * inv, w1 * inv, w2 * inv
                if gour:
                    col = [int(a * c0[k] + b * c1[k] + c * c2[k]) for k in range(3)]
                else:
                    col = c0
                if tex:
                    u = int(a * t0[0] + b * t1[0] + c * t2[0])
                    v = int(a * t0[1] + b * t1[1] + c * t2[1])
                    t = self.texel(u, v, clutx, cluty)
                    if t == 0:
                        continue
                    if raw:
                        out = t
                    else:
                        r = min(31, ((t & 31) * col[0]) >> 7)
                        g = min(31, (((t >> 5) & 31) * col[1]) >> 7)
                        bl = min(31, (((t >> 10) & 31) * col[2]) >> 7)
                        out = (t & 0x8000) | r | (g << 5) | (bl << 10)
                    self.plot(x, y, out, 1 if (semi and (t & 0x8000)) else 0)
                else:
                    out = (col[0] >> 3) | ((col[1] >> 3) << 5) | ((col[2] >> 3) << 10)
                    self.plot(x, y, out, 2 if semi else 0)

    # The fill rule for a sample point exactly on an edge. The console covers pixel (x, y)
    # when the integer point (x, y) is inside the triangle or on its TOP or LEFT edge: a polygon is drawn up to,
    # excluding, its right-most column and bottom-most row (psx-spx "GPU Render Polygon Commands"), so two polygons that
    # share an edge never both draw it. Edge a->b is taken with the interior on its right-hand side on the y-down screen
    # (the sign `area > 0` below; the other winding calls this with the edge reversed). PSXGPU_EDGE=br: right and
    # bottom edges (mirrored), PSXGPU_EDGE=lb: left and bottom (what GL rasterisation gives with the product's
    # half-pixel shift) - both controls.
    EDGE = os.environ.get('PSXGPU_EDGE', 'tl')

    @staticmethod
    def _tl(ax, ay, bx, by):
        if Gpu.EDGE == 'br':
            return (ay == by and bx < ax) or (by > ay)
        if Gpu.EDGE == 'lb':
            return (ay == by and bx < ax) or (by < ay)
        return (ay == by and bx > ax) or (by < ay)

    def line(self, a, b, gour, semi):
        """Both end points included; the steps of the longer axis, the other rounded (psx-spx does not give the
        hardware's exact stepping - an approximation for the slanted ones; horizontal / vertical ones are exact)."""
        (x0, y0, c0), (x1, y1, c1) = a, b
        if abs(x1 - x0) >= 1024 or abs(y1 - y0) >= 512:
            return
        steps = max(abs(x1 - x0), abs(y1 - y0))
        for k in range(steps + 1):
            x = x0 + (round((x1 - x0) * k / steps) if steps else 0)
            y = y0 + (round((y1 - y0) * k / steps) if steps else 0)
            c = c0
            if gour and steps:
                c = 0
                for sh in (0, 8, 16):
                    c |= (((c0 >> sh) & 255) + (((c1 >> sh) & 255) - ((c0 >> sh) & 255)) * k // steps) << sh
            self.plot(x, y, ((c & 0xFF) >> 3) | ((((c >> 8) & 0xFF) >> 3) << 5) | ((((c >> 16) & 0xFF) >> 3) << 10),
                      2 if semi else 0)

    def polygon(self, cmd, words, addr):
        quad = bool(cmd & 8)
        tex = bool(cmd & 4)
        gour = bool(cmd & 0x10)
        semi = bool(cmd & 2)
        raw = bool(cmd & 1)
        n = 4 if quad else 3
        i = 0
        colour = words[0] & 0xFFFFFF
        verts = []
        clut = tpage = None
        for k in range(n):
            if k == 0:
                c = colour
                i = 1
            elif gour:
                c = words[i] & 0xFFFFFF
                i += 1
            else:
                c = colour
            xy = words[i]
            i += 1
            x = (xy & 0x7FF) - (0x800 if xy & 0x400 else 0)
            y = ((xy >> 16) & 0x7FF) - (0x800 if (xy >> 16) & 0x400 else 0)
            uv = (0, 0)
            if tex:
                t = words[i]
                i += 1
                uv = (t & 0xFF, (t >> 8) & 0xFF)
                if k == 0:
                    clut = t >> 16
                if k == 1:
                    tpage = t >> 16
            verts.append((x + self.ox, y + self.oy, (c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF), uv))
        if tex:
            self.tpx = (tpage & 0xF) * 64
            self.tpy = ((tpage >> 4) & 1) * 256
            self.semi = (tpage >> 5) & 3
            self.tdepth = (tpage >> 7) & 3
        if self.area is None:
            self.area = (self.x1, self.y1, self.x2, self.y2)
        if (tex and tpage in self.skip_tpages) or (not tex and self.skip_untextured and cmd in self.skip_cmds):
            return
        # recorded in the frame's own screen coordinates: the drawing offset taken back out
        self.drawn.append((cmd, [(x - self.ox, y - self.oy, c, t) for x, y, c, t in verts], tpage, clut, addr,
                           self.twin_word))
        self.cur = len(self.drawn)
        self.triangle(verts[0:3], tex, gour, raw, semi, clut or 0)
        if quad:
            self.triangle(verts[1:4], tex, gour, raw, semi, clut or 0)

    def rect(self, cmd, words):
        size = (cmd >> 3) & 3
        tex = bool(cmd & 4)
        semi = bool(cmd & 2)
        raw = bool(cmd & 1)
        colour = words[0] & 0xFFFFFF
        xy = words[1]
        x = (xy & 0x7FF) - (0x800 if xy & 0x400 else 0)
        y = ((xy >> 16) & 0x7FF) - (0x800 if (xy >> 16) & 0x400 else 0)
        i = 2
        u0 = v0 = 0
        clut = 0
        if tex:
            t = words[i]
            i += 1
            u0, v0, clut = t & 0xFF, (t >> 8) & 0xFF, t >> 16
        if size == 0:
            w, h = words[i] & 0x3FF, (words[i] >> 16) & 0x1FF
        else:
            w = h = (1, 8, 16)[size - 1]
        x += self.ox
        y += self.oy
        page = (self.tpx // 64) | ((self.tpy // 256) << 4) | (self.tdepth << 7) | (self.semi << 5)
        if tex and page in self.skip_tpages:
            return
        self.cur = OWN_SPRITE
        clutx, cluty = (clut & 0x3F) * 16, (clut >> 6) & 0x1FF
        col = (colour & 0xFF, (colour >> 8) & 0xFF, (colour >> 16) & 0xFF)
        for yy in range(h):
            for xx in range(w):
                if tex:
                    t = self.texel(u0 + xx, v0 + yy, clutx, cluty)
                    if t == 0:
                        continue
                    if not raw:
                        r = min(31, ((t & 31) * col[0]) >> 7)
                        g = min(31, (((t >> 5) & 31) * col[1]) >> 7)
                        bl = min(31, (((t >> 10) & 31) * col[2]) >> 7)
                        t = (t & 0x8000) | r | (g << 5) | (bl << 10)
                    self.plot(x + xx, y + yy, t, 1 if (semi and t & 0x8000) else 0)
                else:
                    self.plot(x + xx, y + yy, (col[0] >> 3) | ((col[1] >> 3) << 5) | ((col[2] >> 3) << 10),
                              2 if semi else 0)

    # ------------------------------------------------------------------ the command stream
    def run(self, frame):
        i = 0
        n = len(frame)
        while i < n:
            seq, word, addr = frame[i]
            cmd = word >> 24
            if 0x20 <= cmd < 0x40:
                quad = bool(cmd & 8)
                tex = bool(cmd & 4)
                gour = bool(cmd & 0x10)
                nv = 4 if quad else 3
                length = 1 + nv * (1 + (1 if tex else 0)) + ((nv - 1) if gour else 0)
                self.polygon(cmd, [w for _, w, _ in frame[i:i + length]], addr)
                i += length
            elif 0x60 <= cmd < 0x80:
                size = (cmd >> 3) & 3
                length = 2 + (1 if cmd & 4 else 0) + (1 if size == 0 else 0)
                if not self.skip_sprites:
                    self.rect(cmd, [w for _, w, _ in frame[i:i + length]])
                i += length
            elif 0x40 <= cmd < 0x60:
                # a line: both end points drawn (psx-spx "GPU Render Line Commands": unlike a polygon's, the lower-right
                # end is not excluded); a poly-line's words run to the 0x5xxx5xxx terminator
                gour, poly = bool(cmd & 0x10), bool(cmd & 8)
                col = word & 0xFFFFFF
                pts = []
                j = i + 1
                while j < n:
                    if pts and gour:   # a Gouraud line's next colour word (a poly-line's terminator in its place)
                        w = frame[j][1]
                        j += 1
                        if poly and len(pts) >= 2 and (w & 0xF000F000) == 0x50005000:
                            break
                        col = w & 0xFFFFFF
                        if j >= n:
                            break
                    w = frame[j][1]
                    j += 1
                    if poly and not gour and len(pts) >= 2 and (w & 0xF000F000) == 0x50005000:
                        break
                    pts.append(((w & 0x7FF) - (0x800 if w & 0x400 else 0) + self.ox,
                                ((w >> 16) & 0x7FF) - (0x800 if (w >> 16) & 0x400 else 0) + self.oy, col))
                    if not poly and len(pts) == 2:
                        break
                self.skipped['line'] = self.skipped.get('line', 0) + 1
                self.cur = OWN_SPRITE
                for k in range(1, len(pts)):
                    self.line(pts[k - 1], pts[k], gour, bool(cmd & 2))
                i = j
            elif cmd == 0x02:
                col = word & 0xFFFFFF
                xy, wh = frame[i + 1][1], frame[i + 2][1]
                x0, y0 = xy & 0x3F0, (xy >> 16) & 0x1FF
                w, h = ((wh & 0x3FF) + 15) & ~15, (wh >> 16) & 0x1FF
                c15 = ((col & 0xFF) >> 3) | ((((col >> 8) & 0xFF) >> 3) << 5) | ((((col >> 16) & 0xFF) >> 3) << 10)
                for yy in range(h):
                    for xx in range(w):
                        self.v[((y0 + yy) % H) * W + (x0 + xx) % W] = c15
                        self.owner[((y0 + yy) % H) * W + (x0 + xx) % W] = OWN_FILL
                i += 3
            elif cmd == 0x80:
                s, d, wh = frame[i + 1][1], frame[i + 2][1], frame[i + 3][1]
                sx, sy, dx, dy = s & 0x3FF, (s >> 16) & 0x1FF, d & 0x3FF, (d >> 16) & 0x1FF
                w, h = wh & 0xFFFF or 0x400, (wh >> 16) or 0x200
                block = [[self.v[((sy + yy) % H) * W + (sx + xx) % W] for xx in range(w)] for yy in range(h)]
                for yy in range(h):
                    for xx in range(w):
                        self.v[((dy + yy) % H) * W + (dx + xx) % W] = block[yy][xx]
                i += 4
            elif cmd == 0xA0:
                d, wh = frame[i + 1][1], frame[i + 2][1]
                dx, dy, w, h = d & 0x3FF, (d >> 16) & 0x1FF, wh & 0xFFFF, wh >> 16
                count = (w * h + 1) // 2
                data = [frame[i + 3 + k][1] for k in range(count)]
                for k in range(w * h):
                    hw = (data[k >> 1] >> (16 * (k & 1))) & 0xFFFF
                    self.v[((dy + k // w) % H) * W + (dx + k % w) % W] = hw
                i += 3 + count
            elif cmd == 0xE1:
                self.tpx = (word & 0xF) * 64
                self.tpy = ((word >> 4) & 1) * 256
                self.semi = (word >> 5) & 3
                self.tdepth = (word >> 7) & 3
                i += 1
            elif cmd == 0xE2:
                self.twin = (word & 31, (word >> 5) & 31, (word >> 10) & 31, (word >> 15) & 31)
                self.twin_word = word
                i += 1
            elif cmd == 0xE3:
                self.x1, self.y1 = word & 0x3FF, (word >> 10) & 0x1FF
                i += 1
            elif cmd == 0xE4:
                self.x2, self.y2 = word & 0x3FF, (word >> 10) & 0x1FF
                i += 1
            elif cmd == 0xE5:
                x = word & 0x7FF
                y = (word >> 11) & 0x7FF
                self.ox = x - 0x800 if x & 0x400 else x
                self.oy = y - 0x800 if y & 0x400 else y
                i += 1
            elif cmd == 0xE6:
                if self.mask_on:
                    self.mask_set, self.mask_check = word & 1, (word >> 1) & 1
                i += 1
            else:
                if cmd not in (0x00, 0x01):
                    self.skipped[hex(cmd)] = self.skipped.get(hex(cmd), 0) + 1
                i += 1


def cmd_frame(args):
    words = read_words(args.gpu)
    frames = split_frames(words)
    print('frames in the stream: %d (%s words)' % (len(frames), [len(f) for f in frames]))
    gpu = Gpu(open(args.vram, 'rb').read())
    if args.gpujson:
        # the drawing area / offset the capture's GPU held when it was saved: a frame the snapshot was
        # already building (frame 0 of a trace) sends no E3..E5 of its own
        import json
        g = json.load(open(args.gpujson))
        a, o = g['drawing_area'], g['drawing_offset']
        gpu.x1, gpu.y1, gpu.x2, gpu.y2 = a['left'], a['top'], a['right'], a['bottom']
        gpu.ox, gpu.oy = o['x'], o['y']
    for f in frames[:args.frame]:
        gpu.run(f)
    gpu.drawn = []
    gpu.area = None
    if args.skip_tpage:
        gpu.skip_tpages = {int(t, 16) for t in args.skip_tpage.split(',')}
    gpu.skip_sprites = args.no_sprites
    if args.skip_cmd:
        gpu.skip_untextured = True
        gpu.skip_cmds = {int(t, 16) for t in args.skip_cmd.split(',')}
    x1, y1, x2, y2 = gpu.x1, gpu.y1, gpu.x2, gpu.y2
    if args.clear:
        # paint the target buffer magenta first, so what this frame did not draw is visible
        for y in range(y1, y2 + 1):
            for x in range(x1, x2 + 1):
                gpu.v[y * W + x] = 0x7C1F
    gpu.run(frames[args.frame])
    if gpu.area:
        x1, y1, x2, y2 = gpu.area
    w, h = x2 - x1 + 1, y2 - y1 + 1
    rgb = bytearray(w * h * 3)
    for y in range(h):
        for x in range(w):
            p = gpu.v[(y1 + y) * W + x1 + x]
            o = (y * w + x) * 3
            rgb[o] = (p & 31) << 3
            rgb[o + 1] = ((p >> 5) & 31) << 3
            rgb[o + 2] = ((p >> 10) & 31) << 3
    write_png(args.out, w, h, rgb)
    if args.owners:
        with open(args.owners, 'wb') as f:
            f.write(struct.pack('<HH', w, h))
            f.write(struct.pack('<%dH' % (w * h), *[gpu.owner[(y1 + y) * W + x1 + x] for y in range(h) for x in range(w)]))
    print('frame %d: draw area (%d,%d)-(%d,%d), %d polygons drawn, skipped %s -> %s' %
          (args.frame, x1, y1, x2, y2, len(gpu.drawn), gpu.skipped, args.out))
    if args.prims:
        with open(args.prims, 'w') as f:
            f.write('cmd,tpage,clut,srcAddr,x0,y0,u0,v0,r0,g0,b0,x1,y1,u1,v1,r1,g1,b1,x2,y2,u2,v2,r2,g2,b2,'
                    'x3,y3,u3,v3,r3,g3,b3,e2,nverts\n')
            for cmd, verts, tpage, clut, addr, e2 in gpu.drawn:
                row = ['0x%02X' % cmd, '' if tpage is None else '0x%04X' % tpage,
                       '' if clut is None else '0x%04X' % clut, '0x%08X' % addr]
                for x, y, c, (u, v) in verts:
                    row += [str(x), str(y), str(u), str(v), str(c[0]), str(c[1]), str(c[2])]
                # a triangle's fourth vertex is left empty; then the texture window it was drawn under
                row += [''] * (7 * (4 - len(verts)))
                row += ['0x%08X' % e2, str(len(verts))]
                f.write(','.join(row) + '\n')
        print('wrote %s' % args.prims)


# ---------------------------------------------------------------------------- whole-frame parity
# What drew a pixel, named from the packet: the texture page
# the polygon's tpage selects and its command. Names are what the traced frames showed on those pages.
PAGE_NAMES = {0x07: 'cells (RACE*.STP page 7)', 0x08: 'cells (RACE*.STP page 8)', 0x0A: 'cells (RACE*.STP page 10)',
              0x0D: 'road surface (GAMEBIN1 road page)', 0x0E: 'objects (G_OBJ01.GTP)', 0x0B: 'props / clouds (page 11)',
              0x0F: 'HUD (DASH1P)', 0x1A: 'machines and riders (BBLEVEL page 26)',
              0x1B: 'machines and riders (BBLEVEL page 27)', 0x1C: 'machines and riders (BBLEVEL page 28)',
              0x16: 'panorama (page 22)', 0x18: 'panorama (page 24)', 0x0C: 'page 12', 0x19: 'page 25'}


def owner_category(owner, rows):
    if owner == OWN_BEFORE:
        return 'drawn before the frame (sky left in VRAM)'
    if owner == OWN_SPRITE:
        return 'sprites (HUD text, effects)'
    if owner == OWN_FILL:
        return 'fill (clear)'
    cmd, tpage = rows[owner - 1]
    if not cmd & 4:
        if cmd & 2:
            return 'untextured semi-transparent (shadow)'
        return 'untextured (sky gradient, lines)'
    if cmd == 0x2F:
        return 'textured semi raw (clouds / sparks)'
    page = tpage & 0x1F
    return PAGE_NAMES.get(page, 'page %d' % page)


def cmd_parity(args):
    import csv
    wa, ha, a = read_png(args.orig)
    wb, hb, b = read_png(args.ours)
    if (wa, ha) != (wb, hb):
        print('sizes differ: %dx%d vs %dx%d' % (wa, ha, wb, hb))
        print('parity verdict FAIL')
        return 1
    rows = []
    for r in csv.DictReader(open(args.prims)):
        rows.append((int(r['cmd'], 16), int(r['tpage'], 16) if r['tpage'] else 0))
    data = open(args.owners, 'rb').read()
    ow, oh = struct.unpack_from('<HH', data, 0)
    assert (ow, oh) == (wa, ha), 'owner map size differs'
    owners = struct.unpack_from('<%dH' % (ow * oh), data, 4)
    cats = {}
    prims = {}  # prim row -> [pixels, orig rgb sums, ours rgb sums]: the per-primitive mean-colour match
    tot_err = tot_close = 0
    crop_n = crop_close = 0
    crop_err = 0.0
    for y in range(ha):
        for x in range(wa):
            i = y * wa + x
            o = i * 3
            d = max(abs(a[o] - b[o]), abs(a[o + 1] - b[o + 1]), abs(a[o + 2] - b[o + 2]))
            e = (abs(a[o] - b[o]) + abs(a[o + 1] - b[o + 1]) + abs(a[o + 2] - b[o + 2])) / 3.0
            c = owner_category(owners[i], rows)
            if 0 < owners[i] < OWN_FILL:
                pr = prims.setdefault(owners[i], [0, 0, 0, 0, 0, 0, 0])
                pr[0] += 1
                for k in range(3):
                    pr[1 + k] += a[o + k]
                    pr[4 + k] += b[o + k]
            st = cats.setdefault(c, [0, 0.0, 0])
            st[0] += 1
            st[1] += e
            st[2] += d <= args.close
            tot_err += e
            tot_close += d <= args.close
            # the part a television shows (every race capture's GPU state: x 9..373, y 8..231)
            if 9 <= x < 374 and 8 <= y < 232:
                crop_n += 1
                crop_err += e
                crop_close += d <= args.close
    n = wa * ha
    print('whole-frame parity: %s against %s (%dx%d), a pixel is close within %d on every channel' %
          (args.ours, args.orig, wa, ha, args.close))
    print('  whole frame              mean |diff| %6.2f   close %5.1f%%' % (tot_err / n, 100.0 * tot_close / n))
    print('  shown part (9..373, 8..231) mean |diff| %6.2f   close %5.1f%%' % (crop_err / crop_n, 100.0 * crop_close / crop_n))
    # A primitive of the original matches when the mean colour of ours over the pixels it covers is within
    # --prim-tol of its own on every channel: robust to the sub-pixel texel shifts a per-pixel test counts,
    # and it still sees a wrong palette, a missing object or a wrong shade.
    pcat = {}
    for row, pr in prims.items():
        if pr[0] < args.prim_min_pixels:
            continue
        ok = max(abs(pr[1 + k] - pr[4 + k]) for k in range(3)) <= args.prim_tol * pr[0]
        t = pcat.setdefault(owner_category(row, rows), [0, 0])
        t[0] += 1
        t[1] += ok
    pn = sum(t[0] for t in pcat.values())
    pm = sum(t[1] for t in pcat.values())
    print('  primitives of %d+ pixels whose mean colour matches within %d: %d of %d (%.1f%%)' %
          (args.prim_min_pixels, args.prim_tol, pm, pn, 100.0 * pm / max(1, pn)))
    print('  by what drew the ORIGINAL pixel (pixels, share of frame, mean |diff|, close, pixels NOT close, prims matched):')
    for c, (cn, ce, cc) in sorted(cats.items(), key=lambda kv: -(kv[1][0] - kv[1][2])):
        t = pcat.get(c, [0, 0])
        print('    %-44s %6d %5.1f%%  %6.2f  %5.1f%%  %6d  %4d/%-4d' % (c, cn, 100.0 * cn / n, ce / cn, 100.0 * cc / cn,
                                                                   cn - cc, t[1], t[0]))
    if args.diff:
        # three panels side by side: the original, ours, and the difference (x4) where it is not close
        out = bytearray(wa * 3 * ha * 3)
        for y in range(ha):
            for x in range(wa):
                o = (y * wa + x) * 3
                for panel, src in ((0, a), (1, b)):
                    p = (y * wa * 3 + panel * wa + x) * 3
                    out[p:p + 3] = bytes(src[o:o + 3])
                d = [min(255, 4 * abs(a[o + k] - b[o + k])) for k in range(3)]
                p = (y * wa * 3 + 2 * wa + x) * 3
                out[p:p + 3] = bytes(d)
        write_png(args.diff, wa * 3, ha, out)
        print('  picture: %s (original | ours | |diff| x4)' % args.diff)
    share = 100.0 * tot_close / n
    fails = []
    if args.min_close is not None and share < args.min_close:
        fails.append('close %.1f%% < %.1f%%' % (share, args.min_close))
    if args.min_prims is not None and 100.0 * pm / max(1, pn) < args.min_prims:
        fails.append('primitives matched %.1f%% < %.1f%%' % (100.0 * pm / max(1, pn), args.min_prims))
    for spec in args.min_category or []:
        name, pct = spec.rsplit(':', 1)
        hit = [(c, v) for c, v in cats.items() if name in c]
        cn = sum(v[0] for _, v in hit)
        cc = sum(v[2] for _, v in hit)
        got = 100.0 * cc / cn if cn else 0.0
        print('  category "%s": %d pixels, close %.1f%% (threshold %s%%)' % (name, cn, got, pct))
        if got < float(pct):
            fails.append('"%s" close %.1f%% < %s%%' % (name, got, pct))
    if args.min_close is not None or args.min_prims is not None or args.min_category:
        print('parity verdict %s%s' % ('PASS' if not fails else 'FAIL', (' (' + '; '.join(fails) + ')') if fails else ''))
        return 0 if not fails else 1
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    f = sub.add_parser('frame')
    f.add_argument('--gpu', required=True)
    f.add_argument('--vram', required=True)
    f.add_argument('--frame', type=int, default=0)
    f.add_argument('--out', required=True)
    f.add_argument('--prims')
    f.add_argument('--clear', action='store_true', help='paint the target buffer magenta first')
    f.add_argument('--gpujson', help="the capture's gpu.json: start from its drawing area and offset")
    f.add_argument('--owners', help='write the packet that drew each pixel last (u16 w, h, then w*h u16; '
                                    'prim row 1.., 0xFFFE sprite, 0xFFFD fill, 0 drawn before the frame)')
    f.add_argument('--skip-tpage', help='comma list of hex tpage words whose polygons are not drawn (the HUD)')
    f.add_argument('--no-sprites', action='store_true', help='do not draw GP0 60..7F (the HUD is made of them)')
    f.add_argument('--skip-cmd', help='comma list of hex untextured polygon commands not drawn')
    q = sub.add_parser('parity', help="whole-frame parity: the original's frame against ours, by what drew each pixel")
    q.add_argument('orig')
    q.add_argument('ours')
    q.add_argument('--prims', required=True, help="the original's --prims csv")
    q.add_argument('--owners', required=True, help="the original's --owners map")
    q.add_argument('--close', type=int, default=24)
    q.add_argument('--diff', help='write a difference picture: the original | ours | |diff| x4 over the owner colours')
    q.add_argument('--min-close', type=float, help='verdict PASS when the share of close pixels is at least this')
    q.add_argument('--min-prims', type=float, help='... and the share of matching primitives at least this')
    q.add_argument('--min-category', action='append', help='NAME:PCT - the close share of the categories whose '
                                                           'name contains NAME at least PCT (repeatable)')
    q.add_argument('--prim-tol', type=int, default=20, help='a primitive matches when its mean colour is this close')
    q.add_argument('--prim-min-pixels', type=int, default=8, help='primitives smaller than this are not judged')
    c = sub.add_parser('compare', help='per-pixel difference of two same-size PNG frames')
    c.add_argument('a')
    c.add_argument('b')
    c.add_argument('--close', type=int, default=24, help='a pixel within this on every channel is "close"')
    c.add_argument('--horizon', type=int, default=88)
    args = ap.parse_args()
    if args.cmd == 'frame':
        cmd_frame(args)
    elif args.cmd == 'compare':
        return cmd_compare(args)
    elif args.cmd == 'parity':
        return cmd_parity(args)


if __name__ == '__main__':
    sys.exit(main())
